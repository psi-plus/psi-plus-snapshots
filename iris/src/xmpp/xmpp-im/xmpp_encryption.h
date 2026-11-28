/*
 * Copyright (C) 2021-2026  Sergey Ilinykh
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
 */

#ifndef XMPP_ENCRYPTION_H
#define XMPP_ENCRYPTION_H

#include <iris/iris_export.h>

#include <iris/jid/jid.h>
#include <iris/xmpp-im/xmpp_features.h>

#include <QByteArray>
#include <QDomDocument>
#include <QFlags>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <map>
#include <memory>
#include <optional>

namespace XMPP {

/** Common trust state for identity keys used by encryption methods. */
enum class EncryptionTrustLevel : quint8 {
    Undecided            = 0x01,
    AutomaticallyTrusted = 0x02,
    ManuallyTrusted      = 0x04,
    Authenticated        = 0x08,
    Distrusted           = 0x10
};
Q_DECLARE_FLAGS(EncryptionTrustLevels, EncryptionTrustLevel)
Q_DECLARE_OPERATORS_FOR_FLAGS(EncryptionTrustLevels)

/**
 * Persistence interface for encryption identity trust.
 *
 * Trust deliberately lives outside protocol implementations: a cryptographic
 * engine may validate that an identity key is stable while the application
 * decides whether that identity is acceptable for a particular operation.
 */
class EncryptionTrustStorage {
public:
    virtual ~EncryptionTrustStorage() = default;

    virtual EncryptionTrustLevel trustLevel(const QString &methodId, const Jid &owner, const QByteArray &keyId) const
        = 0;
    virtual bool setTrustLevel(const QString &methodId, const Jid &owner, const QByteArray &keyId,
                               EncryptionTrustLevel level)
        = 0;
    virtual bool removeTrust(const QString &methodId, const Jid &owner, const QByteArray &keyId) = 0;
};

/** Volatile trust store useful for tests and non-persistent clients. */
class MemoryEncryptionTrustStorage final : public EncryptionTrustStorage {
public:
    EncryptionTrustLevel trustLevel(const QString &methodId, const Jid &owner, const QByteArray &keyId) const override;
    bool                 setTrustLevel(const QString &methodId, const Jid &owner, const QByteArray &keyId,
                                       EncryptionTrustLevel level) override;
    bool                 removeTrust(const QString &methodId, const Jid &owner, const QByteArray &keyId) override;

private:
    QHash<QString, EncryptionTrustLevel> levels_;
};

/**
 * Metadata attached to an encryption/decryption result.
 *
 * The common fields deliberately describe the cryptographic peer rather than
 * a concrete encryption protocol. Protocol-specific state that has to
 * survive deferred replies or a failed operation can be placed in details.
 */
class EncryptionMetadata {
public:
    QString     methodId;
    Jid         sender;
    quint32     senderDeviceId = 0;
    QByteArray  senderKey;
    bool        protocolOnly = false;
    QVariantMap details;
};

/**
 * Context bound to an encryption session.
 *
 * recipients contains logical XMPP recipients.  replyTo is used when a stanza
 * is a deferred response to an encrypted stanza and lets a method preserve the
 * original cryptographic peer/session when that is required by the protocol.
 */
class EncryptionContext {
public:
    QList<Jid>                        recipients;
    std::optional<EncryptionMetadata> replyTo;
    QVariantMap                       options;
    // Historical replay may update cryptographic receive state, but must not
    // acknowledge old key exchanges or send ratchet heartbeat messages.
    bool isArchiveReplay() const { return options.value(QStringLiteral("archiveReplay")).toBool(); }
    void setArchiveReplay(bool enabled = true) { options.insert(QStringLiteral("archiveReplay"), enabled); }
};

/** Asynchronous result of a single encryption/decryption operation. */
class IRIS_EXPORT EncryptionJob : public QObject {
    Q_OBJECT
public:
    enum class Error {
        None,
        Unsupported,
        InvalidInput,
        NoRecipients,
        NoSession,
        UntrustedIdentity,
        CryptoError,
        AuthenticationFailed,
        ProtocolError,
        StorageError,
        NetworkError,
        Cancelled
    };
    Q_ENUM(Error)

    explicit EncryptionJob(QObject *parent = nullptr);
    ~EncryptionJob() override;

    bool                      isFinished() const;
    bool                      success() const;
    Error                     error() const;
    QString                   errorString() const;
    QDomElement               stanza() const;
    QByteArray                data() const;
    const EncryptionMetadata &metadata() const;

    // EncryptionMethod implementations complete jobs through these helpers.
    void complete(const QDomElement &stanza, const EncryptionMetadata &metadata = {});
    void complete(const QByteArray &data, const EncryptionMetadata &metadata = {});
    void fail(Error error, const QString &message = {}, const EncryptionMetadata &metadata = {});

signals:
    void finished();

private:
    class Private;
    std::unique_ptr<Private> d;
};

/**
 * A protocol session.  A session can support XMPP stanzas, all-at-once byte
 * messages and/or incremental streams.  Operations are asynchronous because a
 * method such as OMEMO may need to obtain remote key material before the first
 * encryption can finish.
 */
class EncryptedSession : public QObject {
    Q_OBJECT
public:
    explicit EncryptedSession(const QString &methodId, const EncryptionContext &context = {},
                              QObject *parent = nullptr);
    ~EncryptedSession() override;

    QString                  methodId() const;
    const EncryptionContext &context() const;
    bool                     isClosing() const;
    bool                     hasActiveOperations() const;
    void                     close();

