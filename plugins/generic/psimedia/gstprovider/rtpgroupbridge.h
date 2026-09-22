/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef PSIMEDIA_RTPGROUPBRIDGE_H
#define PSIMEDIA_RTPGROUPBRIDGE_H

#include "rtpbundlerouter.h"
#include "rtpsessionbridge.h"

#include <QHash>
#include <QObject>
#include <QSet>

#include <functional>

namespace PsiMedia {

/**
 * One RFC 3550 session for one negotiated RTP/BUNDLE group.
 *
 * Endpoint codec branches remain independent, but RTP/RTCP scheduling,
 * participant/source state and RTCP ingress are shared. The class operates on
 * authenticated plain RTP/RTCP; SRTP belongs immediately outside this boundary.
 */
class RtpGroupBridge : public QObject {
public:
    struct Endpoint {
        QByteArray             id;
        QString                media;
        QList<PPayloadInfo>    localPayloads;
        QList<PPayloadInfo>    remotePayloads;
        RtpBundleRouter::Route route;
    };

    using NetworkPacketHandler = std::function<void(const PRtpPacket &)>;
    using MediaPacketHandler   = std::function<void(GstBuffer *)>;
    using RuntimeErrorHandler  = std::function<void()>;

    explicit RtpGroupBridge(QObject *parent = nullptr);
    ~RtpGroupBridge() override;

    RtpGroupBridge(const RtpGroupBridge &)            = delete;
    RtpGroupBridge &operator=(const RtpGroupBridge &) = delete;

    bool isValid() const { return session_.isValid(); }

    // Transactional route/PT update. The underlying rtpsession remains alive
    // while members are added/removed; surviving RTP/RTCP state is not reset.
    bool configure(const QList<Endpoint> &endpoints);
    bool removeEndpoint(const QByteArray &endpointId);
    void clear();

    void setNetworkPacketHandler(NetworkPacketHandler handler);
    void setEndpointMediaPacketHandler(const QByteArray &endpointId, MediaPacketHandler handler);
    void setRuntimeErrorHandler(RuntimeErrorHandler handler);

    bool start();
    void stop();

    // Outgoing endpoint RTP joins the shared session. The actual sender SSRC is
    // learned from the packet and retained across membership-only updates.
    GstFlowReturn sendRtp(const QByteArray &endpointId, GstBuffer *buffer,
                          GstClockTime presentationAge = GST_CLOCK_TIME_NONE);
    GstFlowReturn sendRtp(const QByteArray &endpointId, const PRtpPacket &packet);

    // Authenticated incoming plain RTP/RTCP. RTP must identify a configured
    // endpoint before it is allowed to mutate the shared rtpsession. RTCP is
    // validated structurally and enters the group session exactly once.
    GstFlowReturn receivePacket(const PRtpPacket &packet);

    bool requestRtcp(guint64 maxDelay = 0) { return session_.requestRtcp(maxDelay); }
    bool requestRemoteKeyframe(quint32 ssrc, quint8 payloadType)
    {
        return session_.requestRemoteKeyframe(ssrc, payloadType);
    }
    void          setRtcpMinimumInterval(guint64 interval) { session_.setRtcpMinimumInterval(interval); }
    quint64       receivedRtcpPackets() const { return session_.receivedRtcpPackets(); }
    GstStructure *sessionStats() const { return session_.sessionStats(); }

    int     endpointCount() const { return endpoints_.size(); }
    quint64 routeRevision() const { return router_.revision(); }

private:
    struct EndpointState {
        Endpoint     config;
        QSet<quint8> outgoingPayloadTypes;
    };

    bool ownerThread(const char *operation) const;
    bool registerOutgoing(const QByteArray &endpointId, const QByteArray &packet);
    void deliverIncomingRtp(GstBuffer *buffer);
    void emitRuntimeError();

    static QByteArray bytesFromBuffer(GstBuffer *buffer);
    static bool       rtpIdentity(const QByteArray &packet, quint8 *payloadType, quint32 *ssrc);

    RtpSessionBridge                      session_;
    RtpBundleRouter                       router_;
    QHash<QByteArray, EndpointState>      endpoints_;
    QHash<QByteArray, MediaPacketHandler> mediaHandlers_;
    NetworkPacketHandler                  networkPacketHandler_;
    RuntimeErrorHandler                   runtimeErrorHandler_;
};

} // namespace PsiMedia

#endif // PSIMEDIA_RTPGROUPBRIDGE_H
