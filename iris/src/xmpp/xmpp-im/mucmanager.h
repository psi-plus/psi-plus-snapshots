// SPDX-License-Identifier: LGPL-2.1-or-later
// Internal lifecycle and stanza reconciliation for XEP-0045 rooms.
// This is not part of the public Iris API; Client remains its facade.
#pragma once

#include "xmpp_encryption.h"
#include "xmpp_jid.h"
#include "xmpp_status.h"

#include <QDateTime>
#include <QDomDocument>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QList>

namespace XMPP {

class Client;

class MucManager final : public QObject {
    Q_OBJECT
public:
    explicit MucManager(Client *client);

    QString password(const QString &host, const QString &room) const;
    QString nick(const QString &host, const QString &room) const;
    bool join(const QString &host, const QString &room, const QString &nick, const QString &password,
              int maxchars, int maxstanzas, int seconds, const QDateTime &since, const Status &status);
    void changeNick(const QString &host, const QString &room, const QString &nick, const Status &status);
    void setStatus(const QString &host, const QString &room, const Status &status);
    void leave(const QString &host, const QString &room, const QString &reason);
    void leaveAll(const QString &reason);

    // Return true for presence addressed to a currently tracked room.
    bool handlePresence(const Jid &occupant, const Status &status);
    bool acceptsMessage(const Jid &occupant) const;
    // Live mapping only; never use occupant nicks as OMEMO identities.
    Jid realJid(const Jid &occupant) const;

    enum class EchoDisposition { NotEcho, Delivered, Duplicate, Rejected };
    struct EchoResult {
        EchoDisposition   disposition = EchoDisposition::NotEcho;
        QDomDocument      plaintext;
        EncryptionMetadata metadata;
        QString           error;
    };
    // Called only after OMEMO encryption succeeds, before writing the stanza.
    void rememberEncryptedEcho(const QDomElement &original, const QDomElement &encrypted,
                               const QString &methodId);
    EchoResult resolveEncryptedEcho(const QDomElement &stanza, const QString &methodId);
    void clear();

signals:
    void joined(const Jid &room);
    void left(const Jid &room);
    void presence(const Jid &occupant, const Status &status);
    void error(const Jid &room, int code, const QString &message);

private:
    enum class State { Connecting, Connected, Closing };
    struct Room {
        Jid jid;
        State state = State::Connecting;
        QString password;
        // Presence-scoped; a nick is never a durable identity.
        QHash<QString, Jid> realJids;
    };
    struct PendingEcho {
        QDomDocument plaintext;
        QByteArray ciphertextDigest;
        QString methodId;
    };
    struct CompletedEcho {
        QByteArray ciphertextDigest;
    };

    Room *findRoom(const Jid &room);
    const Room *findRoom(const Jid &room) const;
    static QString echoKey(const Jid &room, const QString &id);
    static QByteArray ciphertextDigest(const QDomElement &stanza);
    void clearRoomEchoes(const Jid &room);
    void sendPresence(const Jid &jid, const Status &status);

    Client *client_ = nullptr;
    QList<Room> rooms_;
    QHash<QString, PendingEcho> pendingEchoes_;
    QStringList pendingOrder_;
    QHash<QString, CompletedEcho> completedEchoes_;
    QStringList completedOrder_;
};

} // namespace XMPP
