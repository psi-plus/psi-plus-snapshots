/*
 * jingle-dtls-omemo.cpp - OMEMO-verified DTLS fingerprints in Jingle
 * Copyright (C) 2026  Sergey Ilinykh
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "jingle-dtls-omemo.h"

#include "xmpp_client.h"
#include "xmpp_encryption.h"
#include "xmpp_message.h"
#include "xmpp_omemo.h"

#include <QHash>
#include <QPointer>

namespace XMPP { namespace Jingle {
    namespace {
        const QString VerificationNs(QStringLiteral("http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification"));
        const QString DtlsNs(QStringLiteral("urn:xmpp:jingle:apps:dtls:0"));

        bool trusted(int level)
        {
            const auto value = EncryptionTrustLevel(level);
            return value == EncryptionTrustLevel::AutomaticallyTrusted || value == EncryptionTrustLevel::ManuallyTrusted
                || value == EncryptionTrustLevel::Authenticated;
        }

        QString key(const Jid &peer, const QString &sid) { return peer.bare() + QLatin1Char('\n') + sid; }
    }

    class OmemoDtlsVerification::Private {
    public:
        QPointer<Manager>         manager;
        QPointer<OmemoEncryption> omemo;
        QHash<QString, quint32>   devices;
        QDomDocument              document;
    };

    QString OmemoDtlsVerification::ns() { return VerificationNs; }

    QString OmemoDtlsVerification::methodId() { return OmemoEncryption::methodId(); }

    OmemoDtlsVerification::OmemoDtlsVerification(Manager *manager, OmemoEncryption *omemo, QObject *parent) :
        QObject(parent), d(std::make_unique<Private>())
    {
        Q_ASSERT(manager);
        d->manager = manager;
        d->omemo   = omemo;
        manager->registerDtlsVerification(this);
        connect(manager, &Manager::incomingMessageInitiation, this,
                [this](const XMPP::Message &message, const MessageInitiation &initiation) {
                    if (initiation.action() != MessageInitiation::Action::Proceed || !d->manager
                        || message.from().compare(d->manager->client()->jid(), false))
                        return;
                    for (const auto &extension : initiation.extensions()) {
                        bool       ok     = false;
                        const auto device = extension.attribute(QStringLiteral("id")).toUInt(&ok);
                        if (extension.localName() == QLatin1String("device")
                            && extension.namespaceURI() == VerificationNs && ok && device)
                            setPeerDevice(message.from(), initiation.id(), device);
                    }
                });
    }

    OmemoDtlsVerification::~OmemoDtlsVerification() = default;

    void OmemoDtlsVerification::setPeerDevice(const Jid &peer, const QString &sid, quint32 deviceId)
    {
        d->devices.insert(key(peer, sid), deviceId);
    }

    std::optional<quint32> OmemoDtlsVerification::peerDevice(const Jid &peer, const QString &sid) const
    {
        const auto it = d->devices.constFind(key(peer, sid));
        return it == d->devices.cend() ? std::nullopt : std::optional<quint32>(*it);
    }

    void OmemoDtlsVerification::encryptFingerprint(Session *session, const QDomElement &fingerprint,
                                                   const FingerprintDone &done)
    {
        if (!session || !session->allowsEncryptionMethod(OmemoEncryption::methodId()) || !d->omemo
            || !d->omemo->isReady()) {
            done({}, QStringLiteral("OMEMO is not available for this Jingle session"));
            return;
        }
        const auto device = peerDevice(session->peer(), session->sid());
        if (!device) {
            done({}, QStringLiteral("No OMEMO device was negotiated for the peer"));
            return;
        }
        const Jid  peer      = session->peer();
        const auto preferred = d->omemo->preferredProtocolFor(peer).value_or(OmemoProtocol::Legacy);
        auto       attempt   = std::make_shared<std::function<void(OmemoProtocol)>>();
        std::weak_ptr<std::function<void(OmemoProtocol)>> weakAttempt = attempt;
        QPointer<OmemoDtlsVerification> guard(this);
        *attempt = [guard, peer, device, fingerprint, done, weakAttempt](OmemoProtocol protocol) {
            auto attempt = weakAttempt.lock();
            if (!attempt)
                return;
            if (!guard || !guard->d->omemo) {
                if (guard)
                    done({}, QStringLiteral("The OMEMO encryption method was unregistered"));
                *attempt = nullptr;
                return;
            }
            auto *job = guard->d->omemo->encryptElement(peer, protocol, fingerprint, { *device });
            if (!job) {
                *attempt = nullptr;
                done({}, QStringLiteral("Cannot encrypt DTLS fingerprint"));
                return;
            }
            auto finish = [guard, job, fingerprint, done, attempt, protocol]() {
                job->deleteLater();
                if (!guard)
                    return;
                if (!job->success() && protocol == OmemoProtocol::Omemo2 && guard->d->omemo
                    && guard->d->omemo->supportedProtocols().testFlag(OmemoProtocol::Legacy)) {
                    (*attempt)(OmemoProtocol::Legacy);
                    return;
                }
                *attempt = nullptr;
                if (!job->success()) {
                    done({}, job->errorString());
                    return;
                }
                QDomDocument document;
                auto         verified = document.createElementNS(VerificationNs, QStringLiteral("fingerprint"));
                for (const auto &attribute : { QStringLiteral("hash"), QStringLiteral("setup") })
                    if (fingerprint.hasAttribute(attribute))
                        verified.setAttribute(attribute, fingerprint.attribute(attribute));
                verified.appendChild(document.importNode(job->stanza(), true));
                document.appendChild(verified);
                done(verified, {});
            };
            if (job->isFinished())
                finish();
            else
                QObject::connect(job, &EncryptionJob::finished, guard, finish);
        };
        (*attempt)(preferred);
    }

    void OmemoDtlsVerification::decryptFingerprint(Session *session, const QDomElement &fingerprint,
                                                   const VerificationDone &done)
    {
        if (!session || !session->allowsEncryptionMethod(OmemoEncryption::methodId()) || !d->omemo) {
            done({}, {}, QStringLiteral("This session cannot verify OMEMO DTLS fingerprints"));
            return;
        }
        QDomElement encrypted;
        for (auto child = fingerprint.firstChildElement(QStringLiteral("encrypted")); !child.isNull();
             child      = child.nextSiblingElement(QStringLiteral("encrypted"))) {
            if (child.namespaceURI() == OmemoEncryption::namespaceUri()
                || child.namespaceURI() == OmemoEncryption::legacyNamespaceUri()) {
                if (!encrypted.isNull()) {
                    done({}, {}, QStringLiteral("Ambiguous encrypted DTLS fingerprint"));
                    return;
                }
                encrypted = child;
            }
        }
        if (encrypted.isNull()) {
            done({}, {}, QStringLiteral("No OMEMO envelope in DTLS fingerprint"));
            return;
        }
        auto *job = d->omemo->decryptElement(session->peer(), encrypted);
        if (!job) {
            done({}, {}, QStringLiteral("Cannot decrypt DTLS fingerprint"));
            return;
        }
        QPointer<OmemoDtlsVerification> guard(this);
        QPointer<Session>               current(session);
        const auto                      finish = [guard, current, fingerprint, job, done]() {
            job->deleteLater();
            if (!guard || !current)
                return;
            if (!job->success()) {
                done({}, {}, job->errorString());
                return;
            }
            const auto                 &meta = job->metadata();
            Session::PeerAuthentication authentication;
            authentication.method      = OmemoEncryption::methodId();
            authentication.deviceId    = meta.senderDeviceId;
            authentication.identityKey = meta.senderKey;
            authentication.trustLevel  = meta.details.value(QStringLiteral("trustLevel")).toInt();
            const auto existing        = current->peerAuthentication();
            if (!authentication.deviceId || !authentication.isValid()
                || (existing.isValid()
                    && (existing.method != authentication.method || existing.deviceId != authentication.deviceId
                        || existing.identityKey != authentication.identityKey))
                || (current->isEndToEndRequired() && !trusted(authentication.trustLevel))) {
                done({}, {}, QStringLiteral("The peer's OMEMO identity is not acceptable"));
                return;
            }
            const auto   plain = job->stanza();
            QDomDocument document;
            QDomElement  result;
            if (plain.namespaceURI() == DtlsNs && plain.localName() == QLatin1String("fingerprint")) {
                result = document.importNode(plain, true).toElement();
            } else {
                result = document.createElementNS(DtlsNs, QStringLiteral("fingerprint"));
                for (const auto &name : { QStringLiteral("hash"), QStringLiteral("setup") })
                    if (fingerprint.hasAttribute(name))
                        result.setAttribute(name, fingerprint.attribute(name));
                result.appendChild(document.createTextNode(plain.text().trimmed()));
            }
            document.appendChild(result);
            // ICE validates the decrypted DTLS fingerprint before committing
            // either its parameters or the peer's authenticated identity.
            done(result, authentication, {});
        };
        if (job->isFinished())
            finish();
        else
            connect(job, &EncryptionJob::finished, this, finish);
    }

    void OmemoDtlsVerification::decorateMessageInitiation(const Jid &to, MessageInitiation &initiation)
    {
        Q_UNUSED(to)
        if (initiation.action() != MessageInitiation::Action::Proceed || !d->omemo || !d->omemo->isReady())
            return;
        auto device = d->document.createElementNS(VerificationNs, QStringLiteral("device"));
        device.setAttribute(QStringLiteral("id"), QString::number(d->omemo->ownDeviceId()));
        initiation.addExtension(device);
    }
}} // namespace XMPP::Jingle
