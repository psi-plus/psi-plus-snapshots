// SPDX-License-Identifier: LGPL-2.1-or-later
// A whiteboard added with content-add to a running session that carries another application,
// as when a whiteboard is added to a call. Reuses the looped-back two-client fixture.
#define main iris_sxesession_main
#include "sxesession.cpp"
#undef main

#include <iris/jingle-ft.h>
#include <iris/xmpp-im/jingle-ibb.h>

#include <QBuffer>
#include <iris/xmpp_hash.h>

namespace FT = XMPP::Jingle::FileTransfer;

static SXE::Application *whiteboardOf(J::Session *session)
{
    for (auto content : session->contentList()) {
        auto whiteboard = qobject_cast<SXE::Application *>(content);
        if (whiteboard && whiteboard->state() < J::State::Finishing)
            return whiteboard;
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));

    QPointer<J::Session> bobSession;
    QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &app,
                     [&](J::Session *session) { bobSession = session; });

    // Hold the file transfer open until the whiteboard is added, like an active call.
    FT::File file;
    file.setName(QStringLiteral("note.txt"));
    file.setSize(4);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("data")));
    QBuffer source;
    source.setData("data");
    source.open(QIODevice::ReadOnly);

    QPointer<J::Session> session  = alice.client.jingleManager()->newSession(bob.jid);
    auto                 transfer = qobject_cast<FT::Application *>(session->newContent(FT::NS, J::Origin::Initiator));
    check(transfer, "file transfer content was not created");
    transfer->setFile(file);
    // Peer capabilities are unknown in this fixture: select the transport explicitly.
    check(transfer->setTransport(session->newOutgoingTransport(J::IBB::NS)), "IBB transport was not set");
    bool deviceRequested = false;
    QObject::connect(transfer, &FT::Application::deviceRequested, transfer,
                     [&](quint64, std::optional<quint64>) { deviceRequested = true; });
    session->addContent(transfer);
    session->initiate();
    pump(alice, bob);
    check(bobSession, "incoming session not offered");

    QBuffer sink;
    sink.open(QIODevice::WriteOnly);
    for (auto content : bobSession->contentList()) {
        if (auto received = qobject_cast<FT::Application *>(content)) {
            received->setAcceptFile(file);
            QObject::connect(received, &FT::Application::deviceRequested, &app,
                             [&sink, received](quint64, std::optional<quint64>) { received->setDevice(&sink, false); });
        }
    }
    bobSession->accept();
    pump(alice, bob);
    check(session && bobSession && session->state() == J::State::Active && bobSession->state() == J::State::Active,
          "the initial session was not established");

    // Add a whiteboard to the running session; the peer accepts it like a call accepts added media.
    bool added = false;
    QObject::connect(bobSession, &J::Session::newContentReceived, &app, [&] {
        if (auto whiteboard = whiteboardOf(bobSession)) {
            added = true;
            whiteboard->prepare();
        }
    });
    auto local = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD));
    check(local && local->isHost(), "whiteboard content was not created");
    session->addContent(local);
    pump(alice, bob);
    auto remote = whiteboardOf(bobSession);
    check(added && remote, "added whiteboard was not offered to the peer");
    check(local->syncState() == SXE::Application::SyncState::Synchronized
              && remote->syncState() == SXE::Application::SyncState::Synchronized,
          "added whiteboard did not synchronize");
    check(dump(remote->document()->document()) == dump(local->document()->document()),
          "added whiteboard has a different document");

    check(deviceRequested, "initial file was not requested");
    transfer->setDevice(&source, false);
    pump(alice, bob);
    check(session && bobSession && session->state() == J::State::Active && remote->state() == J::State::Active,
          "finishing the file terminated the active whiteboard");

    QDomDocument temp;
    local->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<rect xmlns='http://www.w3.org/2000/svg' id='r' width='5' height='5'/>")),
        local->document()->document().documentElement());
    local->document()->flush();
    pump(alice, bob);
    check(dump(remote->document()->document()) == dump(local->document()->document()),
          "edits of an added whiteboard were not routed");
    check(sink.data() == QByteArray("data"), "the other application was disturbed by the whiteboard");

    // A second SXE content in the same session is refused: payload is addressed by the sid.
    auto second = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD));
    check(second, "second whiteboard content was not created");
    QPointer<SXE::Application> secondGuard(second);
    session->addContent(second);
    pump(alice, bob);
    check(!secondGuard || secondGuard->state() >= J::State::Finishing, "a second SXE content was accepted");
    check(whiteboardOf(bobSession) == remote && remote->state() == J::State::Active,
          "refusing a second whiteboard disturbed the first one");

    session->terminate(J::Reason::Success);
    pump(alice, bob);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("SXE content-add regressions passed");
    return 0;
}
