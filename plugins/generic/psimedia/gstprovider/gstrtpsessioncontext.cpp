#include "gstrtpsessioncontext.h"

#include "gstthread.h"
#ifdef QT_GUI_LIB
#include "gstvideowidget.h"
#endif
#include "devices.h"

#include <climits>

namespace PsiMedia {
namespace {

constexpr int OpusPayloadType = 111;
constexpr int Vp8PayloadType  = 96;

bool isSupportedRemotePayload(const PPayloadInfo &payload, const QString &media)
{
    if (payload.id < 0 || payload.id > 127)
        return false;
    if (media == QLatin1String("audio")) {
        return payload.name.compare(QLatin1String("OPUS"), Qt::CaseInsensitive) == 0 && payload.clockrate == 48000
            && (payload.channels <= 0 || payload.channels == 2);
    }
    if (media == QLatin1String("video")) {
        return payload.name.compare(QLatin1String("VP8"), Qt::CaseInsensitive) == 0 && payload.clockrate == 90000;
    }
    return false;
}

QList<PPayloadInfo> supportedRemotePayloads(const QList<PPayloadInfo> &remote, const QString &media)
{
    for (const auto &payload : remote) {
        if (isSupportedRemotePayload(payload, media))
            return { payload };
    }
    return {};
}

QList<PPayloadInfo> negotiatedLocalPayloads(bool enabled, bool remoteConfigured, const QList<PPayloadInfo> &remote,
                                            const QString &media)
{
    if (!enabled)
        return {};

    const auto supportedRemote = supportedRemotePayloads(remote, media);
    if (remoteConfigured && supportedRemote.isEmpty())
        return {};

    PPayloadInfo payload;
    if (media == QLatin1String("audio")) {
        payload.id        = supportedRemote.isEmpty() ? OpusPayloadType : supportedRemote.constFirst().id;
        payload.name      = QStringLiteral("OPUS");
        payload.clockrate = 48000;
        payload.channels  = 2;
    } else if (media == QLatin1String("video")) {
        payload.id        = supportedRemote.isEmpty() ? Vp8PayloadType : supportedRemote.constFirst().id;
        payload.name      = QStringLiteral("VP8");
        payload.clockrate = 90000;
    } else {
        return {};
    }
    return { payload };
}

bool hasPayloadType(GstBuffer *buffer, int payloadType)
{
    if (!buffer || payloadType < 0 || payloadType > 127 || gst_buffer_get_size(buffer) < 2)
        return false;

    guint8 bytes[2] = {};
    if (gst_buffer_extract(buffer, 0, bytes, sizeof(bytes)) != sizeof(bytes))
        return false;
    if ((bytes[0] >> 6) != 2)
        return false;
    return (bytes[1] & 0x7f) == payloadType;
}

PRtpPacket packetFromBuffer(GstBuffer *buffer)
{
    PRtpPacket packet;
    packet.type = PRtpPacket::Type::Rtp;
    if (!buffer)
        return packet;

    const gsize size = gst_buffer_get_size(buffer);
    if (!size || size > gsize(INT_MAX))
        return packet;
    packet.rawValue.resize(int(size));
    if (gst_buffer_extract(buffer, 0, packet.rawValue.data(), size) != size)
        packet.rawValue.clear();
    return packet;
}

} // namespace

GstRtpSessionContext::GstRtpSessionContext(GstMainLoop *_gstLoop, DeviceMonitor *deviceMonitor, QObject *parent) :
    QObject(parent), gstLoop(_gstLoop), control(nullptr), hardwareDeviceMonitor(deviceMonitor), isStarted(false),
    isStopping(false), pending_status(false), recorder(this), audioBridge(QStringLiteral("audio")),
    videoBridge(QStringLiteral("video")), allow_writes(false)
{
#ifdef QT_GUI_LIB
    outputWidget  = nullptr;
    previewWidget = nullptr;
#endif

    devices.audioOutVolume = 100;
    devices.audioInVolume  = 100;

    codecs.useLocalAudioParams = true;
    codecs.useLocalVideoParams = true;

    audioRtp.session = this;
    videoRtp.session = this;

    connect(&recorder, SIGNAL(stopped()), SLOT(recorder_stopped()));
}

GstRtpSessionContext::~GstRtpSessionContext() { cleanup(); }

QObject *GstRtpSessionContext::qobject() { return this; }

void GstRtpSessionContext::stopRtpBridges()
{
    audioSendPayloadType.store(-1, std::memory_order_release);
    videoSendPayloadType.store(-1, std::memory_order_release);

    audioBridge.stop();
    videoBridge.stop();
    audioBridge.setNetworkPacketHandler({});
    audioBridge.setMediaPacketHandler({});
    videoBridge.setNetworkPacketHandler({});
    videoBridge.setMediaPacketHandler({});
}

void GstRtpSessionContext::cleanup()
{
    stopRtpBridges();

#ifdef QT_GUI_LIB
    if (outputWidget)
        outputWidget->show_frame(QImage());
    if (previewWidget)
        previewWidget->show_frame(QImage());
#endif

    codecs = RwControlConfigCodecs();

    isStarted      = false;
    isStopping     = false;
    pending_status = false;

    recorder.control = nullptr;

    write_mutex.lock();
    allow_writes = false;
    delete control;
    control = nullptr;
    write_mutex.unlock();
}

void GstRtpSessionContext::setAudioOutputDevice(const QString &deviceId)
{
    devices.audioOutId = deviceId;
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setAudioInputDevice(const QString &deviceId)
{
    devices.audioInId = deviceId;
    devices.fileNameIn.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setVideoInputDevice(const QString &deviceId)
{
    devices.videoInId = deviceId;
    devices.fileNameIn.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setFileInput(const QString &fileName)
{
    devices.fileNameIn = fileName;
    devices.audioInId.clear();
    devices.videoInId.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setFileDataInput(const QByteArray &fileData)
{
    devices.fileDataIn = fileData;
    devices.audioInId.clear();
    devices.videoInId.clear();
    devices.fileNameIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setFileLoopEnabled(bool enabled)
{
    devices.loopFile = enabled;
    if (control)
        control->updateDevices(devices);
}

#ifdef QT_GUI_LIB
void GstRtpSessionContext::setVideoOutputWidget(VideoWidgetContext *widget)
{
    // no change?
    if (!outputWidget && !widget)
        return;
    if (outputWidget && outputWidget->context == widget)
        return;

    delete outputWidget;
    outputWidget = nullptr;

    if (widget)
        outputWidget = new GstVideoWidget(widget, this);

    devices.useVideoOut = widget != nullptr;
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setVideoPreviewWidget(VideoWidgetContext *widget)
{
    // no change?
    if (!previewWidget && !widget)
        return;
    if (previewWidget && previewWidget->context == widget)
        return;

    delete previewWidget;
    previewWidget = nullptr;

    if (widget)
        previewWidget = new GstVideoWidget(widget, this);

    devices.useVideoPreview = widget != nullptr;
    if (control)
        control->updateDevices(devices);
}
#endif

void GstRtpSessionContext::setRecorder(QIODevice *recordDevice)
{
    // can't assign a new recording device after stopping
    Q_ASSERT(!isStopping);

    recorder.setDevice(recordDevice);
}

void GstRtpSessionContext::stopRecording() { recorder.stop(); }

void GstRtpSessionContext::setLocalAudioPreferences(const QList<PAudioParams> &params)
{
    codecs.useLocalAudioParams = true;
    codecs.localAudioParams    = params;
}

void GstRtpSessionContext::setLocalVideoPreferences(const QList<PVideoParams> &params)
{
    codecs.useLocalVideoParams = true;
    codecs.localVideoParams    = params;
}

void GstRtpSessionContext::setMaximumSendingBitrate(int kbps) { codecs.maximumSendingBitrate = kbps; }

void GstRtpSessionContext::setRemoteAudioPreferences(const QList<PPayloadInfo> &info)
{
    codecs.useRemoteAudioPayloadInfo = true;
    codecs.remoteAudioPayloadInfo    = info;
}

void GstRtpSessionContext::setRemoteVideoPreferences(const QList<PPayloadInfo> &info)
{
    codecs.useRemoteVideoPayloadInfo = true;
    codecs.remoteVideoPayloadInfo    = info;
}

void GstRtpSessionContext::start()
{
    Q_ASSERT(!control && !isStarted);

    terminalError = false;
    write_mutex.lock();

    control = new RwControlLocal(gstLoop, hardwareDeviceMonitor, this);
    connect(control, SIGNAL(statusReady(const RwControlStatus &)), SLOT(control_statusReady(const RwControlStatus &)));
    connect(control, SIGNAL(previewFrame(const QImage &)), SLOT(control_previewFrame(const QImage &)));
    connect(control, SIGNAL(outputFrame(const QImage &)), SLOT(control_outputFrame(const QImage &)));
    connect(control, SIGNAL(audioOutputIntensityChanged(int)), SLOT(control_audioOutputIntensityChanged(int)));
    connect(control, SIGNAL(audioInputIntensityChanged(int)), SLOT(control_audioInputIntensityChanged(int)));

    control->app            = this;
    control->cb_rtpAudioOut = cb_control_rtpAudioOut;
    control->cb_rtpVideoOut = cb_control_rtpVideoOut;
    control->cb_recordData  = cb_control_recordData;

    allow_writes = true;
    write_mutex.unlock();

    recorder.control = control;

    lastStatus     = RwControlStatus();
    isStarted      = false;
    pending_status = true;
    control->start(devices, codecs);
}

void GstRtpSessionContext::updatePreferences()
{
    Q_ASSERT(control && !pending_status);

    pending_status = true;
    control->updateCodecs(codecs);
}

void GstRtpSessionContext::transmitAudio()
{
    transmit.useAudio = true;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::transmitVideo()
{
    transmit.useVideo = true;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::pauseAudio()
{
    transmit.useAudio = false;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::pauseVideo()
{
    transmit.useVideo = false;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::stop()
{
    Q_ASSERT(control && !isStopping);

    // note: it's possible to stop even if pending_status is
    //   already true.  this is so we can stop a session that
    //   is in the middle of starting.

    isStopping     = true;
    pending_status = true;
    control->stop();
}

QList<PPayloadInfo> GstRtpSessionContext::localAudioPayloadInfo() const { return lastStatus.localAudioPayloadInfo; }

QList<PPayloadInfo> GstRtpSessionContext::localVideoPayloadInfo() const { return lastStatus.localVideoPayloadInfo; }

QList<PPayloadInfo> GstRtpSessionContext::remoteAudioPayloadInfo() const { return lastStatus.remoteAudioPayloadInfo; }

QList<PPayloadInfo> GstRtpSessionContext::remoteVideoPayloadInfo() const { return lastStatus.remoteVideoPayloadInfo; }

QList<PAudioParams> GstRtpSessionContext::audioParams() const { return lastStatus.localAudioParams; }

QList<PVideoParams> GstRtpSessionContext::videoParams() const { return lastStatus.localVideoParams; }

bool GstRtpSessionContext::canTransmitAudio() const { return lastStatus.canTransmitAudio; }

bool GstRtpSessionContext::canTransmitVideo() const { return lastStatus.canTransmitVideo; }

int GstRtpSessionContext::outputVolume() const { return devices.audioOutVolume; }

void GstRtpSessionContext::setOutputVolume(int level)
{
    devices.audioOutVolume = level;
    if (control)
        control->updateDevices(devices);
}

int GstRtpSessionContext::inputVolume() const { return devices.audioInVolume; }

void GstRtpSessionContext::setInputVolume(int level)
{
    devices.audioInVolume = level;
    if (control)
        control->updateDevices(devices);
}

RtpSessionContext::Error GstRtpSessionContext::errorCode() const { return static_cast<Error>(lastStatus.errorCode); }

RtpChannelContext *GstRtpSessionContext::audioRtpChannel() { return &audioRtp; }

RtpChannelContext *GstRtpSessionContext::videoRtpChannel() { return &videoRtp; }

void GstRtpSessionContext::dumpPipeline(std::function<void(const QStringList &)> callback)
{
    if (control)
        control->dumpPipeline(callback);
    else
        callback(QStringList());
}

void GstRtpSessionContext::push_packet_for_write(GstRtpChannel *from, const PRtpPacket &rtp)
{
    {
        QMutexLocker locker(&write_mutex);
        if (!allow_writes || !control)
            return;
    }

    if (from == &audioRtp)
        audioBridge.receivePacket(rtp);
    else if (from == &videoRtp)
        videoBridge.receivePacket(rtp);
}

bool GstRtpSessionContext::configureRtpBridges()
{
    const bool audioEnabled = codecs.useLocalAudioParams && !codecs.localAudioParams.isEmpty();
    const bool videoEnabled = codecs.useLocalVideoParams && !codecs.localVideoParams.isEmpty();

    const auto localAudio = negotiatedLocalPayloads(audioEnabled, codecs.useRemoteAudioPayloadInfo,
                                                     codecs.remoteAudioPayloadInfo, QStringLiteral("audio"));
    const auto localVideo = negotiatedLocalPayloads(videoEnabled, codecs.useRemoteVideoPayloadInfo,
                                                     codecs.remoteVideoPayloadInfo, QStringLiteral("video"));
    const auto remoteAudio = supportedRemotePayloads(codecs.remoteAudioPayloadInfo, QStringLiteral("audio"));
    const auto remoteVideo = supportedRemotePayloads(codecs.remoteVideoPayloadInfo, QStringLiteral("video"));

    lastStatus.localAudioPayloadInfo  = localAudio;
    lastStatus.localVideoPayloadInfo  = localVideo;
    lastStatus.remoteAudioPayloadInfo = remoteAudio;
    lastStatus.remoteVideoPayloadInfo = remoteVideo;

    const auto configure = [this](RtpSessionBridge &bridge, std::atomic<int> &sendPayloadType, GstRtpChannel &channel,
                                  const QList<PPayloadInfo> &local, const QList<PPayloadInfo> &remote, bool audio) {
        if (local.isEmpty()) {
            sendPayloadType.store(-1, std::memory_order_release);
            bridge.stop();
            bridge.setNetworkPacketHandler({});
            bridge.setMediaPacketHandler({});
            bridge.setRuntimeErrorHandler({});
            return true;
        }
        if (!bridge.isValid() || !bridge.setPayloads(local, remote))
            return false;

        bridge.setRuntimeErrorHandler([this]() { control_rtpBridgeError(); });
        auto *channelPtr = &channel;
        bridge.setNetworkPacketHandler(
            [channelPtr](const PRtpPacket &packet) { channelPtr->push_packet_for_read(packet); });
        bridge.setMediaPacketHandler([this, audio](GstBuffer *buffer) {
            const auto packet = packetFromBuffer(buffer);
            if (packet.rawValue.isEmpty())
                return;

            QMutexLocker locker(&write_mutex);
            if (!allow_writes || !control)
                return;
            if (audio)
                control->rtpAudioIn(packet);
            else
                control->rtpVideoIn(packet);
        });
        sendPayloadType.store(local.constFirst().id, std::memory_order_release);
        return bridge.start();
    };

    return configure(audioBridge, audioSendPayloadType, audioRtp, localAudio, remoteAudio, true)
        && configure(videoBridge, videoSendPayloadType, videoRtp, localVideo, remoteVideo, false);
}

void GstRtpSessionContext::control_statusReady(const RwControlStatus &status)
{
    if (terminalError)
        return;

    lastStatus = status;

    if (!status.finished && !status.error && pending_status && !status.stopped && !isStopping) {
        if (!configureRtpBridges()) {
            terminalError        = true;
            lastStatus.error     = true;
            lastStatus.errorCode = int(ErrorGeneric);
            cleanup();
            emit error();
            return;
        }
    }

    if (status.finished) {
        // finished status just means the file is done
        //   sending.  the session still remains active.
        emit finished();
    } else if (status.error) {
        terminalError = true;
        cleanup();
        emit error();
        return;
    } else if (pending_status) {
        if (status.stopped) {
            pending_status = false;

            cleanup();
            emit stopped();
            return;
        }

        // if we're currently stopping, ignore all other
        //   pending status events except for stopped
        //   (handled above)
        if (isStopping)
            return;

        pending_status = false;

        if (!isStarted) {
            isStarted = true;

            // if there was a pending record, start it
            recorder.startNext();

            emit started();
        } else
            emit preferencesUpdated();
    }
}

void GstRtpSessionContext::control_previewFrame(const QImage &img)
{
#ifdef QT_GUI_LIB
    if (previewWidget)
        previewWidget->show_frame(img);
#else
    Q_UNUSED(img)
#endif
}

void GstRtpSessionContext::control_outputFrame(const QImage &img)
{
#ifdef QT_GUI_LIB
    if (outputWidget)
        outputWidget->show_frame(img);
#else
    Q_UNUSED(img)
#endif
}

void GstRtpSessionContext::control_audioOutputIntensityChanged(int intensity)
{
    emit audioOutputIntensityChanged(intensity);
}

void GstRtpSessionContext::control_audioInputIntensityChanged(int intensity)
{
    emit audioInputIntensityChanged(intensity);
}

void GstRtpSessionContext::control_rtpBridgeError()
{
    if (terminalError || isStopping || !control)
        return;

    terminalError        = true;
    lastStatus.error     = true;
    lastStatus.errorCode = int(ErrorGeneric);
    cleanup();
    emit error();
}

void GstRtpSessionContext::recorder_stopped() { emit stoppedRecording(); }

void GstRtpSessionContext::cb_control_rtpAudioOut(const RtpWorker::EncodedRtpPacket &packet, void *app)
{
    static_cast<GstRtpSessionContext *>(app)->control_rtpAudioOut(packet);
}

void GstRtpSessionContext::cb_control_rtpVideoOut(const RtpWorker::EncodedRtpPacket &packet, void *app)
{
    static_cast<GstRtpSessionContext *>(app)->control_rtpVideoOut(packet);
}

void GstRtpSessionContext::cb_control_recordData(const QByteArray &packet, void *app)
{
    static_cast<GstRtpSessionContext *>(app)->control_recordData(packet);
}

void GstRtpSessionContext::control_rtpAudioOut(const RtpWorker::EncodedRtpPacket &packet)
{
    if (!hasPayloadType(packet.buffer, audioSendPayloadType.load(std::memory_order_acquire)))
        return;
    audioBridge.sendRtp(packet.buffer, packet.presentationAge);
}

void GstRtpSessionContext::control_rtpVideoOut(const RtpWorker::EncodedRtpPacket &packet)
{
    if (!hasPayloadType(packet.buffer, videoSendPayloadType.load(std::memory_order_acquire)))
        return;
    videoBridge.sendRtp(packet.buffer, packet.presentationAge);
}

void GstRtpSessionContext::control_recordData(const QByteArray &packet) { recorder.push_data_for_read(packet); }

} // namespace PsiMedia
