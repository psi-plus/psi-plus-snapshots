// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_SRTP_H
#define JINGLE_RTP_SRTP_H
#include <QObject>
#include <QPointer>
#include <QtCrypto>
#include <iris/iris_export.h>
#include <memory>
#include <optional>

namespace XMPP {
class Dtls;
}

namespace XMPP::Jingle::RTP {
// RFC 7983 classification only, not packet authentication or validation.
enum class DatagramKind { Unknown, Stun, Zrtp, Dtls, Turn, Rtp, Rtcp };
IRIS_EXPORT DatagramKind classifyDatagram(const QByteArray &);
// Runtime DTLS-SRTP profiles usable by both the currently loaded QCA provider
// and the linked libSRTP backend. An empty list means native secure RTP is
// unavailable; plain DTLS may still be usable for SCTP/data channels. Callers
// should snapshot this result when deciding which media capabilities to expose.
IRIS_EXPORT QStringList supportedSecureRtpProfiles();
// One bidirectional protection context per authenticated DTLS association/BUNDLE
// group, not per content or peer JID. All access must be serialized by the owner.
// This class does not authenticate signaling or fingerprints: the caller may
// configure it ONLY with verified DTLS keying material and must reset it on
// security invalidation. It must also discard queued media from the old epoch.
class IRIS_EXPORT SrtpContext {
public:
    enum class Packet { Rtp, Rtcp };
    enum class Error {
        None,
        NotReady,
        UnsupportedProfile,
        InvalidKey,
        InvalidPacket,
        Authentication,
        Replay,
        StreamLimit,
        LibraryFailure
    };
    SrtpContext();
    ~SrtpContext();
    SrtpContext(const SrtpContext &)                  = delete;
    SrtpContext       &operator=(const SrtpContext &) = delete;
    static QStringList supportedProfiles();
    // Reapplying identical active keys is idempotent and retains replay windows.
    // Any failed reconfiguration invalidates the previous context.
    bool  configure(const QString &profile, const QCA::SecureArray &localKey, const QCA::SecureArray &localSalt,
                    const QCA::SecureArray &remoteKey, const QCA::SecureArray &remoteSalt);
    void  reset();
    bool  isReady() const;
    Error lastError() const;
    std::optional<QByteArray> protect(QByteArray, Packet);
    std::optional<QByteArray> unprotect(QByteArray, Packet);

private:
    class Private;
    std::unique_ptr<Private>  d;
    std::optional<QByteArray> process(QByteArray, Packet, bool sending);
};

// Bound to one DTLS association, on the same thread. Does not own DTLS.
// Packet queues must retain epoch() with each packet and discard packets on
// invalidated(). A new authenticated epoch never consumes an old queued packet.
class IRIS_EXPORT SrtpSession : public QObject {
    Q_OBJECT
public:
    explicit SrtpSession(XMPP::Dtls *dtls, QObject *parent = nullptr);
    // Permanently detach when the owning transport is stopped.
    void                      close();
    bool                      isReady() const;
    quint64                   epoch() const { return epoch_; }
    std::optional<QByteArray> protect(QByteArray, SrtpContext::Packet, quint64 epoch);
    std::optional<QByteArray> unprotect(QByteArray, SrtpContext::Packet, quint64 epoch);
    struct ReceivedPacket {
        QByteArray          data;
        SrtpContext::Packet kind;
        quint64             epoch;
    };
    // For an explicitly negotiated RTP/RTCP-mux ICE component, after STUN/TURN
    // processing. DTLS goes to QCA; only authenticated media is returned.
    std::optional<ReceivedPacket> receiveMuxed(QByteArray);
    void                          dispatchMuxed(QByteArray);
    std::optional<QByteArray>     protectMuxed(QByteArray, SrtpContext::Packet, quint64 epoch);
signals:
    void packetReceived(const QByteArray &, XMPP::Jingle::RTP::SrtpContext::Packet, quint64 epoch);
    void ready();
    void invalidated();

private:
    void                 activate();
    void                 invalidate();
    QPointer<XMPP::Dtls> dtls_;
    SrtpContext          context_;
    quint64              epoch_ = 0;
};
}
Q_DECLARE_METATYPE(XMPP::Jingle::RTP::SrtpContext::Packet)
namespace XMPP::Jingle::RTP {
// Implemented by transports offering authenticated RTP/RTCP mux. No ICE types
// cross this boundary. The binding belongs to the transport, not the caller.
class IRIS_EXPORT PacketTransport {
public:
    virtual ~PacketTransport()                                                         = default;
    virtual bool         enableRtpMux()                                                = 0;
    virtual SrtpSession *rtpSession() const                                            = 0;
    virtual bool         sendRtpPacket(QByteArray, SrtpContext::Packet, quint64 epoch) = 0;
};
}
#endif
