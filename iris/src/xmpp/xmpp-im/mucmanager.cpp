// SPDX-License-Identifier: LGPL-2.1-or-later
#include "mucmanager.h"

#include "xmpp_client.h"
#include "xmpp_tasks.h"

#include <utility>

namespace XMPP {
namespace {
constexpr int MaxEchoEntries = 64;

bool isEncryptionMarker(const QDomElement &element)
{
    return element.namespaceURI() == QLatin1String("urn:xmpp:eme:0")
        && element.localName() == QLatin1String("encryption");
}

bool isRoomStanzaId(const QDomElement &element, const Jid &room)
{
    return element.namespaceURI() == QLatin1String("urn:xmpp:sid:0")
        && element.localName() == QLatin1String("stanza-id")
        && Jid(element.attribute(QStringLiteral("by"))).bare() == room.bare();
}
} // namespace

MucManager::MucManager(Client *client) : QObject(client), client_(client) { }

MucManager::Room *MucManager::findRoom(const Jid &room)
{
    for (auto &item : rooms_) {
        if (item.jid.compare(room, false))
            return &item;
    }
    return nullptr;
}

const MucManager::Room *MucManager::findRoom(const Jid &room) const
{
    for (const auto &item : rooms_) {
        if (item.jid.compare(room, false))
            return &item;
    }
    return nullptr;
}

QString MucManager::password(const QString &host, const QString &room) const
{
    const auto *item = findRoom(Jid(room + QLatin1Char('@') + host));
    return item ? item->password : QString();
}

QString MucManager::nick(const QString &host, const QString &room) const
{
    const auto *item = findRoom(Jid(room + QLatin1Char('@') + host));
    return item ? item->jid.resource() : QString();
}

void MucManager::sendPresence(const Jid &jid, const Status &status)
{
    auto *task = new JT_Presence(client_->rootTask());
    task->pres(jid, status);
    task->go(true);
}

bool MucManager::join(const QString &host, const QString &room, const QString &nick, const QString &password,
                      int maxchars, int maxstanzas, int seconds, const QDateTime &since, const Status &status)
{
    const Jid occupant(room + QLatin1Char('@') + host + QLatin1Char('/') + nick);
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        if (it->jid.compare(occupant, false)) {
            if (it->state != State::Closing)
                return false;
            clearRoomEchoes(it->jid);
            it = rooms_.erase(it);
        } else {
            ++it;
        }
    }

    Room item;
    item.jid = occupant;
    item.state = State::Connecting;
    item.password = password;
    rooms_.append(item);

    Status presenceStatus = status;
    presenceStatus.setMUC();
    presenceStatus.setMUCHistory(maxchars, maxstanzas, seconds, since);
    if (!password.isEmpty())
        presenceStatus.setMUCPassword(password);
    sendPresence(occupant, presenceStatus);
    return true;
}

void MucManager::changeNick(const QString &host, const QString &room, const QString &nick, const Status &status)
{
    const Jid newOccupant(room + QLatin1Char('@') + host + QLatin1Char('/') + nick);
    auto *item = findRoom(newOccupant);
    if (!item)
        return;
    if (item->jid.resource() != newOccupant.resource()) {
        item->realJids.clear();
        clearRoomEchoes(item->jid);
    }
    item->jid = newOccupant;

    Status newStatus = status;
    newStatus.setIsAvailable(true);
    sendPresence(newOccupant, newStatus);
}

void MucManager::setStatus(const QString &host, const QString &room, const Status &status)
{
    const auto *item = findRoom(Jid(room + QLatin1Char('@') + host));
    if (!item)
        return;
    Status newStatus = status;
    newStatus.setIsAvailable(true);
    sendPresence(item->jid, newStatus);
}

