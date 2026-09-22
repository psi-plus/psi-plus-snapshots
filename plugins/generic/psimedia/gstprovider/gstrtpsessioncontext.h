/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef PSIMEDIA_GSTRTPSESSIONCONTEXT_H
#define PSIMEDIA_GSTRTPSESSIONCONTEXT_H

#include "psimediaprovider.h"

#include "gstrecorder.h"
#include "gstrtpchannel.h"
#include "rtpsessionbridge.h"
#include "rwcontrol.h"
#include "securertpgroup.h"

#include <QMutex>
#include <QQueue>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <utility>

namespace PsiMedia {

class GstMainLoop;
class GstVideoWidget;
class DeviceMonitor;

//----------------------------------------------------------------------------
// GstRtpSessionContext
//----------------------------------------------------------------------------
class GstRtpSessionContext : public QObject, public RtpSessionContext {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::RtpSessionContext)

public:
    GstMainLoop *gstLoop;

    RwControlLocal        *control;
    RwControlConfigDevices devices;
    RwControlConfigCodecs  codecs;
    RwControlTransmit      transmit;
    RwControlStatus        lastStatus;
    DeviceMonitor         *hardwareDeviceMonitor;
    bool                   isStarted;
    bool                   isStopping;
    bool                   pending_status;
    bool                   terminalError = false;

#ifdef QT_GUI_LIB
    GstVideoWidget *outputWidget, *previewWidget;
#endif

    GstRecorder recorder;

    // keep these parentless, so they can switch threads
    GstRtpChannel audioRtp;
    GstRtpChannel videoRtp;

    // One RFC 3550 session per media type. These live on the provider's Qt
    // thread while the legacy encoder/decoder worker remains on the GLib
    // thread. The bridge callbacks serialize packet delivery back here.
    RtpSessionBridge audioBridge;
    RtpSessionBridge videoBridge;
    std::atomic<int> audioSendPayloadType { -1 };
    std::atomic<int> videoSendPayloadType { -1 };

    QMutex write_mutex;
    bool   allow_writes;

    explicit GstRtpSessionContext(GstMainLoop *_gstLoop, DeviceMonitor *deviceMonitor, QObject *parent = nullptr,
                                  bool secureMode = false);

    ~GstRtpSessionContext() override;

    QObject *qobject() override;

    void cleanup();
    void setAudioOutputDevice(const QString &deviceId) override;
    void setAudioInputDevice(const QString &deviceId) override;
    void setVideoInputDevice(const QString &deviceId) override;
    void setFileInput(const QString &fileName) override;
    void setFileDataInput(const QByteArray &fileData) override;
    void setFileLoopEnabled(bool enabled) override;

#ifdef QT_GUI_LIB
    void setVideoOutputWidget(VideoWidgetContext *widget) override;
    void setVideoPreviewWidget(VideoWidgetContext *widget) override;
#endif

    void                     setRecorder(QIODevice *recordDevice) override;
    void                     stopRecording() override;
    void                     setLocalAudioPreferences(const QList<PAudioParams> &params) override;
    void                     setLocalVideoPreferences(const QList<PVideoParams> &params) override;
    void                     setMaximumSendingBitrate(int kbps) override;
    void                     setRemoteAudioPreferences(const QList<PPayloadInfo> &info) override;
    void                     setRemoteVideoPreferences(const QList<PPayloadInfo> &info) override;
    void                     start() override;
    void                     updatePreferences() override;
    void                     transmitAudio() override;
    void                     transmitVideo() override;
    void                     pauseAudio() override;
    void                     pauseVideo() override;
    void                     stop() override;
    QList<PPayloadInfo>      localAudioPayloadInfo() const override;
    QList<PPayloadInfo>      localVideoPayloadInfo() const override;
    QList<PPayloadInfo>      remoteAudioPayloadInfo() const override;
    QList<PPayloadInfo>      remoteVideoPayloadInfo() const override;
    QList<PAudioParams>      audioParams() const override;
    QList<PVideoParams>      videoParams() const override;
    bool                     canTransmitAudio() const override;
    bool                     canTransmitVideo() const override;
    int                      outputVolume() const override;
    void                     setOutputVolume(int level) override;
    int                      inputVolume() const override;
    void                     setInputVolume(int level) override;
    RtpSessionContext::Error errorCode() const override;
    RtpChannelContext       *audioRtpChannel() override;
    RtpChannelContext       *videoRtpChannel() override;
    void                     dumpPipeline(std::function<void(const QStringList &)> callback) override;

