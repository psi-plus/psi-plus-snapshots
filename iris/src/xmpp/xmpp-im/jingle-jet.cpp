/*
 * jingle-jet.cpp - JET manager, per-session pad and per-content keys
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "jingle-jet.h"

#include "jingle-jet-connection.h"
#include "jingle-jet_p.h"
#include "xmpp_encryption.h"

#include <QPointer>
#include <utility>

namespace XMPP { namespace Jingle { namespace JET {
    namespace {
        const QString JetNs(QStringLiteral("urn:xmpp:jingle:jet:0"));

        bool validSecret(const ProtectedSecret &secret)
        {
            const auto size = keySize(secret.cipher);
            return size && secret.key.size() == *size && secret.iv.size() == 12;
        }

        bool trusted(int trustLevel)
        {
            const auto trust = EncryptionTrustLevel(trustLevel);
            return trust == EncryptionTrustLevel::AutomaticallyTrusted
                || trust == EncryptionTrustLevel::ManuallyTrusted || trust == EncryptionTrustLevel::Authenticated;
        }
    }

    class Manager::Private {
    public:
        QList<KeyTransport *> transports;
        QHash<Session *, QPointer<Pad>> pads;

        KeyTransport *forMethod(const QString &methodId) const
        {
            for (auto *transport : transports)
                if (transport->methodId() == methodId)
                    return transport;
            return nullptr;
        }

        KeyTransport *forProfile(const QString &wireProfile) const
        {
            for (auto *transport : transports)
                if (transport->accepts(wireProfile))
                    return transport;
            return nullptr;
        }
    };

    QString Manager::ns() { return JetNs; }

    Manager::Manager(XMPP::Jingle::Manager *jingle) : QObject(jingle), d(std::make_unique<Private>())
    {
        Q_ASSERT(jingle);
    }

    Manager::~Manager() = default;

    bool Manager::registerKeyTransport(KeyTransport *transport)
    {
        if (!transport || transport->methodId().isEmpty())
            return false;
        if (auto *existing = d->forMethod(transport->methodId()))
            return existing == transport;
        d->transports.append(transport);
        return true;
    }

    void Manager::unregisterKeyTransport(KeyTransport *transport)
    {
        if (!transport || !d->transports.removeAll(transport))
            return;
        emit keyTransportRemoved(transport->methodId());
    }

    QStringList Manager::discoFeatures() const
    {
        QStringList result;
        for (auto *transport : std::as_const(d->transports)) {
            const auto features = transport->discoFeatures();
            if (features.isEmpty())
                continue;
            if (result.isEmpty())
                result.append(JetNs);
            result.append(features);
        }
        result.removeDuplicates();
        return result;
    }

    Pad *Manager::pad(Session *session)
    {
        Q_ASSERT(session);
        Q_ASSERT(session->manager()->jet() == this);
        if (auto *existing = d->pads.value(session).data())
            return existing;
        auto *created = new Pad(this, session);
        d->pads.insert(session, created);
        connect(session, &QObject::destroyed, this, [this, session]() { d->pads.remove(session); });
        return created;
    }

    Pad::Pad(Manager *manager, Session *session) : QObject(session), manager_(manager), session_(session)
    {
        Q_ASSERT(manager_);
        Q_ASSERT(session_);
    }

    Pad::~Pad() = default;

    Protection *Pad::createProtection(QObject *owner, const QString &contentName)
    {
        Q_ASSERT(owner);
        Q_ASSERT(!contentName.isEmpty());
        return new Protection(this, owner, contentName);
    }

    void Pad::prepareOutgoing(const Done &done)
    {
        if (!manager_ || !session_) {
            done({}, QStringLiteral("JET session is no longer available"));
            return;
        }
        KeyTransport *transport = nullptr;
        if (!session_->encryptionMethodId().isEmpty())
            transport = manager_->d->forMethod(session_->encryptionMethodId());
        else if (manager_->d->transports.size() == 1)
            transport = manager_->d->transports.first();

        if (!transport || !session_->allowsEncryptionMethod(transport->methodId())
            || !transport->canEncrypt(session_->peer())) {
            done({}, QStringLiteral("No compatible JET key transport is available"));
            return;
        }

        const QString methodId = transport->methodId();
        QPointer<Pad> guard(this);
        transport->encryptSecret(session_->peer(), [guard, methodId, done](const std::optional<ProtectedSecret> &secret,
                                                                          const QString &error) {
            if (!guard || !guard->manager_)
                return;
            auto *current = guard->manager_->d->forMethod(methodId);
            if (!current || !secret || !validSecret(*secret) || secret->type.isEmpty() || secret->envelope.isNull()
                || !current->accepts(secret->type)) {
                done({}, error.isEmpty() ? QStringLiteral("Invalid outgoing JET key exchange") : error);
            } else {
                done(secret, {});
            }
        });
    }

    void Pad::prepareIncoming(const QDomElement &security, const QString &contentName, const Done &done)
    {
        if (!manager_ || !session_ || security.namespaceURI() != JetNs
            || security.attribute(QStringLiteral("name")) != contentName) {
            done({}, QStringLiteral("Invalid JET security envelope"));
            return;
        }
        const auto profile = security.attribute(QStringLiteral("type"));
        const auto length = keySize(security.attribute(QStringLiteral("cipher")));
        auto *transport = manager_->d->forProfile(profile);
        if (!transport || !session_->allowsEncryptionMethod(transport->methodId()) || !length) {
            done({}, QStringLiteral("Unsupported JET key transport or cipher"));
            return;
        }

        const auto methodId = transport->methodId();
        QPointer<Pad> guard(this);
        transport->decryptSecret(
            session_->peer(), security,
            [guard, methodId, profile, length, done](const std::optional<ProtectedSecret> &secret,
                                                     const QString &error) {
                if (!guard || !guard->manager_)
                    return;
                if (!guard->manager_->d->forMethod(methodId) || !secret || !validSecret(*secret)
                    || secret->key.size() != *length || !secret->authentication.isValid()
                    || secret->authentication.method != methodId || secret->type != profile) {
                    done({}, error.isEmpty() ? QStringLiteral("Invalid incoming JET key exchange") : error);
                } else {
                    done(secret, {});
                }
            });
    }

    Protection::Protection(Pad *pad, QObject *owner, const QString &contentName) :
        QObject(owner), pad_(pad), contentName_(contentName)
    {
        Q_ASSERT(pad_);
    }

    Protection::~Protection()
    {
        QObject::disconnect(removed_);
        QObject::disconnect(providerDestroyed_);
    }

    bool Protection::setIncomingContent(const QDomElement &content)
    {
        if (prepared_ || preparing_)
            return false;
        incomingSecurity_.clear();
        for (auto child = content.firstChildElement(QStringLiteral("security")); !child.isNull();
             child = child.nextSiblingElement(QStringLiteral("security"))) {
            if (child.namespaceURI() != Manager::ns())
                continue;
            if (!incomingSecurity_.isNull())
                return false;
            incomingSecurity_ = document_.importNode(child, true).toElement();
        }
        return incomingSecurity_.isNull()
            || incomingSecurity_.attribute(QStringLiteral("name")) == contentName_;
    }

    void Protection::prepare(bool outgoing, const Done &done)
    {
        if (preparing_)
            return;
        if (prepared_) {
            done({});
            return;
        }
        if (!pad_ || !pad_->session()) {
            done(QStringLiteral("JET session is unavailable"));
            return;
        }

        outgoing_ = outgoing;
        const auto *session = pad_->session();
        const bool protectedIncoming = !incomingSecurity_.isNull();
        const bool needCrypto = outgoing ? session->isEndToEndPreferred() : protectedIncoming;
        const bool mandatory = protectedIncoming || session->isEndToEndRequired();

        if (!needCrypto) {
            prepared_ = true;
            done(!outgoing && mandatory ? QStringLiteral("Incoming offer has no JET security") : QString());
            return;
        }

        auto *registry = pad_->manager();
        if (!registry) {
            done(mandatory ? QStringLiteral("JET key transport is unavailable") : QString());
            if (!mandatory)
                prepared_ = true;
            return;
        }

        preparing_ = true;
        done_ = done;
        QPointer<Protection> guard(this);
        removed_ = connect(registry, &Manager::keyTransportRemoved, this, [guard](const QString &) {
            if (guard && guard->preparing_)
                guard->complete({}, QStringLiteral("JET key transport was unloaded"));
        });
        providerDestroyed_ = connect(registry, &QObject::destroyed, this, [guard]() {
            if (guard && guard->preparing_)
                guard->complete({}, QStringLiteral("JET manager was destroyed"));
        });

        auto completed = [guard](const std::optional<ProtectedSecret> &secret, const QString &error) {
            if (guard && guard->preparing_)
                guard->complete(secret, error);
        };
        if (protectedIncoming)
            pad_->prepareIncoming(incomingSecurity_, contentName_, completed);
        else
            pad_->prepareOutgoing(completed);
    }

    void Protection::complete(const std::optional<ProtectedSecret> &secret, const QString &error)
    {
        if (!preparing_)
            return;
        preparing_ = false;
        QObject::disconnect(removed_);
        QObject::disconnect(providerDestroyed_);

        auto *session = pad_ ? pad_->session() : nullptr;
        const bool mandatory = !incomingSecurity_.isNull() || (session && session->isEndToEndRequired());
        QString failure = error;

        if (secret && !session)
            failure = QStringLiteral("JET session ended during key preparation");
        else if (secret && !incomingSecurity_.isNull()) {
            const auto previous = session->peerAuthentication();
            const auto &auth = secret->authentication;
            if (previous.isValid()
                && (previous.method != auth.method || previous.deviceId != auth.deviceId
                    || previous.identityKey != auth.identityKey)) {
                failure = QStringLiteral("JET peer identity changed");
            } else if (session->isEndToEndRequired() && !trusted(auth.trustLevel)) {
                failure = QStringLiteral("JET peer device is not trusted");
            }
        }

        if (failure.isEmpty() && secret) {
            secret_ = secret;
            if (!incomingSecurity_.isNull())
                session->setPeerAuthentication(secret->authentication);
        } else if (mandatory && failure.isEmpty()) {
            failure = QStringLiteral("JET key exchange failed");
        }

        if (!mandatory && !secret_)
            failure.clear(); // preferred encryption may fall back to plaintext
        prepared_ = failure.isEmpty();
        auto callback = std::move(done_);
        if (callback)
            callback(failure);
    }

    QDomElement Protection::outgoingSecurity(QDomDocument *document) const
    {
        if (!document || !outgoing_ || !secret_)
            return {};
        auto security = document->createElementNS(Manager::ns(), QStringLiteral("security"));
        security.setAttribute(QStringLiteral("name"), contentName_);
        security.setAttribute(QStringLiteral("cipher"), secret_->cipher);
        security.setAttribute(QStringLiteral("type"), secret_->type);
        security.appendChild(document->importNode(secret_->envelope, true));
        return security;
    }

    XMPP::Jingle::Connection::Ptr Protection::wrap(XMPP::Jingle::Connection::Ptr raw, bool sending, quint64 plainSize)
    {
        if (!prepared_ || !raw)
            return {};
        if (!secret_)
            return raw;
        if (connectionUsed_)
            return {}; // No reuse of an AES-GCM key/IV across transport replacements.
        connectionUsed_ = true;
        auto wrapped = XMPP::Jingle::Connection::Ptr(
            new JET::Connection(raw, secret_->key, secret_->iv, sending, plainSize));
        auto *jet = static_cast<JET::Connection *>(wrapped.data());
        if (!jet->isValid())
            return {};
        activeConnection_ = jet;
        connect(jet, &ByteStream::error, this, [this, jet](int) {
            const QString error = jet->errorText();
            emit failed(error);
        });
        return wrapped;
    }

    bool Protection::authenticated() const
    {
        if (!secret_)
            return true;
        auto *jet = qobject_cast<JET::Connection *>(activeConnection_.data());
        return jet && jet->authenticated();
    }
}}} // namespace XMPP::Jingle::JET
