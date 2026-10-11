// SPDX-License-Identifier: LGPL-2.1-or-later
// Reuse the real Session scheduler fixtures and recording XMPP stream.
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main
#include <QEventLoop>
#include <QPointer>
#include <QTimer>
#include <iris/jingle-s5b.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp.h>
#include <iris/xmpp_clientstream.h>

class NoNetworkConnector : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &stanza) override { written.append(stanza.element().cloneNode(true).toElement()); }
    QList<QDomElement> written;
};

static void flush()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

static void wait(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

static void presence(Client &client, const Jid &peer, bool available)
{
    Status status;
    status.setIsAvailable(available);
    check(
        QMetaObject::invokeMethod(&client, "ppPresence", Qt::DirectConnection, Q_ARG(Jid, peer), Q_ARG(Status, status)),
        "Parsed presence was not dispatched");
}

static void testOffline(bool acknowledgeInitialIq, bool incomingActivity, bool confirmOnline = false)
{
    NoNetworkConnector connector;
    RecordingStream    stream(&connector);
    TcpPortReserver    reserver;
    Client             client;
    client.setTcpPortReserver(&reserver);
    const Jid peer("peer@example.test/device"); // Deliberately not in the roster.
    client.connectToServer(&stream, Jid("local@example.test/device"));
    QPointer<J::Session> session = client.jingleManager()->newSession(peer);
    class PreparedApplication : public TestApplication {
    public:
        using TestApplication::TestApplication;
        void prepare() override { setState(J::State::ApprovedToSend); }
    };
    auto                     content = new PreparedApplication(session);
    QPointer<J::Application> contentGuard(content);
    session->addContent(content);
    content->attachTransport();
    session->initiate();
    flush();
    check(!stream.written.isEmpty(), "Initial IQ was not sent");
    const auto initial = stream.written.last();
    if (acknowledgeInitialIq) {
        auto ack = client.doc()->createElement("iq");
        ack.setAttribute("from", peer.full());
        ack.setAttribute("id", initial.attribute("id"));
        ack.setAttribute("type", "result");
        check(client.rootTask()->take(ack), "Initial IQ was not acknowledged");
    }
    content->activate();
    auto timer = session->findChild<QTimer *>(QStringLiteral("jinglePeerUnavailableTimer"));
    check(timer && timer->interval() == 10000, "Production grace interval differs from XEP-0166");
    timer->setInterval(100); // Exercise the real timeout without a ten-second test delay.
    QPointer<J::TransportManagerPad> retainedPad;
    int                              terminated = 0;
    QObject::connect(session, &J::Session::terminated, &client, [&] {
        ++terminated;
        check(session->state() == J::State::Finished && contentGuard->state() == J::State::Finished,
              "Channel callbacks ran before the session/application became terminal");
        check(!client.jingleManager()->session(peer, session->sid()), "Ended SID is still registered");
        if (!acknowledgeInitialIq) {
            auto lateAck = client.doc()->createElement("iq");
            lateAck.setAttribute("from", peer.full());
            lateAck.setAttribute("id", initial.attribute("id"));
            lateAck.setAttribute("type", "result");
            check(client.rootTask()->take(lateAck), "Pending IQ task disappeared before termination callback");
            check(session->state() == J::State::Finished, "Late IQ result revived the finished session");
        }
        content->setTransportUpdates(true); // A late transport callback must produce no outgoing IQ.
        emit content->updated();
    });
    const auto sent = stream.written.size();
    presence(client, Jid("peer@example.test/another-device"), false);
    check(!timer->isActive(), "Sibling resource incorrectly armed session teardown");
    presence(client, peer, false);
    check(timer->isActive() && terminated == 0, "Unavailable presence ended the session before its grace interval");
    wait(55);
    if (incomingActivity) {
        auto activityTransport = session->newOutgoingTransport(J::S5B::NS);
        check(bool(activityTransport), "Activity fixture transport was not created");
        auto transportPad = activityTransport->pad();
        retainedPad       = transportPad;
        transportPad->noteIncomingActivity();
        wait(65);
        check(session && terminated == 0, "Incoming transport activity did not extend the grace interval");
        // A new online presence alone cannot prove the previous SID survived.
        presence(client, peer, true);
        if (confirmOnline) {
            transportPad->noteIncomingActivity();
            check(!timer->isActive(), "Confirmed online activity did not cancel the offline deadline");
            wait(120);
            check(session && terminated == 0, "Confirmed live session was still ended by the offline deadline");
            presence(client, peer, false); // A later outage must arm a new deadline.
        }
    } else {
        presence(client, peer, false); // Duplicate presence must not postpone teardown.
    }
    wait(confirmOnline ? 120 : (incomingActivity ? 70 : 65));
    check(terminated == 1, "Offline session stayed alive, possibly waiting for an IQ acknowledgement");
    flush();
    check(stream.written.size() == sent, "Offline teardown or a late callback sent an IQ to the unavailable peer");
    check(!session && !contentGuard, "Ended session or content was not deleted");
    check(!retainedPad, "Transport pad outlived its owning session");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    testOffline(true, false);
    testOffline(false, false);
    testOffline(true, true);
    testOffline(true, true, true);
    qInfo("Peer offline grace, activity, sibling-resource and pending-IQ regressions passed");
    return 0;
}