    // Internal secure-session implementation used by GstSecureRtpSessionContext.
    bool    secureConfigureEndpoints(const QList<PSecureRtpEndpoint> &endpoints);
    bool    secureConfigureAssociation(const QByteArray &associationId, quint64 epoch, const QString &profile,
                                       const QByteArray &localMasterKey, const QByteArray &localMasterSalt,
                                       const QByteArray &remoteMasterKey, const QByteArray &remoteMasterSalt);
    void    secureInvalidateAssociation(const QByteArray &associationId, quint64 epoch);
    bool    secureAssociationReady(const QByteArray &associationId) const;
    quint64 secureAssociationEpoch(const QByteArray &associationId) const;
    SecureRtpSessionContext::Error secureLastError(const QByteArray &associationId) const;
    void secureSetProtectedPacketHandler(SecureRtpSessionContext::ProtectedPacketHandler handler);
    void secureSetRuntimeErrorHandler(SecureRtpSessionContext::RuntimeErrorHandler handler);
    bool secureReceiveProtectedPacket(const PSecureRtpPacket &packet);

    // channel calls this, which may be in another thread
    void push_packet_for_write(GstRtpChannel *from, const PRtpPacket &rtp);

signals:
    void started();
    void preferencesUpdated();
    void audioOutputIntensityChanged(int intensity);
    void audioInputIntensityChanged(int intensity);
    void stoppedRecording();
    void stopped();
    void finished();
    void error();

private slots:
    void control_statusReady(const RwControlStatus &status);
    void control_previewFrame(const QImage &img);
    void control_outputFrame(const QImage &img);
    void control_audioOutputIntensityChanged(int intensity);
    void control_audioInputIntensityChanged(int intensity);
    void control_videoKeyframeRequested(quint32 ssrc, quint8 payloadType);
    void control_rtpBridgeError();
    void recorder_stopped();

private:
    friend struct GstRtpSessionContextTestAccess;

    static void cb_control_rtpAudioOut(const RtpWorker::EncodedRtpPacket &packet, void *app);
    static void cb_control_rtpVideoOut(const RtpWorker::EncodedRtpPacket &packet, void *app);
    static void cb_control_recordData(const QByteArray &packet, void *app);

    struct SecureGroupState {
        std::unique_ptr<SecureRtpGroup> group;
        QList<PSecureRtpEndpoint>       endpoints;
        bool                            started = false;
    };

    struct SecureProducerRoute {
        QByteArray associationId;
        QByteArray endpointId;
        quint64    epoch   = 0;
        bool       enabled = false;
    };

    struct SecureOutgoingPacket {
        quint64                               routeGeneration = 0;
        QByteArray                            associationId;
        QByteArray                            endpointId;
        quint64                               epoch = 0;
        std::shared_ptr<GstBuffer>            buffer;
        GstClockTime                          presentationAge = GST_CLOCK_TIME_NONE;
        std::chrono::steady_clock::time_point enqueuedAt      = std::chrono::steady_clock::now();

        quint64 byteSize() const { return buffer ? quint64(gst_buffer_get_size(buffer.get())) : 0; }
    };

    static constexpr int     MaxSecureOutgoingPackets = 256;
    static constexpr quint64 MaxSecureOutgoingBytes   = 512 * 1024;
    static constexpr qint64  MaxSecureOutgoingAgeMs   = 1000;
    static constexpr int     MaxSecureOutgoingDrain   = 64;
    static constexpr qint64  MaxSecureOutgoingDrainMs = 5;

