#ifndef XMPP_DTLS_H
#define XMPP_DTLS_H

#include <QAbstractSocket>

#include "xmpp_hash.h"

namespace QCA {
class Certificate;
class PrivateKey;
}

namespace XMPP {

class Dtls : public QObject {
    Q_OBJECT
public:
    explicit Dtls(QObject *parent = nullptr);

    // jid is used as subjectAltName XmppAddr identifier
    void generateCertificate(const QString &localJid = QString());
    void setCertificate(const QCA::Certificate &cert, const QCA::PrivateKey &pkey);
    Hash fingerprint();
    void setRemoteFingerprint(const Hash &fingerprint);

    QAbstractSocket::SocketError error() const;

    QByteArray readDatagram();
    QByteArray readOutgoingDatagram();
    void       writeDatagram(const QByteArray &data);
    void       writeIncomingDatagram(const QByteArray &data);

    void startServer();
    void startClient();
signals:
    void readyRead();
    void readyReadOutgoing();
    void connected();
    void errorOccurred(QAbstractSocket::SocketError);
    void closed();

private:
    class Private;
    Private *d;
};

} // namespace XMPP

#endif // XMPP_DTLS_H
