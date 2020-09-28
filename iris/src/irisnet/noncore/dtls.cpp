#include "dtls.h"
#include <QtCrypto>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <QAbstractSocket>

#define DTLS_DEBUG qDebug

namespace XMPP {

class Dtls::Private : public QObject {
    Q_OBJECT
public:
    Dtls *    q;
    QCA::TLS *tls;

    Hash                         fingerprint;
    Hash                         remoteFingerprint;
    QAbstractSocket::SocketError lastError = QAbstractSocket::UnknownSocketError;

    Private(Dtls *q) : QObject(q), q(q)
    {
        tls = new QCA::TLS(QCA::TLS::Datagram);

        connect(tls, &QCA::TLS::certificateRequested, tls, &QCA::TLS::continueAfterStep);
        connect(tls, &QCA::TLS::handshaken, this, &Dtls::Private::tls_handshaken);
        connect(tls, &QCA::TLS::readyRead, q, &Dtls::readyRead);
        connect(tls, &QCA::TLS::readyReadOutgoing, q, &Dtls::readyReadOutgoing);
        connect(tls, &QCA::TLS::closed, q, &Dtls::closed);
        connect(tls, &QCA::TLS::error, this, &Dtls::Private::tls_error);
    }

    static Hash computeFingerprint(const QCA::Certificate &cert, Hash::Type hashType)
    {
        if (cert.isNull()) {
            return Hash();
        }
        return Hash::from(hashType, cert.toDER());
    }

    void tls_handshaken()
    {
        DTLS_DEBUG("tls handshaken");
        auto peerIdentity = tls->peerIdentityResult();
        if (peerIdentity == QCA::TLS::Valid || peerIdentity == QCA::TLS::InvalidCertificate) {
            const auto  chain = tls->peerCertificateChain();
            const auto &cert  = chain.first();
            if (computeFingerprint(cert, remoteFingerprint.type()) == remoteFingerprint) {
                DTLS_DEBUG("valid");
                tls->continueAfterStep();
                emit q->connected();
                return;
            } else {
                qWarning("dtls fingerprints do not match: %d", int(tls->peerIdentityResult()));
            }
        } else {
            qWarning("dtls peerIdentity failure: %d", int(tls->peerIdentityResult()));
        }

        lastError = QAbstractSocket::SslHandshakeFailedError;
        tls->reset();
        emit q->errorOccurred(lastError);
    }

    void tls_error()
    {
        DTLS_DEBUG("tls error: %d", tls->errorCode());
        switch (tls->errorCode()) {
        case QCA::TLS::ErrorSignerExpired:
        case QCA::TLS::ErrorSignerInvalid:
        case QCA::TLS::ErrorCertKeyMismatch:
            lastError = QAbstractSocket::SocketError::SslInvalidUserDataError;
            break;
        case QCA::TLS::ErrorInit:
            lastError = QAbstractSocket::SocketError::SslInternalError;
            break;
        case QCA::TLS::ErrorHandshake:
            lastError = QAbstractSocket::SocketError::SslHandshakeFailedError;
            break;
        case QCA::TLS::ErrorCrypt:
        default:
            lastError = QAbstractSocket::SocketError::UnknownSocketError;
            break;
        }
        emit q->errorOccurred(lastError);
    }
};

Dtls::Dtls(QObject *parent) : QObject(parent), d(new Private(this))
{
    if (!d->tls->context()) {
        qWarning("DTLS is not supported by your version of QCA");
    }
}

void Dtls::generateCertificate(const QString &localJid)
{
    QCA::CertificateOptions opts;

    QCA::CertificateInfo info;
    info.insert(QCA::CommonName, QStringLiteral("iris.psi-im.org"));
    if (!localJid.isEmpty())
        info.insert(QCA::XMPP, localJid);
    opts.setInfo(info);

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    QCA::BigInteger sn(QRandomGenerator::global()->generate());
#else
    QCA::BigInteger sn(qrand());
#endif
    opts.setSerialNumber(sn);

    auto nowUTC = QDateTime::currentDateTimeUtc();
    opts.setValidityPeriod(nowUTC, nowUTC.addDays(30));

    QCA::Constraints constraints
        = { { QCA::DigitalSignature, QCA::KeyEncipherment, QCA::DataEncipherment, QCA::ClientAuth, QCA::ServerAuth } };
    opts.setConstraints(constraints);
    opts.setAsCA();

    auto pkey = QCA::KeyGenerator().createRSA(2048);
    auto cert = QCA::Certificate(opts, pkey);
    d->tls->setCertificate(cert, pkey);
    d->fingerprint = Private::computeFingerprint(cert, Hash::Sha256);
}

void Dtls::setCertificate(const QCA::Certificate &cert, const QCA::PrivateKey &pkey)
{
    Hash::Type hashType = Hash::Sha256;
    switch (cert.signatureAlgorithm()) {
    case QCA::EMSA1_SHA1:
    case QCA::EMSA3_SHA1:
        hashType = Hash::Sha1;
        break;
    case QCA::EMSA3_SHA256:
        hashType = Hash::Sha256;
        break;
    case QCA::EMSA3_SHA512:
        hashType = Hash::Sha512;
        break;
    default:
        break;
    }

    d->tls->setCertificate(cert, pkey);
    d->fingerprint = Private::computeFingerprint(cert, hashType);
}

Hash Dtls::fingerprint() { return d->fingerprint; }

void Dtls::setRemoteFingerprint(const Hash &fingerprint) { d->remoteFingerprint = fingerprint; }

QAbstractSocket::SocketError Dtls::error() const { return d->lastError; }

void Dtls::startServer() { d->tls->startServer(); }

void Dtls::startClient() { d->tls->startClient(); }

QByteArray Dtls::readDatagram()
{
    QByteArray a = d->tls->read();
    DTLS_DEBUG("dtls: read %d bytes of decrypted data", a.size());
    return a;
}

QByteArray Dtls::readOutgoingDatagram()
{
    auto ba = d->tls->readOutgoing();
    DTLS_DEBUG("dtls: read outgoing packet of %d bytes", ba.size());
    return ba;
}

void Dtls::writeDatagram(const QByteArray &data)
{
    DTLS_DEBUG("dtls: write %d bytes for encryption\n", data.size());
    d->tls->write(data);
}

void Dtls::writeIncomingDatagram(const QByteArray &data)
{
    DTLS_DEBUG("dtls: write incoming %d bytes for decryption\n", data.size());
    d->tls->writeIncoming(data);
}

} // namespace XMPP

#include "dtls.moc"
