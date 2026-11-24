// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the public Client facade backed by the internal MucManager.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>

#include <QCoreApplication>

using namespace XMPP;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

class OfflineConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream final : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &stanza) override
    {
        sent.append(document.importNode(stanza.element(), true).toElement());
    }
    bool stanzaAvailable() const override { return !incoming.isEmpty(); }
    Stanza read() override { return incoming.takeFirst(); }
    void inject(const QDomElement &element)
    {
        incoming.append(createStanza(element));
        emit readyRead();
    }
    QDomDocument document;
    QList<Stanza> incoming;
    QList<QDomElement> sent;
};

static QDomDocument occupantPresence(const QString &from, const QString &real, bool available = true)
{
    QDomDocument doc;
    auto root = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("presence"));
    doc.appendChild(root);
    root.setAttribute(QStringLiteral("from"), from);
    if (!available)
        root.setAttribute(QStringLiteral("type"), QStringLiteral("unavailable"));
    auto muc = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"), QStringLiteral("x"));
    auto item = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"), QStringLiteral("item"));
    item.setAttribute(QStringLiteral("jid"), real);
    item.setAttribute(QStringLiteral("role"), QStringLiteral("participant"));
    item.setAttribute(QStringLiteral("affiliation"), QStringLiteral("member"));
    muc.appendChild(item);
    root.appendChild(muc);
    return doc;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    OfflineConnector connector;
    RecordingStream stream(&connector);
    Client client;
    client.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(), QStringLiteral("desktop"));
    client.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));

    int joins = 0;
    int leaves = 0;
    int occupantUpdates = 0;
    QObject::connect(&client, &Client::groupChatJoined, &client, [&](const Jid &) { ++joins; });
    QObject::connect(&client, &Client::groupChatLeft, &client, [&](const Jid &) { ++leaves; });
    QObject::connect(&client, &Client::groupChatPresence, &client,
                     [&](const Jid &, const Status &) { ++occupantUpdates; });

    const QString host = QStringLiteral("conference.example.test");
    const QString room = QStringLiteral("garden");
    const QString self = QStringLiteral("garden@conference.example.test/Alice");
    const QString bare = QStringLiteral("garden@conference.example.test");
    check(client.groupChatJoin(host, room, QStringLiteral("Alice"), QStringLiteral("secret")),
          "Initial MUC join was rejected");
    check(!client.groupChatJoin(host, room, QStringLiteral("Alice")),
          "A duplicate active MUC join was allowed");
    check(client.groupChatPassword(host, room) == QStringLiteral("secret"), "MUC password was lost");
    check(client.groupChatNick(host, room) == QStringLiteral("Alice"), "MUC nickname was lost");

    stream.inject(occupantPresence(self, QStringLiteral("alice@example.test/desktop")).documentElement());
    check(joins == 1 && occupantUpdates == 1, "MUC join and presence signals changed");
    stream.inject(occupantPresence(bare + QStringLiteral("/Bob"), QStringLiteral("bob@example.test/phone"))
                      .documentElement());
    check(joins == 1 && occupantUpdates == 2, "MUC occupant updates changed");

    client.groupChatChangeNick(host, room, QStringLiteral("Alice2"), Status());
    check(client.groupChatNick(host, room) == QStringLiteral("Alice2"), "MUC nickname change was lost");
    stream.inject(occupantPresence(bare + QStringLiteral("/Alice2"), QStringLiteral("alice@example.test/desktop"))
                      .documentElement());

    client.groupChatLeave(host, room);
    stream.inject(occupantPresence(bare + QStringLiteral("/Alice2"), QStringLiteral("alice@example.test/desktop"), false)
                      .documentElement());
    check(leaves == 1, "MUC leave signal was lost");
    check(client.groupChatJoin(host, room, QStringLiteral("Alice")),
          "Rejoining a room after leave failed");
    stream.inject(occupantPresence(self, QStringLiteral("alice@example.test/desktop")).documentElement());
    check(joins == 2, "MUC state was not reset on rejoin");

    client.close(true);
    check(client.groupChatNick(host, room).isEmpty(), "A disconnected room retained its nickname");
    qInfo("Iris MucManager lifecycle facade test passed");
    return 0;
}
