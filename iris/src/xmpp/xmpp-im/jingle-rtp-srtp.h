// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_SRTP_H
#define JINGLE_RTP_SRTP_H

#include <QObject>
#include <QPointer>
#include <QtCrypto>

#include <iris/iris_export.h>

namespace XMPP {
class Dtls;
}

namespace XMPP::Jingle::RTP {

// RFC 7983 classification only. RTP/RTCP headers are not authenticated here and
// must never be used for content routing before the media backend verifies SRTP.
enum class DatagramKind { Unknown, Stun, Zrtp, Dtls, Turn, Rtp, Rtcp };
IRIS_EXPORT DatagramKind classifyDatagram(const QByteArray &);

enum class PacketKind : quint8 { Rtp, Rtcp };

struct IRIS_EXPORT SecureRtpKeyingMaterial {
    QString          profile;
    QCA::SecureArray localMasterKey;
    QCA::SecureArray localMasterSalt;
    QCA::SecureArray remoteMasterKey;
    QCA::SecureArray remoteMasterSalt;

    bool isValid() const
    {
        return !profile.isEmpty() && localMasterKey.size() > 0 && localMasterSalt.size() > 0
            && remoteMasterKey.size() > 0 && remoteMasterSalt.size() > 0;
    }

    void clear()
    {
        profile.clear();
        localMasterKey   = QCA::SecureArray();
        localMasterSalt  = QCA::SecureArray();
        remoteMasterKey  = QCA::SecureArray();
        remoteMasterSalt = QCA::SecureArray();
    }
};

// One authenticated DTLS-SRTP association / BUNDLE group. Iris owns DTLS,
// fingerprint verification, RFC 7983/5761 muxing and security epochs. Packet
// encryption/authentication is deliberately not implemented here; key material
// and protected media cross the backend-neutral media-session boundary.
class IRIS_EXPORT SecureRtpAssociation : public QObject {
    Q_OBJECT
public:
    SecureRtpAssociation(XMPP::Dtls *dtls, QByteArray associationId, QObject *parent = nullptr);
    ~SecureRtpAssociation() override;

    void                      close();
    bool                      isReady() const;
    const QByteArray         &associationId() const { return associationId_; }
    quint64                   epoch() const { return epoch_; }
    const SecureRtpKeyingMaterial &keyingMaterial() const { return material_; }

    // Ingress from one RTP/RTCP-mux ICE component. DTLS is consumed locally;
    // protected RTP/SRTCP is emitted unchanged for the media backend.
    bool receiveMuxed(QByteArray);

    // Validate backend-produced protected media before the owning transport
    // writes it to ICE. This does not authenticate packet contents.
    bool validateProtectedMuxed(const QByteArray &, PacketKind, quint64 epoch) const;

signals:
    void protectedPacketReceived(const QByteArray &, XMPP::Jingle::RTP::PacketKind, quint64 epoch);
    void ready(quint64 epoch);
    void invalidated(quint64 epoch);

private:
    void activate();
    void invalidate();

    QPointer<XMPP::Dtls>     dtls_;
    QByteArray               associationId_;
    SecureRtpKeyingMaterial  material_;
    quint64                  epoch_  = 0;
    bool                     active_ = false;
};

// Implemented by transports offering authenticated RTP/RTCP mux. The transport
// owns the association and protected network boundary; the media backend owns
// SRTP/SRTCP crypto.
class IRIS_EXPORT PacketTransport {
public:
    virtual ~PacketTransport() = default;
    virtual bool enableRtpMux(const QStringList &profiles) = 0;
    virtual SecureRtpAssociation *rtpAssociation() const = 0;
    virtual bool sendProtectedRtpPacket(QByteArray, PacketKind, quint64 epoch) = 0;
};

} // namespace XMPP::Jingle::RTP

Q_DECLARE_METATYPE(XMPP::Jingle::RTP::PacketKind)

#endif
