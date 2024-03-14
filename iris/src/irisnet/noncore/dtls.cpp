/*
 * Copyright (C) 2021  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "dtls.h"
#include "xmpp_xmlcommon.h"

#include <array>

#include <QtCrypto>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <QAbstractSocket>

#define DTLS_DEBUG(msg, ...) qDebug("dtls: " msg, ##__VA_ARGS__)

/*
Connection flow

                    juliet                   |                   romeo
---------------------------------------------------------------------------------------
1 |             setup:NotSet                 |               setup:NotSet
2 |  generate cert + compute fingerprint     |
3 |             setup:actpass                |
4 |                             ----send fingerprint----> validate
5 |                             <-------iq result--------
5 |                                          |               setup:active
6 |                                          |  generate cert + compute fingerprint
7 |               validate      <---send fingerprint-----
8 |            setup:passive                 |
9 |          start dtls server               |
10|                             --------iq result------->
11|                                          |             start dtls client
12|================================= DTLS HANDSHAKE ===================================
*/

namespace XMPP {

static std::array<const char *, 4> fpRoles { { "active", "passive", "actpass", "holdconn" } };

QString Dtls::FingerPrint::ns() { return QStringLiteral("urn:xmpp:jingle:apps:dtls:0"); }

bool Dtls::FingerPrint::parse(const QDomElement &el)
{
    if (el.namespaceURI() != ns()) {
        qWarning("Unrecognized DTLS xmlns: %s. Parse it as if it were %s", qPrintable(el.namespaceURI()),
                 qPrintable(ns()));
    }
    auto ht = el.attribute(QLatin1String("hash"));
    hash    = QStringView { ht };
    hash.setData(QByteArray::fromHex(el.text().toLatin1()));
    auto setupIt = std::find(fpRoles.begin(), fpRoles.end(), el.attribute(QLatin1String("setup")).toLatin1());
    setup        = Setup(setupIt == fpRoles.end() ? NotSet : std::distance(fpRoles.begin(), setupIt) + 1);
    return isValid();
}

QDomElement Dtls::FingerPrint::toXml(QDomDocument *doc) const
{
    Q_ASSERT(setup != NotSet);
    auto binToHex = [](const QByteArray &in) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
        return in.toHex(':');
#else
        QByteArray out  = in.toHex();
        int        size = out.size();
        for (int k = 2; k < size; k += 3, ++size) {
            out.insert(k, ':');
        }
        return out;
#endif
    };
    auto fingerprint
        = XMLHelper::textTagNS(doc, ns(), QLatin1String("fingerprint"), QString::fromLatin1(binToHex(hash.data())));
    fingerprint.setAttribute(QLatin1String("hash"), hash.stringType());
    fingerprint.setAttribute(QLatin1String("setup"), QLatin1String(fpRoles[setup - 1]));
    return fingerprint;
}

class Dtls::Private : public QObject {
    Q_OBJECT
public:
    Dtls            *q;
    QCA::TLS        *tls = nullptr;
    QCA::PrivateKey  pkey;
    QCA::Certificate cert;

    QString localJid;
    QString remoteJid;
    // original fingerprints
    FingerPrint localFingerprint;
    FingerPrint remoteFingerprint;

    QAbstractSocket::SocketError lastError = QAbstractSocket::UnknownSocketError;

