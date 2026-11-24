/*
 * jingle-jet.h - protocol-independent Jingle Encrypted Transports (XEP-0391)
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef JINGLE_JET_H
#define JINGLE_JET_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-session.h>
#include <iris/xmpp-im/jingle-connection.h>
#include <iris/xmpp-im/jingle.h>

#include <QByteArray>
#include <QDomDocument>
#include <QHash>
#include <QPointer>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace XMPP { namespace Jingle { namespace JET {

    /**
     * Key material and its protected Jingle envelope. Encryption methods own the
     * envelope format; JET validates cipher/key sizes and installs the secret on
     * the content's transport.
     */
    struct IRIS_EXPORT ProtectedSecret {
        QByteArray                  key;
        QByteArray                  iv;
        QString                     cipher; // JET cipher URI
        QString                     type;   // encryption method's wire-profile URI
        QDomElement                 envelope;
        Session::PeerAuthentication authentication;
    };

    /**
     * Protocol-specific key exchange for XEP-0391. XEP-0396 (OMEMO) is one
     * implementation; another encryption method only needs another adapter.
     *
     * Non-owning registration: implementations must unregister before destruction.
     * A callback must be completed once even on cryptographic failure.
     */
    class IRIS_EXPORT KeyTransport {
    public:
        using Done = std::function<void(const std::optional<ProtectedSecret> &secret, const QString &error)>;

        virtual ~KeyTransport()                                                                           = default;
        virtual QString     methodId() const                                                              = 0;
        virtual QStringList discoFeatures() const                                                         = 0;
        virtual bool        canEncrypt(const Jid &peer) const                                             = 0;
        virtual bool        accepts(const QString &wireProfile) const                                     = 0;
        virtual void        encryptSecret(const Jid &peer, const Done &done)                              = 0;
        virtual void        decryptSecret(const Jid &peer, const QDomElement &security, const Done &done) = 0;
    };

    class Pad;
    class Protection;

    /**
     * One provider registry per Jingle account. This object owns neither a
     * Jingle session nor the encryption keys of any content.
     */
    class IRIS_EXPORT Manager : public QObject {
        Q_OBJECT
    public:
        static QString ns(); // urn:xmpp:jingle:jet:0

        explicit Manager(XMPP::Jingle::Manager *jingle);
        ~Manager() override;

        bool registerKeyTransport(KeyTransport *transport);
        void unregisterKeyTransport(KeyTransport *transport);
        QStringList discoFeatures() const;
        Pad *pad(Session *session);

    signals:
        void keyTransportRemoved(const QString &methodId);

    private:
        friend class Pad;
        class Private;
        std::unique_ptr<Private> d;
    };

    /**
     * Session-owned JET context. Chooses a registered key transport using the
     * Jingle session's method, without duplicating the encryption registry.
     */
    class IRIS_EXPORT Pad : public QObject {
        Q_OBJECT
    public:
        Pad(Manager *manager, Session *session);
        ~Pad() override;

        Manager *manager() const { return manager_; }
        Session *session() const { return session_; }
        Protection *createProtection(QObject *owner, const QString &contentName);

        using Done = KeyTransport::Done;
        void prepareOutgoing(const Done &done);
        void prepareIncoming(const QDomElement &security, const QString &contentName, const Done &done);

    private:
        QPointer<Manager> manager_;
        QPointer<Session> session_;
    };

    /**
     * Per-content encryption state. Wraps transport connections, not physical
     * ICE/S5B associations, so BUNDLE members keep independent cipher contexts.
     */
    class IRIS_EXPORT Protection : public QObject {
        Q_OBJECT
    public:
        using Done = std::function<void(const QString &error)>;

        Protection(Pad *pad, QObject *owner, const QString &contentName);
        ~Protection() override;

        bool setIncomingContent(const QDomElement &content);
        void prepare(bool outgoing, const Done &done);
        bool prepared() const { return prepared_; }
        bool encrypted() const { return secret_.has_value(); }
        bool authenticated() const;

        QDomElement outgoingSecurity(QDomDocument *document) const;
        XMPP::Jingle::Connection::Ptr wrap(XMPP::Jingle::Connection::Ptr raw, bool sending, quint64 plainSize);

    signals:
        void failed(const QString &error);

    private:
        void complete(const std::optional<ProtectedSecret> &secret, const QString &error);

        QPointer<Pad> pad_;
        QString contentName_;
        QDomDocument document_;
        QDomElement incomingSecurity_;
        std::optional<ProtectedSecret> secret_;
        QPointer<QObject> activeConnection_;
        Done done_;
        bool preparing_ = false;
        bool prepared_ = false;
        bool outgoing_ = false;
        bool connectionUsed_ = false; // GCM key/IV must never encrypt a second stream
        QMetaObject::Connection removed_;
        QMetaObject::Connection providerDestroyed_;
    };

}}} // namespace XMPP::Jingle::JET
#endif // JINGLE_JET_H
