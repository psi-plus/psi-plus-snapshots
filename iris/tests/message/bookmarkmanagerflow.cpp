// SPDX-License-Identifier: LGPL-2.1-or-later
// BookmarksManager against a scripted server: disco, XEP-0048 Private XML and
// XEP-0402 PEP items, publish and retract. Covers lost-update and activation
// races that the PubSubReplica unit test cannot see.
#include <iris/xmpp.h>
#include <iris/xmpp_bookmarkmanager.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_serverinfomanager.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

#include <functional>
#include <memory>

using namespace XMPP;

namespace {
void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class OfflineConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class Recorder final : public ClientStream {
public:
    explicit Recorder(Connector *connector) : ClientStream(connector) { }
    void               write(const Stanza &s) override { sent.append(s.element().cloneNode(true).toElement()); }
    QList<QDomElement> sent;
};

const QString pubsubNs = QStringLiteral("http://jabber.org/protocol/pubsub");
const QString room1    = QStringLiteral("one@conference.example.test");
const QString room2    = QStringLiteral("two@conference.example.test");
const QString legacy   = QStringLiteral("legacy@conference.example.test");

struct Server {
    OfflineConnector connector;
    Recorder         stream { &connector };
    Client           client;
    int              cursor = 0;

    Server()
    {
        client.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(), QStringLiteral("desktop"));
        client.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));
    }

    static void settle()
    {
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
    }

    // The next unanswered request matching the predicate (in send order).
    QDomElement take(const std::function<bool(const QDomElement &)> &match)
    {
        settle();
        for (int i = 0; i < stream.sent.size(); ++i) {
            const auto iq = stream.sent.at(i);
            if (!iq.hasAttribute(QStringLiteral("answered")) && match(iq)) {
                stream.sent[i].setAttribute(QStringLiteral("answered"), QStringLiteral("1"));
                return iq;
            }
        }
        return {};
    }

    void reply(const QDomElement &request, const QString &payload = {}, const QString &type = QStringLiteral("result"))
    {
        check(!request.isNull(), "Expected request was never sent");
        QDomDocument doc;
        check(doc.setContent(QStringLiteral("<iq xmlns='jabber:client' type='%1' id='%2'>%3</iq>")
                                 .arg(type, request.attribute(QStringLiteral("id")), payload),
                             true),
              "Invalid reply fixture");
        check(client.rootTask()->take(doc.documentElement()), "Reply was not consumed by any task");
        settle();
    }

    static bool hasChild(const QDomElement &iq, const QString &ns, const QString &name)
    {
        for (auto e = iq.firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
            if (e.namespaceURI() == ns && e.localName() == name)
                return true;
        return false;
    }
    static QDomElement pubsubChild(const QDomElement &iq, const QString &name)
    {
        const auto pubsub = iq.firstChildElement(QStringLiteral("pubsub"));
        return pubsub.namespaceURI() == pubsubNs ? pubsub.firstChildElement(name) : QDomElement();
    }
    QDomElement discoTo(const QString &to)
    {
        return take([to](const QDomElement &iq) {
            return iq.attribute(QStringLiteral("to")) == to
                && hasChild(iq, QStringLiteral("http://jabber.org/protocol/disco#info"), QStringLiteral("query"));
        });
    }
    QDomElement privateXml(const QString &type)
    {
        return take([type](const QDomElement &iq) {
            return iq.attribute(QStringLiteral("type")) == type
                && hasChild(iq, QStringLiteral("jabber:iq:private"), QStringLiteral("query"));
        });
    }
    QDomElement pubsub(const QString &operation)
    {
        return take([operation](const QDomElement &iq) { return !pubsubChild(iq, operation).isNull(); });
    }
    QStringList pendingPubsub(const QString &operation) const
    {
        QStringList ids;
        for (const auto &iq : stream.sent) {
            if (iq.hasAttribute(QStringLiteral("answered")))
                continue;
            const auto op = pubsubChild(iq, operation);
            if (!op.isNull())
                ids.append(op.firstChildElement(QStringLiteral("item")).attribute(QStringLiteral("id")));
        }
        return ids;
    }

    // Server with PEP, publish-options and XEP-0402 #compat; legacy storage
    // holds the given conferences.
    void login(BookmarksManager &bookmarks, bool compat, const QString &legacyRooms)
    {
        QMetaObject::invokeMethod(client.serverInfoManager(), "initialize", Qt::DirectConnection);
        reply(discoTo(QStringLiteral("example.test")),
              QStringLiteral("<query xmlns='http://jabber.org/protocol/disco#info'>"
                             "<feature var='http://jabber.org/protocol/pubsub#publish-options'/></query>"));
        reply(discoTo(QStringLiteral("alice@example.test")),
              QStringLiteral("<query xmlns='http://jabber.org/protocol/disco#info'>"
                             "<identity category='pubsub' type='pep'/>%1"
                             "<feature var='http://jabber.org/protocol/pubsub#publish-options'/></query>")
                  .arg(compat ? QStringLiteral("<feature var='urn:xmpp:bookmarks:1#compat'/>") : QString()));
        bookmarks.setOnline(true);
        reply(privateXml(QStringLiteral("get")),
              QStringLiteral("<query xmlns='jabber:iq:private'><storage xmlns='storage:bookmarks'>%1</storage></query>")
                  .arg(legacyRooms));
    }
};

