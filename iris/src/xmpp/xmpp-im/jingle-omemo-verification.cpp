/*
 * jingle-omemo-verification.cpp - OMEMO-verified DTLS fingerprints in Jingle
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

#include "jingle-omemo-verification.h"

#include "jingle-ice.h"
#include "jingle-session.h"
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
        const QString OmemoMethod(QStringLiteral("omemo"));

        struct Fingerprint {
            QDomElement transport;
            QDomElement element;
        };

        struct Scan {
            QList<Fingerprint> plain;               // DTLS fingerprints of ICE transports
            QList<Fingerprint> encrypted;           // OMEMO-verified ones
            bool               unprotected = false; // a transport without DTLS
        };

        bool isIce(const QDomElement &transport)
        {
            return transport.namespaceURI() == ICE::NS || transport.namespaceURI() == ICE::NS_ICE_UDP;
        }

        Scan scan(const QDomElement &jingle)
        {
            Scan result;
            for (auto content = jingle.firstChildElement(QStringLiteral("content")); !content.isNull();
                 content      = content.nextSiblingElement(QStringLiteral("content"))) {
                const auto transport = content.firstChildElement(QStringLiteral("transport"));
                if (transport.isNull())
                    continue;
                // Contents protected by JET (files) are end-to-end encrypted on any transport.
                bool jet = false;
                for (auto sec = content.firstChildElement(QStringLiteral("security")); !sec.isNull();
                     sec      = sec.nextSiblingElement(QStringLiteral("security")))
                    jet = jet || sec.namespaceURI() == QLatin1String("urn:xmpp:jingle:jet:0");
                if (jet)
                    continue;
                if (!isIce(transport)) {
                    // SXE payload protected end to end (SxeOmemo) is protected on its message transport.
                    if (!(transport.namespaceURI() == QLatin1String("urn:xmpp:jingle:transports:sxe")
                          && transport.hasAttribute(QStringLiteral("encryption"))))
                        result.unprotected = true;
                    continue;
                }
                bool dtls = false;
                for (auto fp = transport.firstChildElement(QStringLiteral("fingerprint")); !fp.isNull();
                     fp      = fp.nextSiblingElement(QStringLiteral("fingerprint"))) {
                    if (fp.namespaceURI() == DtlsNs) {
                        result.plain.append({ transport, fp });
                        dtls = true;
                    } else if (fp.namespaceURI() == VerificationNs) {
                        result.encrypted.append({ transport, fp });
                        dtls = true;
                    }
                }
                // A transport update without a fingerprint (candidates) changes nothing on the DTLS layer.
                Q_UNUSED(dtls)
            }
            return result;
        }

        Stanza::Error refusal(const QString &text)
        {
            return Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::NotAcceptable, text);
        }

        bool trusted(int level)
        {
            const auto value = EncryptionTrustLevel(level);
            return value == EncryptionTrustLevel::AutomaticallyTrusted || value == EncryptionTrustLevel::ManuallyTrusted
                || value == EncryptionTrustLevel::Authenticated;
        }

        QString key(const Jid &peer, const QString &sid) { return peer.bare() + QLatin1Char('\n') + sid; }

        // Runs @a step for 0..count-1 one after another; @a step calls its continuation once.
        void sequence(int count, const std::function<void(int, std::function<void()>)> &step,
                      const std::function<void()> &finished)
        {
            auto next = std::make_shared<std::function<void(int)>>();
            *next     = [count, step, finished, next](int index) {
                if (index >= count) {
                    finished();
                    *next = nullptr; // break the cycle
                    return;
                }
                step(index, [next, index]() {
                    if (*next)
                        (*next)(index + 1);
                });
            };
            (*next)(0);
        }
    }

    class OmemoDtlsVerification::Private {
    public:
        QPointer<Manager>                           manager;
        QPointer<OmemoEncryption>                   omemo;
        QHash<QString, quint32>                     devices;
        QHash<QString, Session::PeerAuthentication> pending; // for sessions not created yet
        QDomDocument                                document;
    };

    QString OmemoDtlsVerification::ns() { return VerificationNs; }

    OmemoDtlsVerification::OmemoDtlsVerification(Manager *manager, OmemoEncryption *omemo, QObject *parent) :
        QObject(parent), d(std::make_unique<Private>())
    {
        d->manager = manager;
        d->omemo   = omemo;
        manager->addSignalingFilter(this);
        connect(manager, &Manager::incomingSession, this, [this](Session *session) {
            const auto pending = d->pending.take(key(session->peer(), session->sid()));
            if (pending.isValid())
                session->setPeerAuthentication(pending);
        });
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

    OmemoDtlsVerification::~OmemoDtlsVerification()
    {
        if (d->manager)
            d->manager->removeSignalingFilter(this);
    }

    void OmemoDtlsVerification::setPeerDevice(const Jid &peer, const QString &sid, quint32 deviceId)
    {
        d->devices.insert(key(peer, sid), deviceId);
    }

    std::optional<quint32> OmemoDtlsVerification::peerDevice(const Jid &peer, const QString &sid) const
    {
        const auto it = d->devices.constFind(key(peer, sid));
        return it == d->devices.cend() ? std::nullopt : std::optional<quint32>(*it);
    }

    bool OmemoDtlsVerification::filterIncoming(const Jid &from, const QDomElement &jingle, const Done &done)
    {
        const auto sid      = jingle.attribute(QStringLiteral("sid"));
        const auto session  = d->manager ? d->manager->session(from, sid) : nullptr;
        const bool required = session && session->isEndToEndRequired();
        const auto found    = scan(jingle);
        if (found.encrypted.isEmpty()) {
            if (required && (found.unprotected || !found.plain.isEmpty())) {
                done({}, refusal(QStringLiteral("This session requires end-to-end encryption")));
                return true;
            }
            return false;
        }
        if (!d->omemo || (required && found.unprotected)) {
            done({}, refusal(QStringLiteral("The DTLS fingerprint cannot be verified")));
            return true;
        }

        // Decrypt each fingerprint into a copy of the element; all must come from one device.
        auto copy  = d->document.importNode(jingle, true).toElement();
        auto items = scan(copy).encrypted;
        struct State {
            Session::PeerAuthentication  authentication;
            std::optional<Stanza::Error> error;
        };
        auto                            state = std::make_shared<State>();
        QPointer<OmemoDtlsVerification> guard(this);
        sequence(
            int(items.size()),
            [this, guard, from, items, state](int index, std::function<void()> next) {
                const auto &item      = items.at(index);
                const auto  encrypted = item.element.firstChildElement(QStringLiteral("encrypted"));
                auto        job       = d->omemo->decryptElement(from, encrypted);
                auto        handle    = [guard, job, item, state, next]() {
                    job->deleteLater();
                    if (!guard)
                        return;
                    if (!state->error) {
                        auto       &auth   = state->authentication;
                        const auto &meta   = job->metadata();
                        const auto  plain  = job->stanza();
                        const auto  device = meta.senderDeviceId;
                        if (!job->success()) {
                            state->error = refusal(QStringLiteral("The DTLS fingerprint could not be decrypted: %1")
                                                                 .arg(job->errorString()));
                        } else if (auth.isValid() && auth.deviceId != device) {
                            state->error = refusal(QStringLiteral("DTLS fingerprints from different OMEMO devices"));
                        } else {
                            auth.method          = OmemoMethod;
                            auth.deviceId        = device;
                            auth.identityKey     = meta.senderKey;
                            auth.trustLevel      = meta.details.value(QStringLiteral("trustLevel")).toInt();
                            auto        document = item.transport.ownerDocument();
                            QDomElement fingerprint;
                            if (plain.namespaceURI() == DtlsNs && plain.localName() == QLatin1String("fingerprint")) {
                                // OMEMO 2: the authenticated element, attributes included.
                                fingerprint = document.importNode(plain, true).toElement();
                            } else {
                                fingerprint = document.createElementNS(DtlsNs, QStringLiteral("fingerprint"));
                                for (const auto &name : { QStringLiteral("hash"), QStringLiteral("setup") })
                                    if (item.element.hasAttribute(name))
                                        fingerprint.setAttribute(name, item.element.attribute(name));
                                fingerprint.appendChild(document.createTextNode(plain.text().trimmed()));
                            }
                            auto transport = item.transport;
                            transport.replaceChild(fingerprint, item.element);
                        }
                    }
                    next();
                };
                if (job->isFinished())
                    handle();
                else
                    connect(job, &EncryptionJob::finished, this, handle);
            },
            [this, guard, from, sid, copy, state, done]() {
                if (!guard)
                    return;
                if (state->error) {
                    done({}, state->error);
                    return;
                }
                const auto &auth    = state->authentication;
                auto        session = d->manager ? d->manager->session(from, sid) : nullptr;
                if (session && session->isEndToEndRequired() && !trusted(auth.trustLevel)) {
                    done({}, refusal(QStringLiteral("The peer's OMEMO device is not trusted")));
                    return;
                }
                // Our own fingerprints go to the device that verified its fingerprints.
                d->devices.insert(key(from, sid), auth.deviceId);
                if (session)
                    session->setPeerAuthentication(auth);
                else
                    d->pending.insert(key(from, sid), auth);
                done(copy, std::nullopt);
            });
        return true;
    }

    bool OmemoDtlsVerification::filterOutgoing(const Jid &to, const QDomElement &jingle, const Done &done)
    {
        const auto sid      = jingle.attribute(QStringLiteral("sid"));
        const auto session  = d->manager ? d->manager->session(to, sid) : nullptr;
        const bool required = session && session->isEndToEndRequired();
        const auto found    = scan(jingle);
        if (required && found.unprotected) {
            done({}, refusal(QStringLiteral("This session requires end-to-end encryption")));
            return true;
        }
        if (found.plain.isEmpty())
            return false;
        // A responder has received the initiator's fingerprints before answering: they must have been
        // verified by a trusted device.
        if (required && session->role() == Origin::Responder
            && (!session->peerAuthentication().isValid() || !trusted(session->peerAuthentication().trustLevel))) {
            done({}, refusal(QStringLiteral("The peer's DTLS fingerprint is not verified by a trusted OMEMO device")));
            return true;
        }
        const auto device = peerDevice(to, sid);
        if (!device || !d->omemo || !d->omemo->isReady()) {
            if (!required)
                return false; // as without verification
            done({}, refusal(QStringLiteral("No OMEMO device of the peer to verify the DTLS fingerprint with")));
            return true;
        }
        const auto protocol = d->omemo->preferredProtocolFor(to).value_or(OmemoProtocol::Legacy);

        auto                            copy    = d->document.importNode(jingle, true).toElement();
        auto                            items   = scan(copy).plain;
        auto                            failure = std::make_shared<std::optional<QString>>();
        QPointer<OmemoDtlsVerification> guard(this);
        sequence(
            int(items.size()),
            [this, guard, to, items, protocol, device, failure](int index, std::function<void()> next) {
                const auto &item = items.at(index);
                // An OMEMO 2 bundle can be missing although the device is there: a server that keeps one item
                // per PEP node (as old ejabberd does) holds only the bundle published last by the peer's
                // devices. Legacy OMEMO keeps a node per device, so the fingerprint goes that way then.
                auto encrypt = std::make_shared<std::function<void(OmemoProtocol)>>();
                *encrypt     = [this, guard, to, item, device, failure, next, encrypt](OmemoProtocol current) {
                    auto job    = d->omemo->encryptElement(to, current, item.element, { *device });
                    auto handle = [this, guard, job, item, failure, next, encrypt, current]() {
                        job->deleteLater();
                        if (!guard)
                            return;
                        if (!job->success() && current == OmemoProtocol::Omemo2
                            && d->omemo->supportedProtocols().testFlag(OmemoProtocol::Legacy)) {
                            qInfo("omemo-dtls: OMEMO 2 encryption failed (%s); trying legacy OMEMO",
                                      qUtf8Printable(job->errorString()));
                            (*encrypt)(OmemoProtocol::Legacy);
                            return;
                        }
                        *encrypt = nullptr; // break the cycle
                        if (!*failure) {
                            if (!job->success()) {
                                *failure = job->errorString();
                            } else {
                                auto document = item.transport.ownerDocument();
                                auto verified = document.createElementNS(VerificationNs, QStringLiteral("fingerprint"));
                                for (const auto &name : { QStringLiteral("hash"), QStringLiteral("setup") })
                                    if (item.element.hasAttribute(name))
                                        verified.setAttribute(name, item.element.attribute(name));
                                verified.appendChild(document.importNode(job->stanza(), true));
                                auto transport = item.transport;
                                transport.replaceChild(verified, item.element);
                            }
                        }
                        next();
                    };
                    if (job->isFinished())
                        handle();
                    else
                        connect(job, &EncryptionJob::finished, this, handle);
                };
                (*encrypt)(protocol);
            },
            [guard, copy, failure, required, done]() {
                if (!guard)
                    return;
                if (!*failure) {
                    done(copy, std::nullopt);
                    return;
                }
                if (required)
                    done({}, refusal(QStringLiteral("The DTLS fingerprint could not be encrypted: %1").arg(**failure)));
                else
                    done({}, std::nullopt); // send it unverified, as Conversations does without verification required
            });
        return true;
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
}}
