#include "gstrtpsessioncontext.h"

#include "gstthread.h"
#ifdef QT_GUI_LIB
#include "gstvideowidget.h"
#endif
#include "devices.h"

#include <climits>

#include <QPointer>
#include <QSet>
#include <QThread>

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

GstRtpSessionContext::GstRtpSessionContext(GstMainLoop *_gstLoop, DeviceMonitor *deviceMonitor, QObject *parent,
                                           bool secureMode) :
    QObject(parent), gstLoop(_gstLoop), control(nullptr), hardwareDeviceMonitor(deviceMonitor), isStarted(false),
    isStopping(false), pending_status(false), recorder(this), audioBridge(QStringLiteral("audio")),
    videoBridge(QStringLiteral("video")), allow_writes(false), secureMode_(secureMode)
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

    if (secureMode_) {
        for (auto &[associationId, state] : secureGroups_) {
            Q_UNUSED(associationId)
            if (state.started && state.group)
                state.group->stop();
            state.started = false;
        }
        securePayloadsReady_ = false;
        {
            QMutexLocker locker(&secureOutgoingMutex_);
            ++secureRouteGeneration_;
            if (!secureRouteGeneration_)
                ++secureRouteGeneration_;
            audioSecureProducer_ = {};
            videoSecureProducer_ = {};
            clearSecureOutgoingLocked();
        }
        return;
    }

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
    connect(control, SIGNAL(videoKeyframeRequested(quint32, quint8)),
            SLOT(control_videoKeyframeRequested(quint32, quint8)));

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

    // Secure sessions have a protected-only network boundary. The legacy RTP
    // channels remain present for Provider/1.6 ABI compatibility but cannot be
    // used to inject plaintext network packets.
    if (secureMode_)
        return;

    if (from == &audioRtp)
        audioBridge.receivePacket(rtp);
    else if (from == &videoRtp)
        videoBridge.receivePacket(rtp);
}

