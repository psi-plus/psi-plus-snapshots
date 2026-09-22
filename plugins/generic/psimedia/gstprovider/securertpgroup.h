/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef PSIMEDIA_SECURERTPGROUP_H
#define PSIMEDIA_SECURERTPGROUP_H

#include "rtpgroupbridge.h"
#include "securertpcontext.h"

#include <QObject>

#include <functional>

namespace PsiMedia {

/**
 * SRTP/SRTCP protection boundary for one DTLS association / RTP group.
 *
 * Plain RTP/RTCP exists only on the media side of this object. Network callbacks
 * carry protected packets tagged with the current opaque association identity
 * and security epoch.
 */
class SecureRtpGroup : public QObject {
public:
    using ProtectedPacketHandler = std::function<void(const PSecureRtpPacket &)>;
    using MediaPacketHandler     = RtpGroupBridge::MediaPacketHandler;
    using RuntimeErrorHandler    = std::function<void(SecureRtpSessionContext::Error)>;

    explicit SecureRtpGroup(QObject *parent = nullptr);
    ~SecureRtpGroup() override;

    SecureRtpGroup(const SecureRtpGroup &)            = delete;
    SecureRtpGroup &operator=(const SecureRtpGroup &) = delete;

    bool isValid() const { return bridge_.isValid(); }

    // RTP route/PT membership is independent from the crypto epoch.
    bool configureEndpoints(const QList<RtpGroupBridge::Endpoint> &endpoints);
    bool removeEndpoint(const QByteArray &endpointId);
    void clearEndpoints();

    // Repeated activation with identical association/profile/material may advance
    // epoch without resetting libSRTP replay/ROC/SRTCP-index state.
    bool activate(const QByteArray &associationId, quint64 epoch, const QString &profile,
                  const QByteArray &localMasterKey, const QByteArray &localMasterSalt,
                  const QByteArray &remoteMasterKey, const QByteArray &remoteMasterSalt);
    void invalidate(const QByteArray &associationId, quint64 epoch);

    bool                           isReady() const { return crypto_.isReady(); }
    QByteArray                     associationId() const { return crypto_.associationId(); }
    quint64                        epoch() const { return crypto_.epoch(); }
    SecureRtpSessionContext::Error lastError() const
    {
        return mediaError_ == SecureRtpSessionContext::Error::None ? crypto_.lastError() : mediaError_;
    }

    void setProtectedPacketHandler(ProtectedPacketHandler handler);
    void setEndpointMediaPacketHandler(const QByteArray &endpointId, MediaPacketHandler handler);
    void setRuntimeErrorHandler(RuntimeErrorHandler handler);

    bool start();
    void stop();

    GstFlowReturn sendRtp(const QByteArray &endpointId, GstBuffer *buffer,
                          GstClockTime presentationAge = GST_CLOCK_TIME_NONE);
    GstFlowReturn sendRtp(const QByteArray &endpointId, const PRtpPacket &packet);

    // Returns false for authentication/replay/malformed/stale packets as well as
    // fatal crypto failures. lastError() distinguishes the reason.
    bool receiveProtectedPacket(const PSecureRtpPacket &packet);

    bool requestRtcp(guint64 maxDelay = 0) { return bridge_.requestRtcp(maxDelay); }
    bool requestRemoteKeyframe(quint32 ssrc, quint8 payloadType)
    {
        return bridge_.requestRemoteKeyframe(ssrc, payloadType);
    }
    void          setRtcpMinimumInterval(guint64 interval) { bridge_.setRtcpMinimumInterval(interval); }
    quint64       receivedRtcpPackets() const { return bridge_.receivedRtcpPackets(); }
    GstStructure *sessionStats() const { return bridge_.sessionStats(); }

private:
    bool        ownerThread(const char *operation) const;
    void        protectOutgoing(const PRtpPacket &packet);
    void        reportCryptoFailure(SecureRtpSessionContext::Error error);
    static bool fatalCryptoError(SecureRtpSessionContext::Error error);

    SrtpAssociation                crypto_;
    RtpGroupBridge                 bridge_;
    ProtectedPacketHandler         protectedPacketHandler_;
    RuntimeErrorHandler            runtimeErrorHandler_;
    SecureRtpSessionContext::Error mediaError_ = SecureRtpSessionContext::Error::None;
};

} // namespace PsiMedia

#endif // PSIMEDIA_SECURERTPGROUP_H