    bool                    configureRtpBridges();
    bool                    configureSecureGroups();
    bool                    configureSecureGroup(const QByteArray &associationId);
    bool                    maybeStartSecureGroup(const QByteArray &associationId);
    SecureGroupState       *ensureSecureGroup(const QByteArray &associationId);
    SecureGroupState       *findSecureGroup(const QByteArray &associationId);
    const SecureGroupState *findSecureGroup(const QByteArray &associationId) const;
    void                    stopRtpBridges();

    void refreshSecureProducerRoutes();
    void enqueueSecureOutgoing(bool audio, const RtpWorker::EncodedRtpPacket &packet);
    void drainSecureOutgoing(quint64 routeGeneration);
    void scheduleSecureOutgoingLocked();
    void clearSecureOutgoingLocked();
    void purgeExpiredSecureOutgoingLocked();

    // note: this is executed from a different thread
    void control_rtpAudioOut(const RtpWorker::EncodedRtpPacket &packet);

    // note: this is executed from a different thread
    void control_rtpVideoOut(const RtpWorker::EncodedRtpPacket &packet);

    // note: this is executed from a different thread
    void control_recordData(const QByteArray &packet);

    bool                                            secureMode_ = false;
    std::map<QByteArray, SecureGroupState>          secureGroups_;
    QList<PSecureRtpEndpoint>                       secureEndpoints_;
    bool                                            securePayloadsReady_ = false;
    SecureRtpSessionContext::ProtectedPacketHandler secureProtectedPacketHandler_;
    SecureRtpSessionContext::RuntimeErrorHandler    secureRuntimeErrorHandler_;

    mutable QMutex               secureOutgoingMutex_;
    QQueue<SecureOutgoingPacket> secureOutgoingQueue_;
    quint64                      secureOutgoingBytes_     = 0;
    quint64                      secureRouteGeneration_   = 1;
    bool                         secureOutgoingScheduled_ = false;
    SecureProducerRoute          audioSecureProducer_;
    SecureProducerRoute          videoSecureProducer_;
};

class GstSecureRtpSessionContext final : public GstRtpSessionContext, public SecureRtpSessionContext {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::SecureRtpSessionContext)

public:
    explicit GstSecureRtpSessionContext(GstMainLoop *gstLoop, DeviceMonitor *deviceMonitor, QObject *parent = nullptr) :
        GstRtpSessionContext(gstLoop, deviceMonitor, parent, true)
    {
    }

    QObject *qobject() override { return this; }

    bool configureEndpoints(const QList<PSecureRtpEndpoint> &endpoints) override
    {
        return secureConfigureEndpoints(endpoints);
    }
    bool configureAssociation(const QByteArray &associationId, quint64 epoch, const QString &profile,
                              const QByteArray &localMasterKey, const QByteArray &localMasterSalt,
                              const QByteArray &remoteMasterKey, const QByteArray &remoteMasterSalt) override
    {
        return secureConfigureAssociation(associationId, epoch, profile, localMasterKey, localMasterSalt,
                                          remoteMasterKey, remoteMasterSalt);
    }
    void invalidateAssociation(const QByteArray &associationId, quint64 epoch) override
    {
        secureInvalidateAssociation(associationId, epoch);
    }
    bool associationReady(const QByteArray &associationId) const override
    {
        return secureAssociationReady(associationId);
    }
    quint64 associationEpoch(const QByteArray &associationId) const override
    {
        return secureAssociationEpoch(associationId);
    }
    SecureRtpSessionContext::Error lastError(const QByteArray &associationId) const override
    {
        return secureLastError(associationId);
    }
    void setProtectedPacketHandler(SecureRtpSessionContext::ProtectedPacketHandler handler) override
    {
        secureSetProtectedPacketHandler(std::move(handler));
    }
    void setRuntimeErrorHandler(SecureRtpSessionContext::RuntimeErrorHandler handler) override
    {
        secureSetRuntimeErrorHandler(std::move(handler));
    }
    bool receiveProtectedPacket(const PSecureRtpPacket &packet) override
    {
        return secureReceiveProtectedPacket(packet);
    }
};

} // namespace PsiMedia

#endif // PSIMEDIA_GSTRTPSESSIONCONTEXT_H