    // Validate session state, track jobs, and dispatch to protocol implementations.
    // Per-operation context is merged with the long-lived peer context.
    // Concurrent jobs never modify the session's base context.
    EncryptionJob *encrypt(const QDomElement &xml, const EncryptionContext &operation = {});
    EncryptionJob *decrypt(const QDomElement &xml, const EncryptionContext &operation = {});
    EncryptionJob *encrypt(const QByteArray &data);
    EncryptionJob *decrypt(const QByteArray &data);

    /**
     * Incremental encryption/decryption (DataStream capability). A session owns exactly one
     * stream: submit the next chunk only after the preceding job succeeds. The final chunk
     * must be submitted with final=true, even for an empty stream. A failed or finalized
     * stream cannot be reused; start a new session instead.
     *
     * Unlike DataMessage, a successful non-final decryption does not authenticate the
     * whole stream. Consumers MUST hold decrypted bytes as untrusted until the final
     * job succeeds, and must discard them on failure (for example, an invalid GCM tag).
     * Implementations can emit the authentication tag as part of the final encrypted
     * result, or validate it in the final decrypted result.
     */
    EncryptionJob *encryptChunk(const QByteArray &chunk, bool final = false);
    EncryptionJob *decryptChunk(const QByteArray &chunk, bool final = false);

protected:
    virtual EncryptionJob *doEncrypt(const QDomElement &xml, const EncryptionContext &context);
    virtual EncryptionJob *doDecrypt(const QDomElement &xml, const EncryptionContext &context);
    virtual EncryptionJob *doEncrypt(const QByteArray &data);
    virtual EncryptionJob *doDecrypt(const QByteArray &data);
    virtual EncryptionJob *doEncryptChunk(const QByteArray &chunk, bool final);
    virtual EncryptionJob *doDecryptChunk(const QByteArray &chunk, bool final);

private:
    EncryptionJob *runStream(const QByteArray &chunk, bool encrypting, bool final);
    EncryptionJob *track(EncryptionJob *job);
    void operationFinished(EncryptionJob *job);

    class Private;
    std::unique_ptr<Private> d;

    friend class EncryptionManager;
};

class EncryptionMethod : public QObject {
    Q_OBJECT
public:
    enum class Availability { Unknown, Available, Unavailable };
    enum Capability {
        XmppStanza  = 0x1, /**< XML stanza/SCE style encryption. */
        DataMessage = 0x2, /**< An independent byte message, processed at once. */
        DataStream  = 0x4, /**< Incremental stream/keying adapter. */
        Groupchat   = 0x8  /**< Groupchat stanza encryption; combine with XmppStanza. */
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)

    enum GroupchatRequirement {
        MembersOnly  = 0x1, /**< Require a members-only room. */
        NonAnonymous = 0x2, /**< Require real occupant identities. */
        MemberJids   = 0x4  /**< Require the complete member/admin/owner bare JID list, including offline members. */
    };
    Q_DECLARE_FLAGS(GroupchatRequirements, GroupchatRequirement)

    explicit EncryptionMethod(QObject *parent = nullptr);
    ~EncryptionMethod() override;

    virtual QString           id() const                                                        = 0;
    virtual QString           name() const                                                      = 0;
    virtual Capabilities      capabilities() const                                              = 0;
    virtual EncryptedSession *startSession(Capabilities caps, const EncryptionContext &context) = 0;
    virtual Features          features() const                                                  = 0;
    virtual bool              canDecrypt(const QDomElement &stanza) const                       = 0;
    // Concrete methods decide peer support; absent discovery must not be mistaken for rejection.
    virtual Availability availability(const Jid &peer) const;

    // Only meaningful for methods advertising Groupchat. The host supplies the
    // required room information and must not send if any requirement is unmet.
    virtual GroupchatRequirements groupchatRequirements() const { return {}; }

    /**
     * Resolve and validate the peer data needed to recover a failed incoming
     * decryption. Implementations must not create a new cryptographic session
     * or send a recovery/key-exchange message during this phase. Read-only
     * discovery requests needed to present the decision are allowed.
     */
    virtual EncryptionJob *prepareDecryptionRecovery(const EncryptionMetadata &metadata);

    /**
     * Restore protocol state after the application has explicitly authorized
     * recovery. The caller is responsible for obtaining any user interaction
     * required by the encryption protocol and local trust policy.
     */
    virtual EncryptionJob *recoverDecryption(const EncryptionMetadata &metadata);
};
Q_DECLARE_OPERATORS_FOR_FLAGS(EncryptionMethod::Capabilities)
Q_DECLARE_OPERATORS_FOR_FLAGS(EncryptionMethod::GroupchatRequirements)

/** Registry and session factory for dynamically installable end-to-end encryption methods. */
class EncryptionManager : public QObject {
    Q_OBJECT
public:
    using MethodId   = QString;
    using MethodName = QString;
    using MethodsMap = std::map<MethodId, MethodName>;

    explicit EncryptionManager(QObject *parent = nullptr);
    ~EncryptionManager() override;

    bool registerMethod(EncryptionMethod *method);
    void unregisterMethod(EncryptionMethod *method);

    EncryptionMethod *method(const QString &id) const;
    EncryptionMethod *methodForStanza(const QDomElement &stanza) const;
    EncryptionMethod::Availability availability(const QString &methodId, const Jid &peer) const;
    MethodsMap                     methods(EncryptionMethod::Capabilities caps = {}) const;
    Features          features() const;

    /** A session may combine stanza signaling and incremental data, e.g. for Jingle. */
    EncryptedSession *startSession(const QString &methodId, EncryptionMethod::Capabilities capabilities,
                                   const EncryptionContext &context = {});

signals:
    void methodRegistered(const QString &id);
    void methodUnregistered(const QString &id);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

Q_DECLARE_METATYPE(XMPP::EncryptionMetadata)

#endif // XMPP_ENCRYPTION_H
