// SPDX-License-Identifier: LGPL-2.1-or-later
// SXE payload on an ICE SCTP data channel between two clients, and the fallback to SXE messages
// when the channel breaks. Reuses the looped-back two-client fixture; ICE runs on localhost.
#define main iris_sxesession_main
#include "sxesession.cpp"
#undef main

#include <iris/jingle-ft.h>
#include <iris/jingle-ice.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_hash.h>

#include <QBuffer>
#include <QElapsedTimer>
#include <QThread>

#include <functional>

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-sxe-connection"));
    disco.setFeatures(Features(features));
    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

// Exchanges stanzas and lets the localhost ICE and SCTP traffic run until done() or the timeout.
static bool pumpUntil(Endpoint &a, Endpoint &b, const std::function<bool()> &done, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        pump(a, b);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    pump(a, b);
    return done();
}

static int sxeMessages()
{
    int count = 0;
    for (const auto &message : std::as_const(sentMessages))
        if (!message.firstChildElement(QStringLiteral("sxe")).isNull())
            ++count;
    return count;
}

static J::Connection::Ptr dataChannel(SXE::Application *application)
{
    if (!application->transport())
        return {};
    for (const auto &channel : application->transport()->channels())
        if (channel && channel->features() & J::TransportFeature::MessageOriented
            && channel->features() & J::TransportFeature::DataOriented)
            return channel;
    return {};
}

static bool synchronized(SXE::Application *a, SXE::Application *b)
{
    return a && b && a->syncState() == SXE::Application::SyncState::Synchronized
        && b->syncState() == SXE::Application::SyncState::Synchronized;
}

static bool bundled(J::Session *session, const QString &a, const QString &b)
{
    for (const auto &group : session->negotiatedGroupings())
        if (group.semantics == QLatin1String("BUNDLE") && group.contents.contains(a) && group.contents.contains(b))
            return true;
    return false;
}

// A whiteboard added to a session whose content already runs on ICE, as when a whiteboard is added to a
// call, joins that content's BUNDLE association and its own data channel.
static void addToBundle(Endpoint &alice, Endpoint &bob)
{
    namespace FT = XMPP::Jingle::FileTransfer;
    QPointer<J::Session> bobSession;
    auto connection = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                       [&](J::Session *session) { bobSession = session; });
    Q_UNUSED(connection)

    FT::File file;
    file.setName(QStringLiteral("note.txt"));
    file.setSize(4);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("data")));
    QBuffer source;
    source.setData("data");
    source.open(QIODevice::ReadOnly);
    QBuffer sink;
    sink.open(QIODevice::WriteOnly);

    auto session  = alice.client.jingleManager()->newSession(bob.jid);
    auto transfer = qobject_cast<FT::Application *>(session->newContent(FT::NS, J::Origin::Initiator));
    check(transfer && transfer->setTransport(session->newOutgoingTransport(J::ICE::NS)),
          "file transfer over ICE was not created");
    transfer->setFile(file);
    // The file is held back so the transfer stays active, like the media of a call.
    bool deviceRequested = false;
    QObject::connect(transfer, &FT::Application::deviceRequested, transfer,
                     [&](quint64, std::optional<quint64>) { deviceRequested = true; });
    session->addContent(transfer);
    session->initiate();
    pump(alice, bob);
    check(bobSession, "incoming file transfer session not offered");
    for (auto content : bobSession->contentList()) {
        if (auto received = qobject_cast<FT::Application *>(content)) {
            received->setAcceptFile(file);
            QObject::connect(received, &FT::Application::deviceRequested, received,
                             [&sink, received](quint64, std::optional<quint64>) { received->setDevice(&sink, false); });
        }
    }
    bobSession->accept();
    check(pumpUntil(alice, bob,
                    [&] { return session->state() == J::State::Active && transfer->state() == J::State::Active; }),
          "the file transfer over ICE did not start");

    QPointer<SXE::Application> remote;
    QObject::connect(bobSession, &J::Session::newContentReceived, bobSession, [&] {
        for (auto content : bobSession->contentList())
            if (auto whiteboard = qobject_cast<SXE::Application *>(content); whiteboard && !remote) {
                remote = whiteboard;
                whiteboard->prepare();
            }
    });
    const int messagesBefore = sxeMessages();
    auto      local          = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD));
    check(local && local->allowsSharedTransport(), "the whiteboard may not share the association");
    session->addContent(local);
    check(pumpUntil(alice, bob, [&] { return synchronized(local, remote); }),
          "the whiteboard added to the ICE session did not synchronize");
    check(bundled(session, transfer->contentName(), local->contentName())
              && bundled(bobSession, transfer->contentName(), local->contentName()),
          "the whiteboard did not join the BUNDLE group of the running content");
    check(local->transport()->pad()->ns() == J::ICE::NS && dataChannel(local),
          "the bundled whiteboard has no data channel");
    check(sxeMessages() == messagesBefore, "the bundled whiteboard sent its payload in messages");

    // The shared association still carries the file.
    check(deviceRequested, "the file was not requested");
    transfer->setDevice(&source, false);
    check(pumpUntil(alice, bob, [&] { return sink.data() == "data"; }), "the bundled file transfer did not complete");

    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
}

