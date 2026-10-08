// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/xmpp_ibb.h"

#include "../../src/xmpp/xmpp-im/jingle-ibb.h"
#include <iris/jingle-session.h>
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>

#include <QCoreApplication>
#include <QPointer>
#include <QtCrypto>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class NoNetworkConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream final : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &stanza) override { written.append(stanza.element().cloneNode(true).toElement()); }

    QList<QDomElement> written;
};

static void flushEvents()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

static void acknowledgeLast(Client &client, const RecordingStream &stream, const Jid &peer)
{
    check(!stream.written.isEmpty(), "IBB fixture did not write an IQ");
    const auto sent = stream.written.constLast();
    auto       ack  = client.doc()->createElement(QStringLiteral("iq"));
    ack.setAttribute(QStringLiteral("from"), peer.full());
    ack.setAttribute(QStringLiteral("id"), sent.attribute(QStringLiteral("id")));
    ack.setAttribute(QStringLiteral("type"), QStringLiteral("result"));
    check(client.rootTask()->take(ack), "IBB fixture acknowledgement was not consumed");
    flushEvents();
}

static void testPendingJingleClose(Client &client, const RecordingStream &stream, const Jid &peer)
{
    namespace J = XMPP::Jingle;
    class Success final : public Task {
    public:
        Success(Task *parent) : Task(parent) { setSuccess(); }
    } success(client.rootTask());
    J::Session session(client.jingleManager(), peer);
    auto       transport  = session.newOutgoingTransport(J::IBB::NS);
    auto       connection = transport->addChannel(J::TransportFeature::DataOriented, QStringLiteral("pending-close"));
    check(bool(connection), "Jingle IBB channel missing");
    transport->prepare();
    auto update = transport->takeOutgoingUpdate(false);
    check(bool(std::get<1>(update)), "Jingle IBB offer has no acknowledgement");
    std::get<1>(update)(&success);
    check(transport->update(std::get<0>(update)), "Jingle IBB answer was rejected");
    transport->start();
    flushEvents();
    acknowledgeLast(client, stream, peer);
    check(connection->isOpen(), "Jingle IBB connection did not open");
    auto packet = client.doc()->createElement(QStringLiteral("iq"));
    packet.setAttribute(QStringLiteral("from"), peer.full());
    packet.setAttribute(QStringLiteral("type"), QStringLiteral("set"));
    packet.setAttribute(QStringLiteral("id"), QStringLiteral("buffered-tail"));
    auto data = client.doc()->createElementNS(QStringLiteral("http://jabber.org/protocol/ibb"), QStringLiteral("data"));
    data.setAttribute(QStringLiteral("sid"), std::get<0>(update).attribute(QStringLiteral("sid")));
    data.setAttribute(QStringLiteral("seq"), QStringLiteral("0"));
    data.appendChild(client.doc()->createTextNode(QString::fromLatin1(QByteArray("tail").toBase64())));
    packet.appendChild(data);
    check(client.rootTask()->take(packet), "Incoming IBB packet was not consumed");
    check(connection->bytesAvailable() == 4, "Incoming IBB tail was not buffered");
    // Graceful close retains unread bytes; shutdown must not busy-loop waiting
    // for this consumer to resume reading.
    transport->pad()->manager()->closeAll();
    check(transport->state() == J::State::Finished, "Pending IBB close prevented manager shutdown");
    check(!connection->isOpen() && connection->bytesAvailable() == 0,
          "IBB abort retained an open connection or unread buffered tail");
}

int main(int argc, char **argv)
{
    QCoreApplication   app(argc, argv);
    QCA::Initializer   qca;
    NoNetworkConnector connector;
    RecordingStream    stream(&connector);
    Client             client;
    const Jid          peer(QStringLiteral("peer@example.test/device"));
    client.connectToServer(&stream, Jid(QStringLiteral("local@example.test/device")));

    auto *connection = static_cast<IBBConnection *>(client.ibbManager()->createConnection());
    check(connection, "IBB fixture could not create a connection");
    QPointer<IBBConnection> guard(connection);
    bool                    connected = false;
    QObject::connect(connection, &IBBConnection::connected, &app, [&]() { connected = true; });

    connection->connectToJid(peer, QStringLiteral("reentrant-close"));
    flushEvents();
    acknowledgeLast(client, stream, peer);
    check(connected && connection->isOpen(), "IBB fixture did not open the connection");

    const QByteArray payload("payload");
    check(connection->write(payload) == payload.size(), "IBB fixture could not queue payload");
    flushEvents();
    acknowledgeLast(client, stream, peer);

    QObject::connect(connection, &ByteStream::delayedCloseFinished, &app, [&]() {
        delete connection;
        connection = nullptr;
    });
    connection->close();
    flushEvents();
    acknowledgeLast(client, stream, peer);
    check(!guard && !connection, "IBB close completion did not exercise synchronous destruction");

    testPendingJingleClose(client, stream, peer);
    qInfo("IBB reentrant/pending close regressions passed");
    return 0;
}
