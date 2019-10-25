#include "mpvdeclarativeobject.h"
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <QX11Info>
#include <QGuiApplication>
#endif

namespace {

void wakeup(void *ctx) {
    // This callback is invoked from any mpv thread (but possibly also
    // recursively from a thread that is calling the mpv API). Just notify
    // the Qt GUI thread to wake up (so that it can process events with
    // mpv_wait_event()), and return as quickly as possible.
    QMetaObject::invokeMethod(static_cast<MpvDeclarativeObject *>(ctx),
                              "hasMpvEvents", Qt::QueuedConnection);
}

void on_mpv_redraw(void *ctx) { MpvDeclarativeObject::on_update(ctx); }

void *get_proc_address_mpv(void *ctx, const char *name) {
    Q_UNUSED(ctx)
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (glctx == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}

} // namespace

class MpvRenderer : public QQuickFramebufferObject::Renderer {
    Q_DISABLE_COPY_MOVE(MpvRenderer)

public:
    MpvRenderer(MpvDeclarativeObject *mpvDeclarativeObject)
        : m_mpvDeclarativeObject(mpvDeclarativeObject) {
        Q_ASSERT(m_mpvDeclarativeObject != nullptr);
    }
    ~MpvRenderer() override = default;

    // This function is called when a new FBO is needed.
    // This happens on the initial frame.
    QOpenGLFramebufferObject *
    createFramebufferObject(const QSize &size) override {
        // init mpv_gl:
        if (m_mpvDeclarativeObject->mpv_gl == nullptr) {
            mpv_opengl_init_params gl_init_params{get_proc_address_mpv, nullptr,
                                                  nullptr};
            mpv_render_param params[]{
                {MPV_RENDER_PARAM_API_TYPE,
                 const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                {MPV_RENDER_PARAM_INVALID, nullptr},
                {MPV_RENDER_PARAM_INVALID, nullptr}};
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
            if (QGuiApplication::platformName().contains("xcb")) {
                params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
                params[2].data = QX11Info::display();
            }
#endif

            const int mpvGLInitResult =
                mpv_render_context_create(&m_mpvDeclarativeObject->mpv_gl,
                                          m_mpvDeclarativeObject->mpv, params);
            Q_ASSERT(mpvGLInitResult >= 0);
            mpv_render_context_set_update_callback(
                m_mpvDeclarativeObject->mpv_gl, on_mpv_redraw,
                m_mpvDeclarativeObject);

            QMetaObject::invokeMethod(m_mpvDeclarativeObject, "initFinished");
        }

        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void render() override {
        m_mpvDeclarativeObject->window()->resetOpenGLState();

        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo mpfbo{0, 0, 0, 0};
        mpfbo.fbo = static_cast<int>(fbo->handle());
        mpfbo.w = fbo->width();
        mpfbo.h = fbo->height();
        int flip_y{0};

        mpv_render_param params[] = {
            // Specify the default framebuffer (0) as target. This will
            // render onto the entire screen. If you want to show the video
            // in a smaller rectangle or apply fancy transformations, you'll
            // need to render into a separate FBO and draw it manually.
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
            // Flip rendering (needed due to flipped GL coordinate system).
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}};
        // See render_gl.h on what OpenGL environment mpv expects, and
        // other API details.
        mpv_render_context_render(m_mpvDeclarativeObject->mpv_gl, params);

        m_mpvDeclarativeObject->window()->resetOpenGLState();
    }

private:
    MpvDeclarativeObject *m_mpvDeclarativeObject = nullptr;
};

MpvDeclarativeObject::MpvDeclarativeObject(QQuickItem *parent)
    : QQuickFramebufferObject(parent),
      mpv(mpv::qt::Handle::FromRawHandle(mpv_create())) {
    Q_ASSERT(mpv != nullptr);

    mpvSetProperty(QLatin1String("input-default-bindings"), false);
    mpvSetProperty(QLatin1String("input-vo-keyboard"), false);
    mpvSetProperty(QLatin1String("input-cursor"), false);
    mpvSetProperty(QLatin1String("cursor-autohide"), false);

    auto iterator = properties.constBegin();
    while (iterator != properties.constEnd()) {
        mpvObserveProperty(QLatin1String(iterator.key()));
        ++iterator;
    }

    // From this point on, the wakeup function will be called. The callback
    // can come from any thread, so we use the QueuedConnection mechanism to
    // relay the wakeup in a thread-safe way.
    connect(this, &MpvDeclarativeObject::hasMpvEvents, this,
            &MpvDeclarativeObject::handleMpvEvents, Qt::QueuedConnection);
    mpv_set_wakeup_callback(mpv, wakeup, this);

    const int mpvInitResult = mpv_initialize(mpv);
    Q_ASSERT(mpvInitResult >= 0);

    connect(this, &MpvDeclarativeObject::onUpdate, this,
            &MpvDeclarativeObject::doUpdate, Qt::QueuedConnection);
}

MpvDeclarativeObject::~MpvDeclarativeObject() {
    // only initialized if something got drawn
    if (mpv_gl != nullptr) {
        mpv_render_context_free(mpv_gl);
    }
    // We don't need to destroy mpv handle in our own because we are using
    // mpv::qt::Handle, which is a shared pointer.
    // mpv_terminate_destroy(mpv);
}

void MpvDeclarativeObject::on_update(void *ctx) {
    Q_EMIT static_cast<MpvDeclarativeObject *>(ctx)->onUpdate();
}

// connected to onUpdate() signal makes sure it runs on the GUI thread
void MpvDeclarativeObject::doUpdate() { update(); }

void MpvDeclarativeObject::processMpvLogMessage(mpv_event_log_message *event) {
    const QString logMessage =
        QStringLiteral("[libmpv] %1: %2")
            .arg(QString::fromUtf8(event->prefix),
                 QString::fromUtf8(event->text).trimmed());
    switch (event->log_level) {
    case MPV_LOG_LEVEL_V:
    case MPV_LOG_LEVEL_DEBUG:
    case MPV_LOG_LEVEL_TRACE:
        qDebug().noquote() << logMessage;
        break;
    case MPV_LOG_LEVEL_WARN:
        qWarning().noquote() << logMessage;
        break;
    case MPV_LOG_LEVEL_ERROR:
        qCritical().noquote() << logMessage;
        break;
    case MPV_LOG_LEVEL_FATAL:
        // qFatal() doesn't support the "<<" operator.
        qFatal("%ls", qUtf16Printable(logMessage));
        break;
    case MPV_LOG_LEVEL_INFO:
        qInfo().noquote() << logMessage;
        break;
    default:
        qDebug().noquote() << logMessage;
        break;
    }
}

void MpvDeclarativeObject::processMpvPropertyChange(mpv_event_property *event) {
    const char *eventName = event->name;
    if (!propertyBlackList.contains(QString::fromUtf8(eventName),
                                    Qt::CaseInsensitive)) {
        qDebug().noquote() << "[libmpv] Property changed from mpv:"
                           << eventName;
    }
    if (properties.contains(eventName)) {
        const auto signalName = properties.value(eventName);
        if (signalName != nullptr) {
            QMetaObject::invokeMethod(this, signalName);
        }
    }
}

bool MpvDeclarativeObject::isLoaded() const {
    return ((mediaStatus() == MediaStatus::Loaded) ||
            (mediaStatus() == MediaStatus::Buffering) ||
            (mediaStatus() == MediaStatus::Buffered));
}

bool MpvDeclarativeObject::isPlaying() const {
    return playbackState() == PlaybackState::Playing;
}

bool MpvDeclarativeObject::isPaused() const {
    return playbackState() == PlaybackState::Paused;
}

bool MpvDeclarativeObject::isStopped() const {
    return playbackState() == PlaybackState::Stopped;
}

void MpvDeclarativeObject::setMediaStatus(
    MpvDeclarativeObject::MediaStatus mediaStatus) {
    if (this->mediaStatus() == mediaStatus) {
        return;
    }
    currentMediaStatus = mediaStatus;
    Q_EMIT mediaStatusChanged();
}

void MpvDeclarativeObject::videoReconfig() { Q_EMIT videoSizeChanged(); }

void MpvDeclarativeObject::audioReconfig() {}

void MpvDeclarativeObject::playbackStateChangeEvent() {
    if (isPlaying()) {
        Q_EMIT playing();
    }
    if (isPaused()) {
        Q_EMIT paused();
    }
    if (isStopped()) {
        Q_EMIT stopped();
    }
    Q_EMIT playbackStateChanged();
}

bool MpvDeclarativeObject::mpvSendCommand(const QVariant &arguments) {
    if (arguments.isNull() || !arguments.isValid()) {
        return false;
    }
    qDebug().noquote() << "Sending a command to mpv:" << arguments;
    int errorCode = 0;
    if (mpvCallType() == MpvCallType::Asynchronous) {
        errorCode = mpv::qt::command_async(mpv, arguments, 0);
    } else {
        errorCode = mpv::qt::get_error(mpv::qt::command(mpv, arguments));
    }
    if (errorCode < 0) {
        qWarning().noquote()
            << "Failed to execute a command for mpv:" << arguments;
    }
    return (errorCode >= 0);
}

bool MpvDeclarativeObject::mpvSetProperty(const QString &name,
                                          const QVariant &value) {
    if (name.isEmpty() || value.isNull() || !value.isValid()) {
        return false;
    }
    qDebug().noquote() << "Setting a property for mpv:" << name
                       << "to:" << value;
    int errorCode = 0;
    if (mpvCallType() == MpvCallType::Asynchronous) {
        errorCode = mpv::qt::set_property_async(mpv, name, value, 0);
    } else {
        errorCode = mpv::qt::get_error(mpv::qt::set_property(mpv, name, value));
    }
    if (errorCode < 0) {
        qWarning().noquote() << "Failed to set a property for mpv:" << name;
    }
    return (errorCode >= 0);
}

QVariant MpvDeclarativeObject::mpvGetProperty(const QString &name,
                                              bool *ok) const {
    if (ok != nullptr) {
        *ok = false;
    }
    if (name.isEmpty()) {
        return QVariant();
    }
    const QVariant result = mpv::qt::get_property(mpv, name);
    if (result.isNull() || !result.isValid()) {
        qWarning().noquote() << "Failed to query a property from mpv:" << name;
    } else {
        if (ok != nullptr) {
            *ok = true;
        }
        /*if ((name != QLatin1String("time-pos")) &&
            (name != QLatin1String("duration"))) {
            qDebug().noquote() << "Querying a property from mpv:"
                               << name << "result:" << result;
        }*/
    }
    return result;
}

bool MpvDeclarativeObject::mpvObserveProperty(const QString &name) {
    if (name.isEmpty()) {
        return false;
    }
    qDebug().noquote() << "Observing a property from mpv:" << name;
    const int errorCode = mpv_observe_property(
        mpv, 0, name.toUtf8().constData(), MPV_FORMAT_NONE);
    if (errorCode < 0) {
        qWarning().noquote()
            << "Failed to observe a property from mpv:" << name;
    }
    return (errorCode >= 0);
}

QQuickFramebufferObject::Renderer *
MpvDeclarativeObject::createRenderer() const {
    window()->setPersistentOpenGLContext(true);
    window()->setPersistentSceneGraph(true);
    return new MpvRenderer(const_cast<MpvDeclarativeObject *>(this));
}

QUrl MpvDeclarativeObject::source() const {
    return isStopped() ? QUrl() : currentSource;
}

QString MpvDeclarativeObject::fileName() const {
    return isStopped() ? QString()
                       : mpvGetProperty(QLatin1String("filename")).toString();
}

QSize MpvDeclarativeObject::videoSize() const {
    if (isStopped()) {
        return QSize();
    }
    QSize size(
        qMax(mpvGetProperty(QLatin1String("video-out-params/dw")).toInt(), 0),
        qMax(mpvGetProperty(QLatin1String("video-out-params/dh")).toInt(), 0));
    const int rotate = videoRotate();
    if ((rotate == 90) || (rotate == 270)) {
        size.transpose();
    }
    return size;
}

MpvDeclarativeObject::PlaybackState
MpvDeclarativeObject::playbackState() const {
    const bool stopped = mpvGetProperty(QLatin1String("idle-active")).toBool();
    const bool paused = mpvGetProperty(QLatin1String("pause")).toBool();
    return stopped ? PlaybackState::Stopped
                   : (paused ? PlaybackState::Paused : PlaybackState::Playing);
}

MpvDeclarativeObject::MediaStatus MpvDeclarativeObject::mediaStatus() const {
    return currentMediaStatus;
}

MpvDeclarativeObject::LogLevel MpvDeclarativeObject::logLevel() const {
    const QString level = mpvGetProperty(QLatin1String("msg-level")).toString();
    if (level.isEmpty() || (level == QLatin1String("no")) ||
        (level == QLatin1String("off"))) {
        return LogLevel::Off;
    }
    const QString actualLevel =
        level.right(level.length() - level.lastIndexOf(QLatin1Char('=')) - 1);
    if (actualLevel.isEmpty() || (actualLevel == QLatin1String("no")) ||
        (actualLevel == QLatin1String("off"))) {
        return LogLevel::Off;
    }
    if ((actualLevel == QLatin1String("v")) ||
        (actualLevel == QLatin1String("debug")) ||
        (actualLevel == QLatin1String("trace"))) {
        return LogLevel::Debug;
    }
    if (actualLevel == QLatin1String("warn")) {
        return LogLevel::Warning;
    }
    if (actualLevel == QLatin1String("error")) {
        return LogLevel::Critical;
    }
    if (actualLevel == QLatin1String("fatal")) {
        return LogLevel::Fatal;
    }
    if (actualLevel == QLatin1String("info")) {
        return LogLevel::Info;
    }
    return LogLevel::Debug;
}

qint64 MpvDeclarativeObject::duration() const {
    return isStopped()
        ? 0
        : qMax(mpvGetProperty(QLatin1String("duration")).toLongLong(),
               qint64(0));
}

qint64 MpvDeclarativeObject::position() const {
    return isStopped()
        ? 0
        : qMin(qMax(mpvGetProperty(QLatin1String("time-pos")).toLongLong(),
                    qint64(0)),
               duration());
}

int MpvDeclarativeObject::volume() const {
    return qMin(qMax(mpvGetProperty(QLatin1String("volume")).toInt(), 0), 100);
}

bool MpvDeclarativeObject::mute() const {
    return mpvGetProperty(QLatin1String("mute")).toBool();
}

bool MpvDeclarativeObject::seekable() const {
    return isStopped() ? false
                       : mpvGetProperty(QLatin1String("seekable")).toBool();
}

QString MpvDeclarativeObject::mediaTitle() const {
    return isStopped()
        ? QString()
        : mpvGetProperty(QLatin1String("media-title")).toString();
}

QString MpvDeclarativeObject::hwdec() const {
    // Querying "hwdec" itself will return empty string.
    return mpvGetProperty(QLatin1String("hwdec-current")).toString();
}

QString MpvDeclarativeObject::mpvVersion() const {
    return mpvGetProperty(QLatin1String("mpv-version")).toString();
}

QString MpvDeclarativeObject::mpvConfiguration() const {
    return mpvGetProperty(QLatin1String("mpv-configuration")).toString();
}

QString MpvDeclarativeObject::ffmpegVersion() const {
    return mpvGetProperty(QLatin1String("ffmpeg-version")).toString();
}

QString MpvDeclarativeObject::qtVersion() const {
    // qVersion(): run-time Qt version
    // QT_VERSION_STR: Qt version against which the application is compiled
    return QLatin1String(qVersion());
}

int MpvDeclarativeObject::vid() const {
    return isStopped() ? 0 : mpvGetProperty(QLatin1String("vid")).toInt();
}

int MpvDeclarativeObject::aid() const {
    return isStopped() ? 0 : mpvGetProperty(QLatin1String("aid")).toInt();
}

int MpvDeclarativeObject::sid() const {
    return isStopped() ? 0 : mpvGetProperty(QLatin1String("sid")).toInt();
}

int MpvDeclarativeObject::videoRotate() const {
    return isStopped()
        ? 0
        : qMin((qMax(mpvGetProperty(QLatin1String("video-out-params/rotate"))
                         .toInt(),
                     0) +
                360) %
                   360,
               359);
}

qreal MpvDeclarativeObject::videoAspect() const {
    return isStopped()
        ? 1.7777
        : qMax(
              mpvGetProperty(QLatin1String("video-out-params/aspect")).toReal(),
              0.0);
}

qreal MpvDeclarativeObject::speed() const {
    return qMax(mpvGetProperty(QLatin1String("speed")).toReal(), 0.0);
}

bool MpvDeclarativeObject::deinterlace() const {
    return mpvGetProperty(QLatin1String("deinterlace")).toBool();
}

bool MpvDeclarativeObject::audioExclusive() const {
    return mpvGetProperty(QLatin1String("audio-exclusive")).toBool();
}

QString MpvDeclarativeObject::audioFileAuto() const {
    return mpvGetProperty(QLatin1String("audio-file-auto")).toString();
}

QString MpvDeclarativeObject::subAuto() const {
    return mpvGetProperty(QLatin1String("sub-auto")).toString();
}

QString MpvDeclarativeObject::subCodepage() const {
    QString codePage = mpvGetProperty(QLatin1String("sub-codepage")).toString();
    if (codePage.startsWith(QLatin1Char('+'))) {
        codePage.remove(0, 1);
    }
    return codePage;
}

QString MpvDeclarativeObject::vo() const {
    return mpvGetProperty(QLatin1String("vo")).toString();
}

QString MpvDeclarativeObject::ao() const {
    return mpvGetProperty(QLatin1String("ao")).toString();
}

QString MpvDeclarativeObject::screenshotFormat() const {
    return mpvGetProperty(QLatin1String("screenshot-format")).toString();
}

bool MpvDeclarativeObject::screenshotTagColorspace() const {
    return mpvGetProperty(QLatin1String("screenshot-tag-colorspace")).toBool();
}

int MpvDeclarativeObject::screenshotPngCompression() const {
    return qMin(
        qMax(
            mpvGetProperty(QLatin1String("screenshot-png-compression")).toInt(),
            0),
        9);
}

int MpvDeclarativeObject::screenshotJpegQuality() const {
    return qMin(
        qMax(mpvGetProperty(QLatin1String("screenshot-jpeg-quality")).toInt(),
             0),
        100);
}

QString MpvDeclarativeObject::screenshotTemplate() const {
    return mpvGetProperty(QLatin1String("screenshot-template")).toString();
}

QString MpvDeclarativeObject::screenshotDirectory() const {
    return mpvGetProperty(QLatin1String("screenshot-directory")).toString();
}

QString MpvDeclarativeObject::profile() const {
    return mpvGetProperty(QLatin1String("profile")).toString();
}

bool MpvDeclarativeObject::hrSeek() const {
    return mpvGetProperty(QLatin1String("hr-seek")).toBool();
}

bool MpvDeclarativeObject::ytdl() const {
    return mpvGetProperty(QLatin1String("ytdl")).toBool();
}

bool MpvDeclarativeObject::loadScripts() const {
    return mpvGetProperty(QLatin1String("load-scripts")).toBool();
}

QString MpvDeclarativeObject::path() const {
    return isStopped() ? QString()
                       : mpvGetProperty(QLatin1String("path")).toString();
}

QString MpvDeclarativeObject::fileFormat() const {
    return isStopped()
        ? QString()
        : mpvGetProperty(QLatin1String("file-format")).toString();
}

qint64 MpvDeclarativeObject::fileSize() const {
    return isStopped()
        ? 0
        : qMax(mpvGetProperty(QLatin1String("file-size")).toLongLong(),
               qint64(0));
}

qreal MpvDeclarativeObject::videoBitrate() const {
    return isStopped()
        ? 0.0
        : qMax(mpvGetProperty(QLatin1String("video-bitrate")).toReal(), 0.0);
}

qreal MpvDeclarativeObject::audioBitrate() const {
    return isStopped()
        ? 0.0
        : qMax(mpvGetProperty(QLatin1String("audio-bitrate")).toReal(), 0.0);
}

MpvDeclarativeObject::AudioDevices
MpvDeclarativeObject::audioDeviceList() const {
    AudioDevices audioDevices;
    QVariantList deviceList =
        mpvGetProperty(QLatin1String("audio-device-list")).toList();
    for (const auto &device : deviceList) {
        const auto &deviceInfo = device.toMap();
        SingleTrackInfo singleTrackInfo;
        singleTrackInfo["name"] = deviceInfo["name"];
        singleTrackInfo["description"] = deviceInfo["description"];
        audioDevices.append(singleTrackInfo);
    }
    return audioDevices;
}

QString MpvDeclarativeObject::videoFormat() const {
    return isStopped()
        ? QString()
        : mpvGetProperty(QLatin1String("video-format")).toString();
}

MpvDeclarativeObject::MpvCallType MpvDeclarativeObject::mpvCallType() const {
    return currentMpvCallType;
}

MpvDeclarativeObject::MediaTracks MpvDeclarativeObject::mediaTracks() const {
    MediaTracks mediaTracks;
    QVariantList trackList =
        mpvGetProperty(QLatin1String("track-list")).toList();
    for (const auto &track : trackList) {
        const auto &trackInfo = track.toMap();
        if ((trackInfo["type"] != QLatin1String("video")) &&
            (trackInfo["type"] != QLatin1String("audio")) &&
            (trackInfo["type"] != QLatin1String("sub"))) {
            continue;
        }
        SingleTrackInfo singleTrackInfo;
        singleTrackInfo["id"] = trackInfo["id"];
        singleTrackInfo["type"] = trackInfo["type"];
        singleTrackInfo["src-id"] = trackInfo["src-id"];
        if (trackInfo["title"].toString().isEmpty()) {
            if (trackInfo["lang"].toString() != QLatin1String("und")) {
                singleTrackInfo["title"] = trackInfo["lang"];
            } else if (!trackInfo["external"].toBool()) {
                singleTrackInfo["title"] = "[internal]";
            } else {
                singleTrackInfo["title"] = "[untitled]";
            }
        } else {
            singleTrackInfo["title"] = trackInfo["title"];
        }
        singleTrackInfo["lang"] = trackInfo["lang"];
        singleTrackInfo["default"] = trackInfo["default"];
        singleTrackInfo["forced"] = trackInfo["forced"];
        singleTrackInfo["codec"] = trackInfo["codec"];
        singleTrackInfo["external"] = trackInfo["external"];
        singleTrackInfo["external-filename"] = trackInfo["external-filename"];
        singleTrackInfo["selected"] = trackInfo["selected"];
        singleTrackInfo["decoder-desc"] = trackInfo["decoder-desc"];
        if (trackInfo["type"] == QLatin1String("video")) {
            singleTrackInfo["albumart"] = trackInfo["albumart"];
            singleTrackInfo["demux-w"] = trackInfo["demux-w"];
            singleTrackInfo["demux-h"] = trackInfo["demux-h"];
            singleTrackInfo["demux-fps"] = trackInfo["demux-fps"];
            mediaTracks.videoChannels.append(singleTrackInfo);
        } else if (trackInfo["type"] == QLatin1String("audio")) {
            singleTrackInfo["demux-channel-count"] =
                trackInfo["demux-channel-count"];
            singleTrackInfo["demux-channels"] = trackInfo["demux-channels"];
            singleTrackInfo["demux-samplerate"] = trackInfo["demux-samplerate"];
            mediaTracks.audioTracks.append(singleTrackInfo);
        } else if (trackInfo["type"] == QLatin1String("sub")) {
            mediaTracks.subtitleStreams.append(singleTrackInfo);
        }
    }
    return mediaTracks;
}

MpvDeclarativeObject::Chapters MpvDeclarativeObject::chapters() const {
    Chapters chapters;
    QVariantList chapterList =
        mpvGetProperty(QLatin1String("chapter-list")).toList();
    for (const auto &chapter : chapterList) {
        const auto &chapterInfo = chapter.toMap();
        SingleTrackInfo singleTrackInfo;
        singleTrackInfo["title"] = chapterInfo["title"];
        singleTrackInfo["time"] = chapterInfo["time"];
        chapters.append(singleTrackInfo);
    }
    return chapters;
}

MpvDeclarativeObject::Metadata MpvDeclarativeObject::metadata() const {
    Metadata metadata;
    QVariantMap metadataMap = mpvGetProperty(QLatin1String("metadata")).toMap();
    auto iterator = metadataMap.constBegin();
    while (iterator != metadataMap.constEnd()) {
        metadata[iterator.key()] = iterator.value();
        ++iterator;
    }
    return metadata;
}

qreal MpvDeclarativeObject::avsync() const {
    return isStopped()
        ? 0.0
        : qMax(mpvGetProperty(QLatin1String("avsync")).toReal(), 0.0);
}

int MpvDeclarativeObject::percentPos() const {
    return isStopped()
        ? 0
        : qMin(qMax(mpvGetProperty(QLatin1String("percent-pos")).toInt(), 0),
               100);
}

qreal MpvDeclarativeObject::estimatedVfFps() const {
    return isStopped()
        ? 0.0
        : qMax(mpvGetProperty(QLatin1String("estimated-vf-fps")).toReal(), 0.0);
}

bool MpvDeclarativeObject::open(const QUrl &url) {
    if (!url.isValid()) {
        return false;
    }
    if (url != currentSource) {
        setSource(url);
    }
    if (!isPlaying()) {
        play();
    }
    return true;
}

bool MpvDeclarativeObject::play() {
    if (!isPaused() || !currentSource.isValid()) {
        return false;
    }
    const bool result = mpvSetProperty(QLatin1String("pause"), false);
    if (result) {
        Q_EMIT playing();
    }
    return result;
}

bool MpvDeclarativeObject::play(const QUrl &url) {
    if (!url.isValid()) {
        return false;
    }
    bool result = false;
    if ((url == currentSource) && !isPlaying()) {
        result = play();
    } else {
        result = open(url);
    }
    return result;
}

bool MpvDeclarativeObject::pause() {
    if (!isPlaying()) {
        return false;
    }
    const bool result = mpvSetProperty(QLatin1String("pause"), true);
    if (result) {
        Q_EMIT paused();
    }
    return result;
}

bool MpvDeclarativeObject::stop() {
    if (isStopped()) {
        return false;
    }
    const bool result = mpvSendCommand(QVariantList{"stop"});
    if (result) {
        Q_EMIT stopped();
    }
    currentSource.clear();
    return result;
}

bool MpvDeclarativeObject::seek(qint64 value, bool absolute, bool percent) {
    if (isStopped()) {
        return false;
    }
    QStringList arguments;
    arguments.append(percent ? "absolute-percent"
                             : (absolute ? "absolute" : "relative"));
    const qint64 min = (absolute || percent) ? 0 : -position();
    const qint64 max =
        percent ? 100 : (absolute ? duration() : duration() - position());
    return mpvSendCommand(
        QVariantList{"seek", qMin(qMax(value, min), max), arguments});
}

bool MpvDeclarativeObject::seekAbsolute(qint64 position) {
    if (isStopped() || position == this->position()) {
        return false;
    }
    return seek(qMin(qMax(position, qint64(0)), duration()), true);
}

bool MpvDeclarativeObject::seekRelative(qint64 offset) {
    if (isStopped() || offset == 0) {
        return false;
    }
    return seek(qMin(qMax(offset, -position()), duration() - position()));
}

bool MpvDeclarativeObject::seekPercent(int percent) {
    if (isStopped() || percent == this->percentPos()) {
        return false;
    }
    return seek(qMin(qMax(percent, 0), 100), true, true);
}

bool MpvDeclarativeObject::screenshot() {
    if (isStopped()) {
        return false;
    }
    // Replace "subtitles" with "video" if you don't want to include subtitles
    // when screenshotting.
    return mpvSendCommand(QVariantList{"screenshot", "subtitles"});
}

bool MpvDeclarativeObject::screenshotToFile(const QString &filePath) {
    if (isStopped() || filePath.isEmpty()) {
        return false;
    }
    // libmpv's default: including subtitles when making a screenshot.
    return mpvSendCommand(
        QVariantList{"screenshot-to-file", filePath, "subtitles"});
}

void MpvDeclarativeObject::setSource(const QUrl &source) {
    if (!source.isValid() || (source == currentSource)) {
        return;
    }
    const bool result = mpvSendCommand(QVariantList{
        "loadfile",
        source.isLocalFile() ? source.toLocalFile() : source.url()});
    if (result) {
        currentSource = source;
        Q_EMIT sourceChanged();
    }
}

void MpvDeclarativeObject::setMute(bool mute) {
    if (mute == this->mute()) {
        return;
    }
    mpvSetProperty(QLatin1String("mute"), mute);
}

void MpvDeclarativeObject::setPlaybackState(
    MpvDeclarativeObject::PlaybackState playbackState) {
    if (isStopped() || (this->playbackState() == playbackState)) {
        return;
    }
    bool result = false;
    switch (playbackState) {
    case PlaybackState::Stopped:
        result = stop();
        break;
    case PlaybackState::Paused:
        result = pause();
        break;
    case PlaybackState::Playing:
        result = play();
        break;
    }
    if (result) {
        Q_EMIT playbackStateChanged();
    }
}

void MpvDeclarativeObject::setLogLevel(
    MpvDeclarativeObject::LogLevel logLevel) {
    if (logLevel == this->logLevel()) {
        return;
    }
    QString level(QLatin1String("debug"));
    switch (logLevel) {
    case LogLevel::Off:
        level = QLatin1String("no");
        break;
    case LogLevel::Debug:
        // libmpv's log level: v (verbose) < debug < trace (print all messages)
        // Use "v" to avoid noisy message floods.
        level = QLatin1String("v");
        break;
    case LogLevel::Warning:
        level = QLatin1String("warn");
        break;
    case LogLevel::Critical:
        level = QLatin1String("error");
        break;
    case LogLevel::Fatal:
        level = QLatin1String("fatal");
        break;
    case LogLevel::Info:
        level = QLatin1String("info");
        break;
    }
    const bool result1 =
        mpvSetProperty(QLatin1String("terminal"), level != QLatin1String("no"));
    const bool result2 = mpvSetProperty(QLatin1String("msg-level"),
                                        QStringLiteral("all=%1").arg(level));
    const int result3 =
        mpv_request_log_messages(mpv, level.toUtf8().constData());
    if (result1 && result2 && (result3 >= 0)) {
        Q_EMIT logLevelChanged();
    } else {
        qWarning().noquote() << "Failed to set log level.";
    }
}

void MpvDeclarativeObject::setPosition(qint64 position) {
    if (isStopped() || position == this->position()) {
        return;
    }
    seek(qMin(qMax(position, qint64(0)), duration()));
}

void MpvDeclarativeObject::setVolume(int volume) {
    if (volume == this->volume()) {
        return;
    }
    mpvSetProperty(QLatin1String("volume"), qMin(qMax(volume, 0), 100));
}

void MpvDeclarativeObject::setHwdec(const QString &hwdec) {
    if (hwdec.isEmpty() || hwdec == this->hwdec()) {
        return;
    }
    mpvSetProperty(QLatin1String("hwdec"), hwdec);
}

void MpvDeclarativeObject::setVid(int vid) {
    if (isStopped() || vid == this->vid()) {
        return;
    }
    mpvSetProperty(QLatin1String("vid"), qMax(vid, 0));
}

void MpvDeclarativeObject::setAid(int aid) {
    if (isStopped() || aid == this->aid()) {
        return;
    }
    mpvSetProperty(QLatin1String("aid"), qMax(aid, 0));
}

void MpvDeclarativeObject::setSid(int sid) {
    if (isStopped() || sid == this->sid()) {
        return;
    }
    mpvSetProperty(QLatin1String("sid"), qMax(sid, 0));
}

void MpvDeclarativeObject::setVideoRotate(int videoRotate) {
    if (isStopped() || videoRotate == this->videoRotate()) {
        return;
    }
    mpvSetProperty(QLatin1String("video-rotate"),
                   qMin(qMax(videoRotate, 0), 359));
}

void MpvDeclarativeObject::setVideoAspect(qreal videoAspect) {
    if (isStopped() || videoAspect == this->videoAspect()) {
        return;
    }
    mpvSetProperty(QLatin1String("video-aspect"), qMax(videoAspect, 0.0));
}

void MpvDeclarativeObject::setSpeed(qreal speed) {
    if (isStopped() || speed == this->speed()) {
        return;
    }
    mpvSetProperty(QLatin1String("speed"), qMax(speed, 0.0));
}

void MpvDeclarativeObject::setDeinterlace(bool deinterlace) {
    if (deinterlace == this->deinterlace()) {
        return;
    }
    mpvSetProperty(QLatin1String("deinterlace"), deinterlace);
}

void MpvDeclarativeObject::setAudioExclusive(bool audioExclusive) {
    if (audioExclusive == this->audioExclusive()) {
        return;
    }
    mpvSetProperty(QLatin1String("audio-exclusive"), audioExclusive);
}

void MpvDeclarativeObject::setAudioFileAuto(const QString &audioFileAuto) {
    if (audioFileAuto.isEmpty() || audioFileAuto == this->audioFileAuto()) {
        return;
    }
    mpvSetProperty(QLatin1String("audio-file-auto"), audioFileAuto);
}

void MpvDeclarativeObject::setSubAuto(const QString &subAuto) {
    if (subAuto.isEmpty() || subAuto == this->subAuto()) {
        return;
    }
    mpvSetProperty(QLatin1String("sub-auto"), subAuto);
}

void MpvDeclarativeObject::setSubCodepage(const QString &subCodepage) {
    if (subCodepage.isEmpty() || subCodepage == this->subCodepage()) {
        return;
    }
    mpvSetProperty(QLatin1String("sub-codepage"),
                   subCodepage.startsWith(QLatin1Char('+'))
                       ? subCodepage
                       : (subCodepage.startsWith("cp")
                              ? QLatin1Char('+') + subCodepage
                              : subCodepage));
}

void MpvDeclarativeObject::setVo(const QString &vo) {
    if (vo.isEmpty() || vo == this->vo()) {
        return;
    }
    mpvSetProperty(QLatin1String("vo"), vo);
}

void MpvDeclarativeObject::setAo(const QString &ao) {
    if (ao.isEmpty() || ao == this->ao()) {
        return;
    }
    mpvSetProperty(QLatin1String("ao"), ao);
}

void MpvDeclarativeObject::setScreenshotFormat(
    const QString &screenshotFormat) {
    if (screenshotFormat.isEmpty() ||
        screenshotFormat == this->screenshotFormat()) {
        return;
    }
    mpvSetProperty(QLatin1String("screenshot-format"), screenshotFormat);
}

void MpvDeclarativeObject::setScreenshotPngCompression(
    int screenshotPngCompression) {
    if (screenshotPngCompression == this->screenshotPngCompression()) {
        return;
    }
    mpvSetProperty(QLatin1String("screenshot-png-compression"),
                   qMin(qMax(screenshotPngCompression, 0), 9));
}

void MpvDeclarativeObject::setScreenshotTemplate(
    const QString &screenshotTemplate) {
    if (screenshotTemplate.isEmpty() ||
        screenshotTemplate == this->screenshotTemplate()) {
        return;
    }
    mpvSetProperty(QLatin1String("screenshot-template"), screenshotTemplate);
}

void MpvDeclarativeObject::setScreenshotDirectory(
    const QString &screenshotDirectory) {
    if (screenshotDirectory.isEmpty() ||
        screenshotDirectory == this->screenshotDirectory()) {
        return;
    }
    mpvSetProperty(QLatin1String("screenshot-directory"), screenshotDirectory);
}

void MpvDeclarativeObject::setProfile(const QString &profile) {
    if (profile.isEmpty() || profile == this->profile()) {
        return;
    }
    mpvSendCommand(QVariantList{"apply-profile", profile});
}

void MpvDeclarativeObject::setHrSeek(bool hrSeek) {
    if (hrSeek == this->hrSeek()) {
        return;
    }
    mpvSetProperty(QLatin1String("hr-seek"), hrSeek ? "yes" : "no");
}

void MpvDeclarativeObject::setYtdl(bool ytdl) {
    if (ytdl == this->ytdl()) {
        return;
    }
    mpvSetProperty(QLatin1String("ytdl"), ytdl);
}

void MpvDeclarativeObject::setLoadScripts(bool loadScripts) {
    if (loadScripts == this->loadScripts()) {
        return;
    }
    mpvSetProperty(QLatin1String("load-scripts"), loadScripts);
}

void MpvDeclarativeObject::setScreenshotTagColorspace(
    bool screenshotTagColorspace) {
    if (screenshotTagColorspace == this->screenshotTagColorspace()) {
        return;
    }
    mpvSetProperty(QLatin1String("screenshot-tag-colorspace"),
                   screenshotTagColorspace);
}

void MpvDeclarativeObject::setScreenshotJpegQuality(int screenshotJpegQuality) {
    if (screenshotJpegQuality == this->screenshotJpegQuality()) {
        return;
    }
    mpvSetProperty(QLatin1String("screenshot-jpeg-quality"),
                   qMin(qMax(screenshotJpegQuality, 0), 100));
}

void MpvDeclarativeObject::setMpvCallType(
    MpvDeclarativeObject::MpvCallType mpvCallType) {
    if (this->mpvCallType() == mpvCallType) {
        return;
    }
    currentMpvCallType = mpvCallType;
    Q_EMIT mpvCallTypeChanged();
}

void MpvDeclarativeObject::setPercentPos(int percentPos) {
    if (isStopped() || percentPos == this->percentPos()) {
        return;
    }
    mpvSetProperty(QLatin1String("percent-pos"),
                   qMin(qMax(percentPos, 0), 100));
}

void MpvDeclarativeObject::handleMpvEvents() {
    // Process all events, until the event queue is empty.
    while (mpv != nullptr) {
        mpv_event *event = mpv_wait_event(mpv, 0.005);
        // Nothing happened. Happens on timeouts or sporadic wakeups.
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        bool shouldOutput = true;
        switch (event->event_id) {
        // Happens when the player quits. The player enters a state where it
        // tries to disconnect all clients. Most requests to the player will
        // fail, and the client should react to this and quit with
        // mpv_destroy() as soon as possible.
        case MPV_EVENT_SHUTDOWN:
            break;
        // See mpv_request_log_messages().
        case MPV_EVENT_LOG_MESSAGE:
            processMpvLogMessage(
                static_cast<mpv_event_log_message *>(event->data));
            shouldOutput = false;
            break;
        // Reply to a mpv_get_property_async() request.
        // See also mpv_event and mpv_event_property.
        case MPV_EVENT_GET_PROPERTY_REPLY:
            shouldOutput = false;
            break;
        // Reply to a mpv_set_property_async() request.
        // (Unlike MPV_EVENT_GET_PROPERTY, mpv_event_property is not used.)
        case MPV_EVENT_SET_PROPERTY_REPLY:
            shouldOutput = false;
            break;
        // Reply to a mpv_command_async() or mpv_command_node_async() request.
        // See also mpv_event and mpv_event_command.
        case MPV_EVENT_COMMAND_REPLY:
            shouldOutput = false;
            break;
        // Notification before playback start of a file (before the file is
        // loaded).
        case MPV_EVENT_START_FILE:
            setMediaStatus(MediaStatus::Loading);
            break;
        // Notification after playback end (after the file was unloaded).
        // See also mpv_event and mpv_event_end_file.
        case MPV_EVENT_END_FILE:
            setMediaStatus(MediaStatus::End);
            playbackStateChangeEvent();
            break;
        // Notification when the file has been loaded (headers were read
        // etc.), and decoding starts.
        case MPV_EVENT_FILE_LOADED:
            setMediaStatus(MediaStatus::Loaded);
            Q_EMIT loaded();
            playbackStateChangeEvent();
            break;
        // Idle mode was entered. In this mode, no file is played, and the
        // playback core waits for new commands. (The command line player
        // normally quits instead of entering idle mode, unless --idle was
        // specified. If mpv was started with mpv_create(), idle mode is enabled
        // by default.)
        case MPV_EVENT_IDLE:
            playbackStateChangeEvent();
            break;
        // Sent every time after a video frame is displayed. Note that
        // currently, this will be sent in lower frequency if there is no video,
        // or playback is paused - but that will be removed in the future, and
        // it will be restricted to video frames only.
        case MPV_EVENT_TICK:
            shouldOutput = false;
            break;
        // Triggered by the script-message input command. The command uses the
        // first argument of the command as client name (see mpv_client_name())
        // to dispatch the message, and passes along all arguments starting from
        // the second argument as strings.
        // See also mpv_event and mpv_event_client_message.
        case MPV_EVENT_CLIENT_MESSAGE:
            break;
        // Happens after video changed in some way. This can happen on
        // resolution changes, pixel format changes, or video filter changes.
        // The event is sent after the video filters and the VO are
        // reconfigured. Applications embedding a mpv window should listen to
        // this event in order to resize the window if needed.
        // Note that this event can happen sporadically, and you should check
        // yourself whether the video parameters really changed before doing
        // something expensive.
        case MPV_EVENT_VIDEO_RECONFIG:
            videoReconfig();
            break;
        // Similar to MPV_EVENT_VIDEO_RECONFIG. This is relatively
        // uninteresting, because there is no such thing as audio output
        // embedding.
        case MPV_EVENT_AUDIO_RECONFIG:
            audioReconfig();
            break;
        // Happens when a seek was initiated. Playback stops. Usually it will
        // resume with MPV_EVENT_PLAYBACK_RESTART as soon as the seek is
        // finished.
        case MPV_EVENT_SEEK:
            break;
        // There was a discontinuity of some sort (like a seek), and playback
        // was reinitialized. Usually happens after seeking, or ordered chapter
        // segment switches. The main purpose is allowing the client to detect
        // when a seek request is finished.
        case MPV_EVENT_PLAYBACK_RESTART:
            break;
        // Event sent due to mpv_observe_property().
        // See also mpv_event and mpv_event_property.
        case MPV_EVENT_PROPERTY_CHANGE:
            processMpvPropertyChange(
                static_cast<mpv_event_property *>(event->data));
            shouldOutput = false;
            break;
        // Happens if the internal per-mpv_handle ringbuffer overflows, and at
        // least 1 event had to be dropped. This can happen if the client
        // doesn't read the event queue quickly enough with mpv_wait_event(), or
        // if the client makes a very large number of asynchronous calls at
        // once.
        // Event delivery will continue normally once this event was returned
        // (this forces the client to empty the queue completely).
        case MPV_EVENT_QUEUE_OVERFLOW:
            break;
        // Triggered if a hook handler was registered with mpv_hook_add(), and
        // the hook is invoked. If you receive this, you must handle it, and
        // continue the hook with mpv_hook_continue().
        // See also mpv_event and mpv_event_hook.
        case MPV_EVENT_HOOK:
            break;
        default:
            break;
        }
        if (shouldOutput) {
            qDebug().noquote()
                << "[libmpv] Event received from mpv:"
                << QString::fromUtf8(mpv_event_name(event->event_id));
        }
    }
}