    Private(Dtls *q) : QObject(q), q(q) { }

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
            if (computeFingerprint(cert, remoteFingerprint.hash.type()) == remoteFingerprint.hash) {
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

    void setRemoteFingerprint(const FingerPrint &fp)
    {
        bool needRestart = false;
        if (tls) {
            if (remoteFingerprint == fp)
                return;
            // need to restart dtls. see rfc8842 (todo: but in fact we need more checks)
            needRestart            = true;
            localFingerprint.setup = Dtls::NotSet;
        }
        remoteFingerprint = fp;
        if (needRestart)
            emit q->needRestart();
        if (localFingerprint.setup == Dtls::NotSet)
            return; // will be handled in acceptIncoming

        bool remoteActiveOrPassive
            = remoteFingerprint.setup == Dtls::Passive || remoteFingerprint.setup == Dtls::Active;
        if (localFingerprint.setup == Dtls::ActPass) { // response fingerprint
            if (!remoteActiveOrPassive) {
                qWarning("Unexpected remote fingerprint setup. Assume remote setup=active");
                remoteFingerprint.setup = Dtls::Active;
            }
            localFingerprint.setup = remoteFingerprint.setup == Dtls::Active ? Dtls::Passive : Dtls::Active;
            if (localFingerprint.setup == Dtls::Passive)
                negotiate();
            return;
        }
        // local is active or passive already, no idea in what scenario. probably something custom
        bool roleConflict = remoteFingerprint.setup == localFingerprint.setup;
        if (!roleConflict && !remoteActiveOrPassive) {
            if (localFingerprint.setup == Dtls::Passive)
                negotiate();
            return; // looks valid
        }
        if (roleConflict)
            qWarning("setRemoteFingerprint: dtls role conflict");
        if (!remoteActiveOrPassive)
            qWarning("setRemoteFingerprint: expected active or passive remote fingerprint but got something else");
        lastError = QAbstractSocket::OperationError;
        emit q->errorOccurred(lastError);
    }

    void acceptIncoming()
    {
        if (cert.isNull()) {
            generateCertificate();
        }
        Q_ASSERT(localFingerprint.setup == Dtls::NotSet);
        if (remoteFingerprint.setup == Dtls::ActPass) {
            localFingerprint.setup  = Dtls::Active;
            remoteFingerprint.setup = Dtls::Passive;
        } else {
            localFingerprint.setup = remoteFingerprint.setup == Dtls::Active ? Dtls::Passive : Dtls::Active;
        }
        if (localFingerprint.setup == Dtls::Passive) {
            negotiate(); // start server
        }
    }

    void negotiate()
    {
        if (tls) {
            delete tls;
        }

        if (!remoteFingerprint.isValid()) {
            qWarning("remote fingerprint is not set");
            lastError = QAbstractSocket::SocketError::OperationError;
            emit q->errorOccurred(lastError);
            return;
        }

        tls = new QCA::TLS(QCA::TLS::Datagram);
        tls->setCertificate(cert, pkey);

        connect(tls, &QCA::TLS::certificateRequested, tls, &QCA::TLS::continueAfterStep);
        connect(tls, &QCA::TLS::handshaken, this, &Dtls::Private::tls_handshaken);
        connect(tls, &QCA::TLS::readyRead, q, &Dtls::readyRead);
        connect(tls, &QCA::TLS::readyReadOutgoing, q, &Dtls::readyReadOutgoing);
        connect(tls, &QCA::TLS::closed, q, &Dtls::closed);
        connect(tls, &QCA::TLS::error, this, &Dtls::Private::tls_error);

        if (localFingerprint.setup == Dtls::Passive) {
            qDebug("Starting DTLS server");
            tls->startServer();
        } else {
            qDebug("Starting DTLS client");
            tls->startClient();
        }
    }

    void generateCertificate()
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

        QCA::Constraints constraints = { { QCA::DigitalSignature, QCA::KeyEncipherment, QCA::DataEncipherment,
                                           QCA::ClientAuth, QCA::ServerAuth } };
        opts.setConstraints(constraints);
        opts.setAsCA();

        pkey                  = QCA::KeyGenerator().createRSA(2048);
        cert                  = QCA::Certificate(opts, pkey);
        localFingerprint.hash = Private::computeFingerprint(cert, Hash::Sha256);
    }
};

Dtls::Dtls(QObject *parent, const QString &localJid, const QString &remoteJid) : QObject(parent), d(new Private(this))
{
    d->localJid  = localJid;
    d->remoteJid = remoteJid;
    if (!isSupported()) {
        qWarning("DTLS is not supported by your version of QCA");
    }
}

void Dtls::setLocalCertificate(const QCA::Certificate &cert, const QCA::PrivateKey &pkey)
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
    d->localFingerprint.hash = Private::computeFingerprint(cert, hashType);
}

QCA::Certificate Dtls::localCertificate() const { return d->cert; }

QCA::Certificate Dtls::remoteCertificate() const
{
    if (!d->tls || d->tls->peerCertificateChain().isEmpty())
        return {};
    auto const &chain = d->tls->peerCertificateChain();
    return chain.first();
}

void Dtls::initOutgoing()
{
    if (d->cert.isNull()) {
        d->generateCertificate();
    }
    d->localFingerprint.setup = ActPass;
}

void Dtls::acceptIncoming() { d->acceptIncoming(); }

void Dtls::onRemoteAcceptedFingerprint()
{
    if (d->localFingerprint.setup == Active) {
        d->negotiate();
    }
}

const Dtls::FingerPrint &Dtls::localFingerprint() const { return d->localFingerprint; }

const Dtls::FingerPrint &Dtls::remoteFingerprint() const { return d->remoteFingerprint; }

void Dtls::setRemoteFingerprint(const FingerPrint &fp) { d->setRemoteFingerprint(fp); }

QAbstractSocket::SocketError Dtls::error() const { return d->lastError; }

void Dtls::negotiate() { d->negotiate(); }

bool Dtls::isStarted() const { return d->tls != nullptr; }

bool Dtls::isSupported() { return QCA::isSupported("dtls"); }

QByteArray Dtls::readDatagram()
{
    if (!d->tls) {
        DTLS_DEBUG("negotiation hasn't started yet. ignore readDatagram");
        return {};
    }
    QByteArray a = d->tls->read();
    // DTLS_DEBUG("read %d bytes of decrypted data", a.size());
    return a;
}

QByteArray Dtls::readOutgoingDatagram()
{
    if (!d->tls) {
        DTLS_DEBUG("negotiation hasn't started yet. ignore readOutgoingDatagram");
        return {};
    }
    auto ba = d->tls->readOutgoing();
    // DTLS_DEBUG("read outgoing packet of %d bytes", ba.size());
    return ba;
}

void Dtls::writeDatagram(const QByteArray &data)
{
    // DTLS_DEBUG("write %d bytes for encryption\n", data.size());
    if (!d->tls) {
        DTLS_DEBUG("negotiation hasn't started yet. ignore writeDatagram");
        return;
    }
    d->tls->write(data);
}

void Dtls::writeIncomingDatagram(const QByteArray &data)
{
    // DTLS_DEBUG("write incoming %d bytes for decryption\n", data.size());
    if (!d->tls) {
        DTLS_DEBUG("negotiation hasn't started yet. ignore incoming datagram");
        return;
    }
    d->tls->writeIncoming(data);
}

} // namespace XMPP

#include "dtls.moc"