QString itemsPayload(const QStringList &rooms)
{
    QString items;
    for (const auto &room : rooms)
        items += QStringLiteral("<item id='%1'><conference xmlns='urn:xmpp:bookmarks:1' name='%1'/></item>").arg(room);
    return QStringLiteral("<pubsub xmlns='http://jabber.org/protocol/pubsub'>"
                          "<items node='urn:xmpp:bookmarks:1'>%1</items></pubsub>")
        .arg(items);
}

QStringList roomsOf(const BookmarksManager &bookmarks)
{
    QStringList rooms;
    for (const auto &room : bookmarks.conferences())
        rooms.append(room.room().bare());
    rooms.sort();
    return rooms;
}

NativeBookmark bookmark(const QString &room, const QString &name = QStringLiteral("Room"), bool autojoin = false)
{
    return NativeBookmark::create(Jid(room), name, autojoin);
}

void consecutiveEditsBeforeAcknowledgement()
{
    Server           server;
    BookmarksManager bookmarks(&server.client);
    server.login(bookmarks, true, {});
    server.reply(server.pubsub(QStringLiteral("items")), itemsPayload({}));
    check(bookmarks.usesNativeBookmarks(), "XEP-0402 was not selected with #compat");

    check(bookmarks.upsertConference(bookmark(room1)), "First edit rejected");
    check(bookmarks.upsertConference(bookmark(room2)), "Second edit rejected");
    check(roomsOf(bookmarks) == QStringList({ room1, room2 }), "Optimistic list lost an unacknowledged edit");

    server.reply(server.pubsub(QStringLiteral("publish")));
    server.reply(server.pubsub(QStringLiteral("publish")));
    check(server.pendingPubsub(QStringLiteral("retract")).isEmpty(), "A later edit retracted the earlier one");
    check(roomsOf(bookmarks) == QStringList({ room1, room2 }), "Acknowledged rooms are missing");

    // Editing one room must not revert a pending edit of another.
    check(bookmarks.upsertConference(bookmark(room1, QStringLiteral("Renamed"))), "Rename rejected");
    check(bookmarks.upsertConference(bookmark(room2, QStringLiteral("Room"), true)), "Autojoin edit rejected");
    for (const auto &room : bookmarks.conferences())
        if (room.room().bare() == room1)
            check(room.name() == QStringLiteral("Renamed"), "Second edit reverted the first room's rename");
}