bool GstRtpSessionContext::configureRtpBridges()
{
    const bool audioEnabled = codecs.useLocalAudioParams && !codecs.localAudioParams.isEmpty();
    const bool videoEnabled = codecs.useLocalVideoParams && !codecs.localVideoParams.isEmpty();

    const auto localAudio  = negotiatedLocalPayloads(audioEnabled, codecs.useRemoteAudioPayloadInfo,
                                                     codecs.remoteAudioPayloadInfo, QStringLiteral("audio"));
    const auto localVideo  = negotiatedLocalPayloads(videoEnabled, codecs.useRemoteVideoPayloadInfo,
                                                     codecs.remoteVideoPayloadInfo, QStringLiteral("video"));
    const auto remoteAudio = supportedRemotePayloads(codecs.remoteAudioPayloadInfo, QStringLiteral("audio"));
    const auto remoteVideo = supportedRemotePayloads(codecs.remoteVideoPayloadInfo, QStringLiteral("video"));

    lastStatus.localAudioPayloadInfo  = localAudio;
    lastStatus.localVideoPayloadInfo  = localVideo;
    lastStatus.remoteAudioPayloadInfo = remoteAudio;
    lastStatus.remoteVideoPayloadInfo = remoteVideo;

    if (secureMode_) {
        audioSendPayloadType.store(localAudio.isEmpty() ? -1 : localAudio.constFirst().id, std::memory_order_release);
        videoSendPayloadType.store(localVideo.isEmpty() ? -1 : localVideo.constFirst().id, std::memory_order_release);
        securePayloadsReady_ = true;
        return configureSecureGroups();
    }

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

GstRtpSessionContext::SecureGroupState *GstRtpSessionContext::findSecureGroup(const QByteArray &associationId)
{
    const auto it = secureGroups_.find(associationId);
    return it == secureGroups_.end() ? nullptr : &it->second;
}

const GstRtpSessionContext::SecureGroupState *
GstRtpSessionContext::findSecureGroup(const QByteArray &associationId) const
{
    const auto it = secureGroups_.find(associationId);
    return it == secureGroups_.end() ? nullptr : &it->second;
}

GstRtpSessionContext::SecureGroupState *GstRtpSessionContext::ensureSecureGroup(const QByteArray &associationId)
{
    if (associationId.isEmpty())
        return nullptr;

    auto [it, inserted] = secureGroups_.try_emplace(associationId);
    auto &state         = it->second;
    if (!inserted && state.group)
        return &state;

    state.group = std::make_unique<SecureRtpGroup>();
    state.group->setProtectedPacketHandler([this](const PSecureRtpPacket &packet) {
        const auto handler = secureProtectedPacketHandler_;
        if (!handler)
            return;
        QPointer<GstRtpSessionContext> guard(this);
        handler(packet);
        if (!guard)
            return;
    });
    state.group->setRuntimeErrorHandler([this, associationId](SecureRtpSessionContext::Error error) {
        const auto                    *current = findSecureGroup(associationId);
        const quint64                  epoch   = current && current->group ? current->group->epoch() : 0;
        const auto                     handler = secureRuntimeErrorHandler_;
        QPointer<GstRtpSessionContext> guard(this);
        if (handler)
            handler(associationId, epoch, error);
        if (guard)
            guard->control_rtpBridgeError();
    });
    return &state;
}

bool GstRtpSessionContext::configureSecureGroup(const QByteArray &associationId)
{
    auto *state = findSecureGroup(associationId);
    if (!secureMode_ || !state || !state->group)
        return false;
    if (!securePayloadsReady_ || state->endpoints.isEmpty())
        return true;

    QList<RtpGroupBridge::Endpoint> groupEndpoints;
    for (const auto &endpoint : state->endpoints) {
        const bool  audio  = endpoint.media == QLatin1String("audio");
        const auto &local  = audio ? lastStatus.localAudioPayloadInfo : lastStatus.localVideoPayloadInfo;
        const auto &remote = audio ? lastStatus.remoteAudioPayloadInfo : lastStatus.remoteVideoPayloadInfo;
        if (local.isEmpty() || remote.isEmpty())
            return false;

        RtpGroupBridge::Endpoint groupEndpoint;
        groupEndpoint.id                   = endpoint.endpointId;
        groupEndpoint.media                = endpoint.media;
        groupEndpoint.localPayloads        = local;
        groupEndpoint.remotePayloads       = remote;
        groupEndpoint.route.endpointId     = endpoint.endpointId;
        groupEndpoint.route.mid            = endpoint.mid;
        groupEndpoint.route.midExtensionId = endpoint.midExtensionId;

        for (int payloadType : endpoint.incomingPayloadTypes) {
            if (payloadType < 0 || payloadType > 127)
                return false;
            groupEndpoint.route.incomingPayloadTypes.insert(quint8(payloadType));
        }
        if (groupEndpoint.route.incomingPayloadTypes.isEmpty())
            return false;

        for (quint32 ssrc : endpoint.incomingSsrcs) {
            if (ssrc)
                groupEndpoint.route.incomingSsrcs.insert(ssrc);
        }
        for (quint32 ssrc : endpoint.localSsrcs) {
            if (ssrc)
                groupEndpoint.route.localSsrcs.insert(ssrc);
        }

        groupEndpoints.append(std::move(groupEndpoint));
    }

    if (!state->group->configureEndpoints(groupEndpoints))
        return false;

    for (const auto &endpoint : groupEndpoints) {
        const bool audio = endpoint.media == QLatin1String("audio");
        state->group->setEndpointMediaPacketHandler(endpoint.id, [this, audio](GstBuffer *buffer) {
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
    }

    return maybeStartSecureGroup(associationId);
}

bool GstRtpSessionContext::configureSecureGroups()
{
    if (!secureMode_)
        return false;
    if (!securePayloadsReady_ || secureEndpoints_.isEmpty())
        return true;

    for (const auto &[associationId, state] : secureGroups_) {
        if (!state.endpoints.isEmpty() && !configureSecureGroup(associationId))
            return false;
    }
    refreshSecureProducerRoutes();
    return true;
}

bool GstRtpSessionContext::maybeStartSecureGroup(const QByteArray &associationId)
{
    auto *state = findSecureGroup(associationId);
    if (!secureMode_ || !state || !state->group)
        return false;
    if (state->started)
        return true;
    if (!securePayloadsReady_ || state->endpoints.isEmpty() || !state->group->isReady())
        return true;

    if (!state->group->start())
        return false;
    state->started = true;
    return true;
}

bool GstRtpSessionContext::secureConfigureEndpoints(const QList<PSecureRtpEndpoint> &endpoints)
{
    if (!secureMode_ || QThread::currentThread() != thread())
        return false;

    QSet<QByteArray>                                endpointIds;
    QSet<QString>                                   media;
    std::map<QByteArray, QList<PSecureRtpEndpoint>> desired;
    for (const auto &endpoint : endpoints) {
        if (endpoint.endpointId.isEmpty() || endpoint.associationId.isEmpty()
            || endpointIds.contains(endpoint.endpointId)
            || (endpoint.media != QLatin1String("audio") && endpoint.media != QLatin1String("video"))
            || media.contains(endpoint.media))
            return false;
        endpointIds.insert(endpoint.endpointId);
        media.insert(endpoint.media);
        for (int payloadType : endpoint.incomingPayloadTypes) {
            if (payloadType < 0 || payloadType > 127)
                return false;
        }
        desired[endpoint.associationId].append(endpoint);
    }

    // Snapshot group membership so a later group's rejected route/PT table can
    // roll back earlier groups without touching crypto state.
    std::map<QByteArray, QList<PSecureRtpEndpoint>> previous;
    for (const auto &[associationId, state] : secureGroups_)
        previous.emplace(associationId, state.endpoints);

    const auto previousEndpoints = secureEndpoints_;
    secureEndpoints_             = endpoints;

    for (const auto &[associationId, groupEndpoints] : desired) {
        auto *state = ensureSecureGroup(associationId);
        if (!state) {
            secureEndpoints_ = previousEndpoints;
            return false;
        }
        state->endpoints = groupEndpoints;
        if (securePayloadsReady_ && !configureSecureGroup(associationId)) {
            secureEndpoints_ = previousEndpoints;
            for (auto &[restoreId, restoreEndpoints] : previous) {
                auto *restore      = ensureSecureGroup(restoreId);
                restore->endpoints = restoreEndpoints;
                if (restore->group && restoreEndpoints.isEmpty()) {
                    restore->group->clearEndpoints();
                    restore->started = false;
                } else if (securePayloadsReady_ && !restoreEndpoints.isEmpty()) {
                    configureSecureGroup(restoreId);
                }
            }
            for (auto &[id, value] : secureGroups_) {
                if (!previous.count(id)) {
                    if (value.group)
                        value.group->clearEndpoints();
                    value.started = false;
                    value.endpoints.clear();
                }
            }
            refreshSecureProducerRoutes();
            return false;
        }
    }

    // Groups removed from the route table no longer accept/produce media, but
    // their crypto object may remain staged until Iris invalidates the DTLS
    // association explicitly.
    for (auto &[associationId, state] : secureGroups_) {
        if (desired.count(associationId))
            continue;
        if (state.group)
            state.group->clearEndpoints();
        state.started = false;
        state.endpoints.clear();
    }

    refreshSecureProducerRoutes();
    return true;
}

bool GstRtpSessionContext::secureConfigureAssociation(const QByteArray &associationId, quint64 epoch,
                                                      const QString &profile, const QByteArray &localMasterKey,
                                                      const QByteArray &localMasterSalt,
                                                      const QByteArray &remoteMasterKey,
                                                      const QByteArray &remoteMasterSalt)
{
    if (!secureMode_ || QThread::currentThread() != thread() || associationId.isEmpty())
        return false;
    auto *state = ensureSecureGroup(associationId);
    if (!state
        || !state->group->activate(associationId, epoch, profile, localMasterKey, localMasterSalt, remoteMasterKey,
                                   remoteMasterSalt))
        return false;
    if (!maybeStartSecureGroup(associationId))
        return false;
    refreshSecureProducerRoutes();
    return true;
}

void GstRtpSessionContext::secureInvalidateAssociation(const QByteArray &associationId, quint64 epoch)
{
    if (!secureMode_ || QThread::currentThread() != thread())
        return;
    auto *state = findSecureGroup(associationId);
    if (!state || !state->group)
        return;
    state->group->invalidate(associationId, epoch);
    refreshSecureProducerRoutes();
}

bool GstRtpSessionContext::secureAssociationReady(const QByteArray &associationId) const
{
    const auto *state = findSecureGroup(associationId);
    return secureMode_ && state && state->group && state->group->isReady();
}

quint64 GstRtpSessionContext::secureAssociationEpoch(const QByteArray &associationId) const
{
    const auto *state = findSecureGroup(associationId);
    return secureMode_ && state && state->group ? state->group->epoch() : 0;
}

SecureRtpSessionContext::Error GstRtpSessionContext::secureLastError(const QByteArray &associationId) const
{
    const auto *state = findSecureGroup(associationId);
    return secureMode_ && state && state->group ? state->group->lastError() : SecureRtpSessionContext::Error::NotReady;
}

void GstRtpSessionContext::secureSetProtectedPacketHandler(SecureRtpSessionContext::ProtectedPacketHandler handler)
{
    if (!secureMode_ || QThread::currentThread() != thread())
        return;
    secureProtectedPacketHandler_ = std::move(handler);
}

void GstRtpSessionContext::secureSetRuntimeErrorHandler(SecureRtpSessionContext::RuntimeErrorHandler handler)
{
    if (!secureMode_ || QThread::currentThread() != thread())
        return;
    secureRuntimeErrorHandler_ = std::move(handler);
}

bool GstRtpSessionContext::secureReceiveProtectedPacket(const PSecureRtpPacket &packet)
{
    if (!secureMode_ || QThread::currentThread() != thread() || packet.associationId.isEmpty())
        return false;
    auto *state = findSecureGroup(packet.associationId);
    return state && state->group && state->group->receiveProtectedPacket(packet);
}

void GstRtpSessionContext::clearSecureOutgoingLocked()
{
    secureOutgoingQueue_.clear();
    secureOutgoingBytes_     = 0;
    secureOutgoingScheduled_ = false;
}

void GstRtpSessionContext::purgeExpiredSecureOutgoingLocked()
{
    const auto now = std::chrono::steady_clock::now();
    while (!secureOutgoingQueue_.isEmpty()) {
        const auto &front = secureOutgoingQueue_.head();
        const auto  age   = std::chrono::duration_cast<std::chrono::milliseconds>(now - front.enqueuedAt).count();
        if (age <= MaxSecureOutgoingAgeMs)
            break;
        const auto    dropped = secureOutgoingQueue_.dequeue();
        const quint64 size    = dropped.byteSize();
        secureOutgoingBytes_  = size > secureOutgoingBytes_ ? 0 : secureOutgoingBytes_ - size;
    }
}

void GstRtpSessionContext::scheduleSecureOutgoingLocked()
{
    if (secureOutgoingScheduled_)
        return;
    const quint64 generation = secureRouteGeneration_;
    secureOutgoingScheduled_ = true;
    if (!QMetaObject::invokeMethod(
            this, [this, generation]() { drainSecureOutgoing(generation); }, Qt::QueuedConnection))
        secureOutgoingScheduled_ = false;
}

void GstRtpSessionContext::refreshSecureProducerRoutes()
{
    SecureProducerRoute audio;
    SecureProducerRoute video;

    for (const auto &endpoint : secureEndpoints_) {
        const auto *state = findSecureGroup(endpoint.associationId);
        if (!state || !state->group || !state->started || !state->group->isReady())
            continue;

        SecureProducerRoute route;
        route.associationId = endpoint.associationId;
        route.endpointId    = endpoint.endpointId;
        route.epoch         = state->group->epoch();
        route.enabled       = true;
        if (endpoint.media == QLatin1String("audio"))
            audio = std::move(route);
        else if (endpoint.media == QLatin1String("video"))
            video = std::move(route);
    }

    QMutexLocker locker(&secureOutgoingMutex_);
    ++secureRouteGeneration_;
    if (!secureRouteGeneration_)
        ++secureRouteGeneration_;
    clearSecureOutgoingLocked();
    audioSecureProducer_ = std::move(audio);
    videoSecureProducer_ = std::move(video);
}

void GstRtpSessionContext::enqueueSecureOutgoing(bool audio, const RtpWorker::EncodedRtpPacket &packet)
{
    if (!packet.buffer)
        return;

    auto buffer
        = std::shared_ptr<GstBuffer>(gst_buffer_ref(packet.buffer), [](GstBuffer *value) { gst_buffer_unref(value); });

    QMutexLocker locker(&secureOutgoingMutex_);
    purgeExpiredSecureOutgoingLocked();
    const auto &producer = audio ? audioSecureProducer_ : videoSecureProducer_;
    if (!producer.enabled || producer.associationId.isEmpty() || producer.endpointId.isEmpty())
        return;

    SecureOutgoingPacket item;
    item.routeGeneration = secureRouteGeneration_;
    item.associationId   = producer.associationId;
    item.endpointId      = producer.endpointId;
    item.epoch           = producer.epoch;
    item.buffer          = std::move(buffer);
    item.presentationAge = packet.presentationAge;

    const quint64 size = item.byteSize();
    if (!size || size > MaxSecureOutgoingBytes)
        return;

    while (!secureOutgoingQueue_.isEmpty()
           && (secureOutgoingQueue_.size() >= MaxSecureOutgoingPackets
               || secureOutgoingBytes_ + size > MaxSecureOutgoingBytes)) {
        const auto    dropped     = secureOutgoingQueue_.dequeue();
        const quint64 droppedSize = dropped.byteSize();
        secureOutgoingBytes_      = droppedSize > secureOutgoingBytes_ ? 0 : secureOutgoingBytes_ - droppedSize;
    }

    secureOutgoingQueue_.enqueue(std::move(item));
    secureOutgoingBytes_ += size;
    scheduleSecureOutgoingLocked();
}

void GstRtpSessionContext::drainSecureOutgoing(quint64 routeGeneration)
{
    const auto startedAt = std::chrono::steady_clock::now();
    int        delivered = 0;

    for (;;) {
        SecureOutgoingPacket item;
        {
            QMutexLocker locker(&secureOutgoingMutex_);
            if (routeGeneration != secureRouteGeneration_)
                return;
            purgeExpiredSecureOutgoingLocked();
            if (secureOutgoingQueue_.isEmpty()) {
                secureOutgoingScheduled_ = false;
                return;
            }

            item                 = secureOutgoingQueue_.dequeue();
            const quint64 size   = item.byteSize();
            secureOutgoingBytes_ = size > secureOutgoingBytes_ ? 0 : secureOutgoingBytes_ - size;
        }

        auto *state = findSecureGroup(item.associationId);
        if (item.routeGeneration == secureRouteGeneration_ && state && state->started && state->group
            && state->group->isReady() && state->group->epoch() == item.epoch) {
            state->group->sendRtp(item.endpointId, item.buffer.get(), item.presentationAge);
        }

        ++delivered;
        const auto elapsed
            = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt)
                  .count();
        if (delivered < MaxSecureOutgoingDrain && elapsed < MaxSecureOutgoingDrainMs)
            continue;

        QMutexLocker locker(&secureOutgoingMutex_);
        if (routeGeneration != secureRouteGeneration_)
            return;
        secureOutgoingScheduled_ = false;
        purgeExpiredSecureOutgoingLocked();
        if (!secureOutgoingQueue_.isEmpty())
            scheduleSecureOutgoingLocked();
        return;
    }
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

void GstRtpSessionContext::control_videoKeyframeRequested(quint32 ssrc, quint8 payloadType)
{
    if (!ssrc || payloadType > 127 || terminalError || isStopping)
        return;

    if (!secureMode_) {
        videoBridge.requestRemoteKeyframe(ssrc, payloadType);
        return;
    }

    SecureGroupState *exact             = nullptr;
    SecureGroupState *fallback          = nullptr;
    bool              fallbackAmbiguous = false;

    for (auto &[associationId, state] : secureGroups_) {
        Q_UNUSED(associationId)
        if (!state.started || !state.group || !state.group->isReady())
            continue;

        for (const auto &endpoint : state.endpoints) {
            if (endpoint.media != QLatin1String("video") || !endpoint.incomingPayloadTypes.contains(int(payloadType)))
                continue;

            if (!endpoint.incomingSsrcs.isEmpty() && endpoint.incomingSsrcs.contains(ssrc)) {
                if (exact && exact != &state)
                    return;
                exact = &state;
                continue;
            }

            if (endpoint.incomingSsrcs.isEmpty()) {
                if (fallback && fallback != &state)
                    fallbackAmbiguous = true;
                else
                    fallback = &state;
            }
        }
    }

    SecureGroupState *target = exact ? exact : (!fallbackAmbiguous ? fallback : nullptr);
    if (!target || !target->group)
        return;

#ifdef RTPWORKER_DEBUG
    qDebug() << "requesting remote video keyframe through RTP group"
             << "ssrc=" << ssrc << "pt=" << payloadType;
#endif
    target->group->requestRemoteKeyframe(ssrc, payloadType);
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
    if (!secureMode_) {
        audioBridge.sendRtp(packet.buffer, packet.presentationAge);
        return;
    }
    enqueueSecureOutgoing(true, packet);
}

void GstRtpSessionContext::control_rtpVideoOut(const RtpWorker::EncodedRtpPacket &packet)
{
    if (!hasPayloadType(packet.buffer, videoSendPayloadType.load(std::memory_order_acquire)))
        return;
    if (!secureMode_) {
        videoBridge.sendRtp(packet.buffer, packet.presentationAge);
        return;
    }
    enqueueSecureOutgoing(false, packet);
}

void GstRtpSessionContext::control_recordData(const QByteArray &packet) { recorder.push_data_for_read(packet); }

} // namespace PsiMedia
