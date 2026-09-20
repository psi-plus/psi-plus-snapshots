// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the transport-replace fixture so this regression exercises the same
// Application replacement state machine as the protocol tests.
#include <functional>

#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

class ReentrantRejectSelector : public TestSelector {
public:
    QSharedPointer<J::Transport> getNextTransport() override
    {
        auto result   = TestSelector::getNextTransport();
        auto callback = std::move(onGetNext);
        if (callback)
            callback();
        return result;
    }

    std::function<void()> onGetNext;
};

static void testRejectSkipsReentrantStaleSibling(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("reject-reentrant@example.test/device")),
                       J::Origin::Initiator);

    auto audioOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-old"));
    auto audioFallback
        = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("audio-fallback"));
    auto audioSelector = std::make_unique<ReentrantRejectSelector>();
    audioSelector->backupTransport(audioFallback);
    auto audioSelectorRaw = audioSelector.get();
    auto audio
        = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld, std::move(audioSelector));
    audio->markReplaceInProgress();

    auto videoOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-old"));
    auto video    = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                   std::make_unique<TestSelector>());
    video->markReplaceInProgress();
    auto videoNew = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("video-new"));

    audioSelectorRaw->onGetNext = [video, videoNew]() {
        check(video->setTransport(videoNew), "reentrant sibling replacement during transport-reject failed");
        video->markReplacePlanned();
    };

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReject,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-rejected") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-stale-reject") } }));

    check(ok, "reentrant stale sibling made transport-reject reject the whole batch");
    check(audio->transport().data() == audioFallback.data() && audio->replacePlanned(),
          "valid first transport-reject member did not select its fallback");
    check(video->transport().data() == videoNew.data(), "stale transport-reject changed the newer sibling transport");
    check(video->replacePlanned(), "stale transport-reject completed the newer sibling transaction");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testRejectSkipsReentrantStaleSibling(client);

    qInfo("Transport-reject reentrancy regression passed");
    return 0;
}
