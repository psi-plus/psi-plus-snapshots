// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the transport-replace fixture to verify that an invalid member of a
// transport-reject batch cannot partially commit an earlier valid member.
#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

static void testInvalidLaterRejectIsAtomic(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("reject-atomic@example.test/device")),
                       J::Origin::Initiator);

    auto audioOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-old"));
    auto audioFallback
        = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("audio-fallback"));
    auto audioSelector = std::make_unique<TestSelector>();
    audioSelector->backupTransport(audioFallback);
    auto audio
        = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld, std::move(audioSelector));
    audio->markReplaceInProgress();

    auto videoOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-old"));
    auto video    = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                   std::make_unique<TestSelector>());
    // The peer cannot reject a replacement that has not reached InProgress.
    video->markReplacePlanned();

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReject,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-rejected") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-out-of-order") } }));

    check(!ok, "transport-reject batch with an out-of-order member was accepted");
    check(audio->transport().data() == audioOld.data(),
          "invalid later transport-reject partially replaced an earlier valid member");
    check(audio->replaceInProgress(),
          "invalid later transport-reject partially completed an earlier replacement transaction");
    check(video->transport().data() == videoOld.data() && video->replacePlanned(),
          "invalid transport-reject member changed its application state");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testInvalidLaterRejectIsAtomic(client);

    qInfo("Transport-reject atomicity regression passed");
    return 0;
}