void MucManager::clearRoomEchoes(const Jid &room)
{
    const QString prefix = room.bare() + QLatin1Char('\n');
    for (auto it = pendingEchoes_.begin(); it != pendingEchoes_.end();) {
        if (it.key().startsWith(prefix)) {
            pendingOrder_.removeAll(it.key());
            it = pendingEchoes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = completedEchoes_.begin(); it != completedEchoes_.end();) {
        if ((*it).startsWith(prefix)) {
            completedOrder_.removeAll(*it);
            it = completedEchoes_.erase(it);
        } else {
            ++it;
        }
    }
}

void MucManager::leave(const QString &host, const QString &room, const QString &reason)
{
    auto *item = findRoom(Jid(room + QLatin1Char('@') + host));
    if (!item)
        return;
    item->state = State::Closing;
    item->realJids.clear();
    clearRoomEchoes(item->jid);

    Status status;
    status.setIsAvailable(false);
    status.setStatus(reason);
    sendPresence(item->jid, status);
}

void MucManager::leaveAll(const QString &reason)
{
    pendingEchoes_.clear();
    pendingOrder_.clear();
    completedEchoes_.clear();
    completedOrder_.clear();
    if (!client_->hasStream() || !client_->isActive())
        return;
    for (auto &item : rooms_) {
        item.state = State::Closing;
        item.realJids.clear();

        Status status;
        status.setIsAvailable(false);
        status.setStatus(reason);
        sendPresence(item.jid, status);
    }
}

bool MucManager::handlePresence(const Jid &occupant, const Status &status)
{
    for (auto it = rooms_.begin(); it != rooms_.end(); ++it) {
        auto &room = *it;
        if (!room.jid.compare(occupant, false))
            continue;

        // An occupant nickname may belong to a different account on every
        // join. Its cryptographic identity exists only while presence says so.
        if (!occupant.resource().isEmpty()) {
            const Jid real = status.hasMUCItem() ? status.mucItem().jid() : Jid();
            if (room.state != State::Closing && status.isAvailable() && !status.hasError()
                && real.isValid() && !real.node().isEmpty())
                room.realJids.insert(occupant.resource(), Jid(real.bare()));
            else
                room.realJids.remove(occupant.resource());
        }

        const bool self = room.jid.resource() == occupant.resource() || occupant.resource().isEmpty();
        switch (room.state) {
        case State::Connecting:
            if (self && status.hasError()) {
                const Jid jid = room.jid;
                clearRoomEchoes(jid);
                rooms_.erase(it);
                emit error(jid, status.errorCode(), status.errorString());
            } else {
                // Preserve existing Iris semantics for joining MUC presence.
                if (!status.hasError()) {
                    room.state = State::Connected;
                    const Jid jid = room.jid;
                    emit joined(jid);
                }
                emit presence(occupant, status);
            }
            break;
        case State::Connected:
            emit presence(occupant, status);
            break;
        case State::Closing:
            if (self && !status.isAvailable()) {
                const Jid jid = room.jid;
                clearRoomEchoes(jid);
                rooms_.erase(it);
                emit left(jid);
            }
            break;
        }
        return true;
    }
    return false;
}

bool MucManager::acceptsMessage(const Jid &occupant) const
{
    const auto *room = findRoom(occupant);
    return room && room->state == State::Connected;
}

Jid MucManager::realJid(const Jid &occupant) const
{
    const auto *room = findRoom(occupant);
    if (!room || room->state != State::Connected || occupant.resource().isEmpty())
        return {};
    return room->realJids.value(occupant.resource());
}

QString MucManager::echoKey(const Jid &room, const QString &id)
{
    return room.bare() + QLatin1Char('\n') + id;
}

void MucManager::rememberEncryptedEcho(const QDomElement &original, const QString &methodId)
{
    if (methodId != QLatin1String("omemo")
        || original.attribute(QStringLiteral("type")) != QLatin1String("groupchat"))
        return;
    const Jid room(original.attribute(QStringLiteral("to")));
    const QString id = original.attribute(QStringLiteral("id"));
    const auto *item = findRoom(room);
    if (!item || item->state != State::Connected || !room.isValid() || room.node().isEmpty()
        || !room.resource().isEmpty() || id.isEmpty())
        return;

    QDomDocument plaintext;
    plaintext.appendChild(plaintext.importNode(original, true));
    const QString key = echoKey(room, id);
    pendingEchoes_.insert(key, plaintext);
    pendingOrder_.removeAll(key);
    pendingOrder_.append(key);
    completedEchoes_.remove(key);
    completedOrder_.removeAll(key);
    while (pendingOrder_.size() > MaxEchoEntries)
        pendingEchoes_.remove(pendingOrder_.takeFirst());
}

MucManager::EchoResult MucManager::resolveEncryptedEcho(const QDomElement &stanza, const QString &methodId)
{
    EchoResult result;
    if (methodId != QLatin1String("omemo")
        || stanza.attribute(QStringLiteral("type")) != QLatin1String("groupchat"))
        return result;

    const Jid occupant(stanza.attribute(QStringLiteral("from")));
    const auto *room = findRoom(occupant);
    if (!room || room->state != State::Connected || occupant.resource().isEmpty()
        || room->jid.resource() != occupant.resource())
        return result;
    // XEP-0045 reflects our outgoing message using the current occupant JID
    // and client-generated message ID.

    const QString id = stanza.attribute(QStringLiteral("id"));
    if (id.isEmpty())
        return result;
    const QString key = echoKey(occupant, id);
    auto pending = pendingEchoes_.find(key);
    if (completedEchoes_.contains(key)) {
        result.disposition = EchoDisposition::Duplicate;
        return result;
    }
    if (pending == pendingEchoes_.end())
        return result;

    auto root = result.plaintext.importNode(pending->documentElement(), true).toElement();
    result.plaintext.appendChild(root);
    root.setAttribute(QStringLiteral("from"), occupant.full());
    if (stanza.hasAttribute(QStringLiteral("to")))
        root.setAttribute(QStringLiteral("to"), stanza.attribute(QStringLiteral("to")));
    else
        root.removeAttribute(QStringLiteral("to"));

    // Only server-generated metadata is taken from the echo; the plaintext
    // content is solely from our own successfully encrypted outgoing stanza.
    for (auto child = stanza.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (isRoomStanzaId(child, occupant) || isEncryptionMarker(child))
            root.appendChild(result.plaintext.importNode(child, true));
    }
    result.metadata.methodId = methodId;
    result.metadata.sender = client_->jid().bare();
    result.metadata.details.insert(QStringLiteral("localMucEcho"), true);
    result.disposition = EchoDisposition::Delivered;

    pendingOrder_.removeAll(key);
    pendingEchoes_.erase(pending);
    completedEchoes_.insert(key);
    completedOrder_.removeAll(key);
    completedOrder_.append(key);
    while (completedOrder_.size() > MaxEchoEntries)
        completedEchoes_.remove(completedOrder_.takeFirst());
    return result;
}

void MucManager::clear()
{
    rooms_.clear();
    pendingEchoes_.clear();
    pendingOrder_.clear();
    completedEchoes_.clear();
    completedOrder_.clear();
}

} // namespace XMPP