// Both sides add a whiteboard to a running ICE session at once, as when both participants of a call asked
// for one: the content-adds cross with the same name. XEP-0166 lets the initiator's overrule; the
// responder withdraws its own and joins the initiator's, which ends up in the BUNDLE group.
static void crossingAdds(Endpoint &alice, Endpoint &bob)
{
    namespace FT = XMPP::Jingle::FileTransfer;
    QPointer<J::Session> bobSession;
    auto connection = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                       [&](J::Session *session) { bobSession = session; });

    FT::File file;
    file.setName(QStringLiteral("held.txt"));
    file.setSize(4);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("data")));
    auto session  = alice.client.jingleManager()->newSession(bob.jid);
    auto transfer = qobject_cast<FT::Application *>(session->newContent(FT::NS, J::Origin::Initiator));
    check(transfer && transfer->setTransport(session->newOutgoingTransport(J::ICE::NS)),
          "file transfer over ICE was not created");
    transfer->setFile(file);
    session->addContent(transfer);
    session->initiate();
    pump(alice, bob);
    QObject::disconnect(connection);
    check(bobSession, "incoming file transfer session not offered");
    for (auto content : bobSession->contentList())
        if (auto received = qobject_cast<FT::Application *>(content))
            received->setAcceptFile(file);
    bobSession->accept();
    check(pumpUntil(alice, bob,
                    [&] { return session->state() == J::State::Active && transfer->state() == J::State::Active; }),
          "the file transfer over ICE did not start");

    QPointer<SXE::Application> joined;
    QObject::connect(bobSession, &J::Session::newContentReceived, bobSession, [&] {
        for (auto content : bobSession->contentList())
            if (auto whiteboard = qobject_cast<SXE::Application *>(content);
                whiteboard && whiteboard->isRemote() && !joined) {
                joined = whiteboard;
                whiteboard->prepare();
            }
    });
    int errors = 0;
    onStanza   = [&errors](const Endpoint &, QDomElement &stanza) {
        if (stanza.tagName() == QLatin1String("iq")
            && stanza.attribute(QStringLiteral("type")) == QLatin1String("error")
            && stanza.firstChildElement(QStringLiteral("error"))
                   .elementsByTagName(QStringLiteral("tie-break"))
                   .isEmpty())
            ++errors;
    };
    auto aliceBoard = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD));
    QPointer<SXE::Application> bobBoard
        = qobject_cast<SXE::Application *>(bobSession->newContent(SXE::NS_SVG_WHITEBOARD));
    check(aliceBoard && bobBoard && aliceBoard->contentName() == bobBoard->contentName(),
          "the crossing whiteboards were not created with the same name");
    session->addContent(aliceBoard);
    bobSession->addContent(bobBoard);
    check(pumpUntil(alice, bob, [&] { return synchronized(aliceBoard, joined); }),
          "the initiator's whiteboard did not win the crossing content-add");
    check(!bobBoard || bobBoard->state() >= J::State::Finishing, "the responder's crossing whiteboard stayed");
    check(errors == 0, "crossing content-adds were refused with errors other than tie-break");
    check(bundled(session, transfer->contentName(), aliceBoard->contentName())
              && bundled(bobSession, transfer->contentName(), aliceBoard->contentName()),
          "the winning whiteboard did not join the BUNDLE group");
    check(!dataChannel(aliceBoard).isNull(), "the winning whiteboard has no data channel");
    onStanza = nullptr;

    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    TcpPortReserver  reserver;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        endpoint->client.setTcpPortReserver(&reserver);
        endpoint->client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    }
    const auto features = alice.client.jingleManager()->discoFeatures();
    if (!features.contains(SXE::NS_CONNECTIONS)) {
        // Without SCTP over ICE (or DTLS) there is no connection transport to test.
        qInfo("SXE connection tests skipped: no transport can carry SXE messages");
        return 0;
    }
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures());
    setPeerFeatures(bob.client, alice.jid, features);

    QPointer<J::Session> bobSession;
    QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &app,
                     [&](J::Session *session) { bobSession = session; });

    auto session = alice.client.jingleManager()->newSession(bob.jid);
    auto content = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD, J::Origin::Both));
    check(content && content->isHost(), "whiteboard content was not created");
    session->addContent(content);
    session->initiate();
    pump(alice, bob);
    check(bobSession, "incoming whiteboard session not offered");
    auto remote = qobject_cast<SXE::Application *>(
        bobSession->contentList().value(J::ContentKey { content->contentName(), J::Origin::Initiator }));
    check(remote, "incoming whiteboard content missing");
    remote->prepare();
    bobSession->accept();

    check(pumpUntil(alice, bob, [&] { return synchronized(content, remote); }),
          "whiteboard did not synchronize over the connection");
    check(content->transport()->pad()->ns() != SXE::NS_TRANSPORT
              && remote->transport()->pad()->ns() == content->transport()->pad()->ns(),
          "a connection transport was not selected");
    check(sxeMessages() == 0, "SXE payload went through messages although a connection was available");
    check(dump(remote->document()->document()) == dump(content->document()->document()),
          "synchronized documents differ");

    // Edits in both directions, including character data far beyond one message of the message transport.
    QDomDocument temp;
    content->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<rect xmlns='http://www.w3.org/2000/svg' id='r' width='5' height='5'/>")),
        content->document()->document().documentElement());
    content->document()->flush();
    const auto big  = QString(300000, QLatin1Char('x'));
    auto       rect = content->document()->document().documentElement().lastChildElement();
    content->document()->setAttribute(rect, QStringLiteral("data-big"), big);
    content->document()->flush();
    remote->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<circle xmlns='http://www.w3.org/2000/svg' id='c' r='3'/>")),
        remote->document()->document().documentElement());
    remote->document()->flush();
    check(pumpUntil(alice, bob,
                    [&] {
                        return dump(remote->document()->document()) == dump(content->document()->document())
                            && !remote->document()->document().documentElement().lastChildElement().isNull()
                            && content->document()->document().elementsByTagName(QStringLiteral("circle")).size() == 1;
                    }),
          "edits over the connection were not exchanged");
    check(sxeMessages() == 0, "edits went through messages");

    // The channel breaks: the content moves to the next transport, which may be another connection
    // transport, and finally to SXE messages. The joiner synchronizes again after every replacement.
    for (int attempt = 0; content->transport()->pad()->ns() != SXE::NS_TRANSPORT; ++attempt) {
        check(attempt < 4, "the whiteboard never fell back to SXE messages");
        const auto failedNs = content->transport()->pad()->ns();
        auto       channel  = dataChannel(content);
        check(bool(channel), "the data channel of the whiteboard was not found");
        channel->close();
        check(pumpUntil(alice, bob,
                        [&] {
                            return content->transport() && remote->transport()
                                && content->transport()->pad()->ns() != failedNs
                                && remote->transport()->pad()->ns() == content->transport()->pad()->ns()
                                && synchronized(content, remote)
                                && (content->transport()->pad()->ns() == SXE::NS_TRANSPORT || dataChannel(content));
                        }),
              "the whiteboard did not move to the next transport");
        check(dump(remote->document()->document()) == dump(content->document()->document()),
              "documents differ after a transport replacement");
    }
    check(sxeMessages() > 0, "no SXE messages after the fallback");

    content->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<line xmlns='http://www.w3.org/2000/svg' id='l' x2='9' y2='9'/>")),
        content->document()->document().documentElement());
    content->document()->flush();
    check(pumpUntil(alice, bob,
                    [&] { return dump(remote->document()->document()) == dump(content->document()->document()); }),
          "edits after the fallback were not exchanged");
    check(content->state() == J::State::Active && remote->state() == J::State::Active,
          "the whiteboard content did not stay active");

    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);

    addToBundle(alice, bob);
    crossingAdds(alice, bob);

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("SXE connection, fallback and BUNDLE tests passed");
    return 0;
}
