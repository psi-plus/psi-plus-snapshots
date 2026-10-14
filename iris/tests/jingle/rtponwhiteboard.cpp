// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rtpcontentaddracefixture.h"

static void addMedia(bool responderAdds, const QString &ns, const QString &kind)
{
    auto            provider = std::make_shared<RaceProvider>();
    TcpPortReserver reserver;
    Endpoint        alice("alice"), bob("bob");
    for (auto endpoint : { &alice, &bob }) {
        endpoint->client.setTcpPortReserver(&reserver);
        endpoint->client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
        endpoint->client.jingleManager()->rtpManager()->setMediaProvider(provider);
        endpoint->client.jingleManager()->rtpManager()->setTransportNamespaces({ ns });
    }
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures());
    setPeerFeatures(bob.client, alice.jid, alice.client.jingleManager()->discoFeatures());
    QPointer<J::Session> incoming;
    auto observer = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                     [&](J::Session *session) { incoming = session; });
    auto session  = alice.client.jingleManager()->newSession(bob.jid);
    auto board    = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD));
    check(board && board->setTransport(session->newOutgoingTransport(ns)), "cannot create initial whiteboard");
    session->addContent(board);
    session->initiate();
    check(pumpUntil(alice, bob, [&] { return bool(incoming); }), "no incoming whiteboard");
    QObject::disconnect(observer);
    QPointer<SXE::Application> remoteBoard;
    for (auto content : incoming->contentList()) {
        remoteBoard = qobject_cast<SXE::Application *>(content);
        content->prepare();
    }
    incoming->accept();
    check(pumpUntil(alice, bob, [&] { return synchronized(board, remoteBoard); }), "initial whiteboard did not sync");
    check(dataChannel(board) && dataChannel(remoteBoard), "initial board did not use SCTP");
    const auto boardTransport       = board->transport();
    const auto remoteBoardTransport = remoteBoard->transport();
    auto       source               = responderAdds ? incoming.data() : session;
    auto       receiver             = responderAdds ? session : incoming.data();
    QObject::connect(receiver, &J::Session::newContentReceived, receiver, [receiver] {
        for (auto content : receiver->contentList())
            if (content->isRemote() && content->state() == J::State::Created)
                content->prepare();
    });
    const auto name  = kind + (responderAdds ? "-responder" : "-initiator");
    auto       media = source->manager()->rtpManager()->createOutgoing(
        source, kind == "microphone" ? R::Media::Audio : R::Media::Video, source->role(), name);
    check(media, "cannot add media to whiteboard session");
    check(pumpUntil(alice, bob,
                    [&] {
                        auto remote = receiver->content(name, source->role());
                        return media->state() == J::State::Active && remote && remote->state() == J::State::Active;
                    }),
          "media added after plain-DTLS whiteboard did not become active");
    check(!bundled(source, board->contentName(), name) && !bundled(receiver, board->contentName(), name),
          "SRTP content reused a plain-DTLS whiteboard association");
    auto packets = dynamic_cast<R::PacketTransport *>(media->transport().data());
    check(packets && packets->rtpAssociation() && packets->rtpAssociation()->isReady(), "media has no secure RTP");
    check(board->transport() == boardTransport && remoteBoard->transport() == remoteBoardTransport
              && synchronized(board, remoteBoard),
          "adding media disturbed the initial whiteboard");
    QDomDocument doc;
    board->document()->insertNodeAfter(parse(doc, "<rect xmlns='http://www.w3.org/2000/svg' id='after-media'/>"),
                                       board->document()->document().documentElement());
    board->document()->flush();
    check(pumpUntil(alice, bob,
                    [&] { return dump(board->document()->document()) == dump(remoteBoard->document()->document()); }),
          "whiteboard updates stopped after media joined");
    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    if (!Dtls::isSupported() || Dtls::supportedSRTPProfiles().isEmpty()) {
        qInfo("RTP after whiteboard tests skipped: no DTLS-SRTP backend");
        return 0;
    }
    for (const auto &ns : { J::ICE::NS, J::ICE::NS_ICE_UDP })
        for (bool responderAdds : { false, true })
            for (const auto &kind :
                 { QStringLiteral("microphone"), QStringLiteral("camera"), QStringLiteral("screen") })
                addMedia(responderAdds, ns, kind);
    qInfo("RTP after whiteboard regressions passed");
}
