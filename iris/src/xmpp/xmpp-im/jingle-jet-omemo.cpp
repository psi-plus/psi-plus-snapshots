/*
 * jingle-jet-omemo.cpp - Jingle Encrypted Transports with OMEMO (XEP-0391, XEP-0396)
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

#include "jingle-jet-omemo.h"

#include "jingle-jet_p.h"
#include "xmpp_caps.h"
#include "xmpp_client.h"
#include "xmpp_encryption.h"
#include "xmpp_omemo.h"

#include <QPointer>
#include <QtCrypto>

namespace XMPP { namespace Jingle {
    namespace {
        const QString JetOmemoNs(QStringLiteral("urn:xmpp:jingle:jet-omemo:0"));

        int keySizeFor(OmemoProtocol protocol) { return protocol == OmemoProtocol::Omemo2 ? 32 : 16; }

        QDomElement makeSecret(QDomDocument &document, const QByteArray &key, const QByteArray &iv)
        {
            auto secret = document.createElementNS(JetOmemoNs, QStringLiteral("secret"));
            secret.setAttribute(QStringLiteral("key"), QString::fromLatin1(key.toBase64()));
            secret.setAttribute(QStringLiteral("iv"), QString::fromLatin1(iv.toBase64()));
            document.appendChild(secret);
            return secret;
        }
    }

    class JetOmemo::Private {
    public:
        QPointer<Manager>         manager;
        QPointer<OmemoEncryption> omemo;
        QPointer<JET::Manager>    jet;
    };

    QString JetOmemo::ns() { return JetOmemoNs; }
    QString JetOmemo::jetNs() { return JET::Manager::ns(); }

    JetOmemo::JetOmemo(Manager *manager, OmemoEncryption *omemo, QObject *parent) :
        QObject(parent), d(std::make_unique<Private>())
    {
        d->manager = manager;
        d->omemo   = omemo;
        d->jet     = manager->jet();
        Q_ASSERT(d->jet);
        if (!d->jet->registerKeyTransport(this))
            qWarning("jingle-jet-omemo: duplicate key transport registration");
    }

    JetOmemo::~JetOmemo()
    {
        if (d->jet)
            d->jet->unregisterKeyTransport(this);
    }

    QString JetOmemo::methodId() const { return OmemoEncryption::methodId(); }

    QStringList JetOmemo::discoFeatures() const
    {
        if (!d->omemo)
            return {};
        const auto supported = d->omemo->supportedProtocols();
        if ((supported.testFlag(OmemoProtocol::Legacy) && JET::StreamCipher::isSupported(16))
            || (supported.testFlag(OmemoProtocol::Omemo2) && JET::StreamCipher::isSupported(32)))
            return { JetOmemoNs };
        return {};
    }

    bool JetOmemo::canEncrypt(const Jid &peer) const
    {
        if (!d->manager || !d->omemo || !d->omemo->isReady())
            return false;
        const auto protocol = d->omemo->preferredProtocolFor(peer);
        return protocol && JET::StreamCipher::isSupported(keySizeFor(*protocol))
            && d->manager->client()->capsManager()->features(peer).test(JetOmemoNs);
    }

    bool JetOmemo::accepts(const QString &wireProfile) const
    {
        return wireProfile == OmemoEncryption::namespaceUri() || wireProfile == OmemoEncryption::legacyNamespaceUri();
    }

    void JetOmemo::encryptSecret(const Jid &peer, const Done &done)
    {
        if (!canEncrypt(peer)) {
            done({}, QStringLiteral("The peer cannot receive JET-OMEMO key material"));
            return;
        }

        const auto   protocol = d->omemo->preferredProtocolFor(peer);
        QDomDocument document;
        QByteArray   key;
        QByteArray   iv;
        QDomElement  secret;
        if (*protocol == OmemoProtocol::Omemo2) {
            key    = QCA::Random::randomArray(32).toByteArray();
            iv     = QCA::Random::randomArray(12).toByteArray();
            secret = makeSecret(document, key, iv);
        }

        auto *job = d->omemo->encryptElement(peer, *protocol, secret);
        if (!job) {
            done({}, QStringLiteral("Cannot encrypt the JET transport secret"));
            return;
        }
        QPointer<JetOmemo> guard(this);
        const auto         finish = [guard, job, protocol = *protocol, key, iv, done]() {
            if (!guard) {
                job->deleteLater();
                return;
            }
            if (!job->success()) {
                const auto error = job->errorString();
                job->deleteLater();
                done({}, error);
                return;
            }
            JET::ProtectedSecret result;
            result.key      = protocol == OmemoProtocol::Omemo2
                             ? key
                             : job->metadata().details.value(QStringLiteral("transportKey")).toByteArray();
            result.iv       = protocol == OmemoProtocol::Omemo2
                              ? iv
                              : job->metadata().details.value(QStringLiteral("transportIv")).toByteArray();
            result.cipher   = JET::cipherFor(keySizeFor(protocol));
            result.type     = protocol == OmemoProtocol::Omemo2 ? OmemoEncryption::namespaceUri()
                                                                        : OmemoEncryption::legacyNamespaceUri();
            result.envelope = job->stanza();
            job->deleteLater();
            done(result, {});
        };
        if (job->isFinished())
            finish();
        else
            connect(job, &EncryptionJob::finished, this, finish);
    }

    void JetOmemo::decryptSecret(const Jid &peer, const QDomElement &security, const Done &done)
    {
        const auto wireProfile = security.attribute(QStringLiteral("type"));
        const bool omemo2      = wireProfile == OmemoEncryption::namespaceUri();
        const auto cipher      = security.attribute(QStringLiteral("cipher"));
        const auto keySize     = JET::keySize(cipher);
        if (!d->omemo || !accepts(wireProfile) || keySize != std::optional<int>(omemo2 ? 32 : 16)) {
            done({}, QStringLiteral("Unsupported JET-OMEMO profile or cipher"));
            return;
        }

        auto *job = d->omemo->decryptElement(peer, security.firstChildElement(QStringLiteral("encrypted")));
        if (!job) {
            done({}, QStringLiteral("Cannot decrypt the JET transport secret"));
            return;
        }
        QPointer<JetOmemo> guard(this);
        const auto         finish = [guard, job, wireProfile, cipher, omemo2, done]() {
            if (!guard) {
                job->deleteLater();
                return;
            }
            if (!job->success()) {
                const auto error = job->errorString();
                job->deleteLater();
                done({}, error);
                return;
            }

            const auto          &metadata = job->metadata();
            JET::ProtectedSecret result;
            result.key = metadata.details.value(QStringLiteral("transportKey")).toByteArray();
            result.iv  = metadata.details.value(QStringLiteral("transportIv")).toByteArray();
            if (omemo2) {
                const auto secret = job->stanza();
                const bool valid = secret.namespaceURI() == JetOmemoNs && secret.localName() == QLatin1String("secret");
                result.key
                    = valid ? QByteArray::fromBase64(secret.attribute(QStringLiteral("key")).toLatin1()) : QByteArray();
                result.iv
                    = valid ? QByteArray::fromBase64(secret.attribute(QStringLiteral("iv")).toLatin1()) : QByteArray();
            }
            result.cipher                     = cipher;
            result.type                       = wireProfile;
            result.authentication.method      = OmemoEncryption::methodId();
            result.authentication.deviceId    = metadata.senderDeviceId;
            result.authentication.identityKey = metadata.senderKey;
            result.authentication.trustLevel  = metadata.details.value(QStringLiteral("trustLevel")).toInt();
            job->deleteLater();
            done(result, {});
        };
        if (job->isFinished())
            finish();
        else
            connect(job, &EncryptionJob::finished, this, finish);
    }

}} // namespace XMPP::Jingle