void rejectedItemDoesNotBlockOthers()
{
    Server           server;
    BookmarksManager bookmarks(&server.client);
    QStringList      errors;
    QObject::connect(&bookmarks, &BookmarksManager::syncError, [&errors](const QString &e) { errors.append(e); });
    server.login(bookmarks, true, {});
    server.reply(server.pubsub(QStringLiteral("items")), itemsPayload({}));

    check(bookmarks.upsertConference(bookmark(room1)), "Edit rejected");
    server.reply(server.pubsub(QStringLiteral("publish")),
                 QStringLiteral("<error type='modify'><not-acceptable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                                "</error>"),
                 QStringLiteral("error"));
    check(!errors.isEmpty(), "Permanent rejection was not reported");
    check(!roomsOf(bookmarks).contains(room1), "Rejected room stayed in the optimistic list");
    check(bookmarks.upsertConference(bookmark(room2)), "Edit after a rejection was refused");
    check(server.pendingPubsub(QStringLiteral("publish")) == QStringList({ room2 }),
          "A rejected item blocked or was resent with later edits");
}

void urlsAreReadOnlyWithNativeBookmarks()
{
    Server           server;
    BookmarksManager bookmarks(&server.client);
    server.login(bookmarks, true, {});
    server.reply(server.pubsub(QStringLiteral("items")), itemsPayload({}));
    check(!bookmarks.setUrls({ { QStringLiteral("Link"), QStringLiteral("https://example.test") } }),
          "URL edit accepted with XEP-0402");
    check(server.privateXml(QStringLiteral("set")).isNull(), "Private XML was written in native mode");
}

void editDuringNativeActivationKeepsLegacyRooms()
{
    Server           server;
    BookmarksManager bookmarks(&server.client);
    server.login(bookmarks, true, QStringLiteral("<conference jid='%1' name='L'/>").arg(legacy));
    const auto items = server.pubsub(QStringLiteral("items")); // PEP load still outstanding
    check(!items.isNull(), "Native bookmarks were not requested");
    check(roomsOf(bookmarks) == QStringList({ legacy }), "Legacy rooms not shown before PEP load");

    check(bookmarks.upsertConference(bookmark(room1)), "Edit during activation rejected");
    check(roomsOf(bookmarks) == QStringList({ legacy, room1 }),
          "An edit during PEP activation hid the existing legacy rooms");

    // The server advertises #compat but has not migrated the legacy rooms:
    // the manager keeps Private XML, which must now receive the edit.
    server.reply(items, itemsPayload({}));
    check(!bookmarks.usesNativeBookmarks(), "Empty unmigrated PEP node replaced legacy bookmarks");
    const auto set = server.privateXml(QStringLiteral("set"));
    check(!set.isNull(), "Edit made during activation was never saved");
    QStringList saved;
    const auto  storage = set.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("storage"));
    for (auto c = storage.firstChildElement(QStringLiteral("conference")); !c.isNull();
         c      = c.nextSiblingElement(QStringLiteral("conference")))
        saved.append(c.attribute(QStringLiteral("jid")));
    saved.sort();
    check(saved == QStringList({ legacy, room1 }), "Legacy save lost a room");
}

void editDuringNativeActivationIsPublished()
{
    Server           server;
    BookmarksManager bookmarks(&server.client);
    server.login(bookmarks, true, QStringLiteral("<conference jid='%1' name='L'/>").arg(legacy));
    const auto items = server.pubsub(QStringLiteral("items"));
    check(bookmarks.upsertConference(bookmark(room1)), "Edit during activation rejected");
    // Migrated node: legacy room exists in PEP too.
    server.reply(items, itemsPayload({ legacy }));
    check(bookmarks.usesNativeBookmarks(), "Migrated PEP node was not selected");
    check(roomsOf(bookmarks) == QStringList({ legacy, room1 }), "Activation lost the pending edit");
    if (server.pendingPubsub(QStringLiteral("publish")) != QStringList({ room1 }))
        qWarning() << "published after activation:" << server.pendingPubsub(QStringLiteral("publish"));
    check(server.pendingPubsub(QStringLiteral("publish")) == QStringList({ room1 }),
          "Only the edited room should be published after activation");
    check(server.privateXml(QStringLiteral("set")).isNull(), "Private XML written after activation");
}
void editDuringReloadAfterReconnect()
{
    Server           server;
    BookmarksManager bookmarks(&server.client);
    server.login(bookmarks, true, {});
    server.reply(server.pubsub(QStringLiteral("items")), itemsPayload({ room1, room2 }));
    check(bookmarks.usesNativeBookmarks(), "XEP-0402 not selected");

    bookmarks.setOnline(false);
    bookmarks.setOnline(true);
    server.reply(server.privateXml(QStringLiteral("get")),
                 QStringLiteral("<query xmlns='jabber:iq:private'><storage xmlns='storage:bookmarks'/></query>"));
    const auto items = server.pubsub(QStringLiteral("items"));
    check(!items.isNull(), "Native node not reloaded after reconnect");
    check(roomsOf(bookmarks) == QStringList({ room1, room2 }), "Reconnect hid the known rooms");
    check(bookmarks.upsertConference(bookmark(room2, QStringLiteral("Renamed"))), "Edit during reload rejected");
    check(roomsOf(bookmarks) == QStringList({ room1, room2 }), "Edit during reload hid other rooms");
    server.reply(items, itemsPayload({ room1, room2 }));
    check(server.pendingPubsub(QStringLiteral("publish")) == QStringList({ room2 }),
          "Reload republished untouched rooms instead of only the edit");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    consecutiveEditsBeforeAcknowledgement();
    rejectedItemDoesNotBlockOthers();
    urlsAreReadOnlyWithNativeBookmarks();
    editDuringNativeActivationIsPublished();
    editDuringNativeActivationKeepsLegacyRooms();
    editDuringReloadAfterReconnect();
    return 0;
}
