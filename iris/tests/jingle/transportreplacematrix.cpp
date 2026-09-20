// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the legacy transport-replace fixture in a second executable so the
// matrix below exercises exactly the same fake transports/selectors as the
// historical-behavior suite without duplicating several hundred lines of test
// plumbing. Rename its standalone entry point while including it here.
#include <functional>

#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

class ReentrantCanReplaceSelector : public TestSelector {
public:
    bool canReplace(QSharedPointer<J::Transport> oldTransport, QSharedPointer<J::Transport> newTransport) override
    {
        const bool result   = TestSelector::canReplace(std::move(oldTransport), std::move(newTransport));
        auto       callback = std::move(onCanReplace);
        if (callback)
            callback();
        return result;
    }

    std::function<void()> onCanReplace;
};

class ReentrantUpdateTransport : public TestTransport {
public:
    using TestTransport::TestTransport;

    bool commitPreparedUpdate(PreparedUpdatePtr update) override
    {
        const bool ok       = TestTransport::commitPreparedUpdate(std::move(update));
        auto       callback = std::move(onUpdate);
        if (callback)
            callback();
        return ok;
    }

    std::function<void()> onUpdate;
};

static QDomElement makeUnqualifiedTransportReplace(QDomDocument &doc, const QString &name, J::Origin creator)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
    J::ContentBase::setCreatorAttr(content, creator);
    content.setAttribute(QStringLiteral("name"), name);
    content.appendChild(doc.createElement(QStringLiteral("transport")));
    jingle.appendChild(content);
    return jingle;
}

static void testUnqualifiedTransportIsMalformed(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("plain@example.test/device")), J::Origin::Initiator);
    auto       old            = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
    auto       selector       = std::make_unique<TestSelector>();
    TestSelector *selectorRaw = nullptr;
    auto          app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                                       &selectorRaw);

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeUnqualifiedTransportReplace(doc, QStringLiteral("audio"), J::Origin::Initiator));

    check(!ok, "transport-replace with an unqualified transport element was accepted");
    check(app->transport().data() == old.data(), "malformed unqualified transport changed current transport");
    check(selectorRaw->canReplaceCalls == 0 && selectorRaw->replaceCalls == 0,
          "malformed unqualified transport reached the selector");
}

static void testSelectorReplaceFailureKeepsCurrentTransport(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("replace-false@example.test/device")),
                       J::Origin::Initiator);
    auto       old            = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
    auto       selector       = std::make_unique<TestSelector>();
    selector->allowReplace    = false;
    TestSelector *selectorRaw = nullptr;
    auto          app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                                       &selectorRaw);

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Initiator,
                                                TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));

    check(ok, "selector replace failure rejected the whole transport-replace action");
    check(app->transport().data() == old.data(), "selector replace failure changed current transport");
    check(app->replaceIdle(), "selector replace failure changed replacement transaction state");
    check(selectorRaw->canReplaceCalls == 1 && selectorRaw->replaceCalls == 1,
          "selector replace failure did not follow canReplace -> replace path");
}

static void testMixedBatchAcceptsSupportedAndRejectsUnsupported(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("mixed@example.test/device")), J::Origin::Initiator);

    auto audioOld      = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-old"));
    auto audioSelector = std::make_unique<TestSelector>();
    TestSelector *audioSelectorRaw = nullptr;
    auto          audio            = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                                    std::move(audioSelector), &audioSelectorRaw);

    auto videoOld      = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-old"));
    auto videoSelector = std::make_unique<TestSelector>();
    videoSelector->allowCanReplace = false;
    TestSelector *videoSelectorRaw = nullptr;
    auto          video            = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                                    std::move(videoSelector), &videoSelectorRaw);

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-remote") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-remote") } }));

    check(ok && !isTieBreak(session), "mixed supported/unsupported transport-replace batch was rejected wholesale");
    check(audio->transport().data() != audioOld.data() && audio->replaceInProgress(),
          "supported member of mixed batch was not installed");
    check(static_cast<TestTransport *>(audio->transport().data())->id() == QLatin1String("audio-remote"),
          "supported member of mixed batch installed the wrong transport");
    check(video->transport().data() == videoOld.data() && video->replaceIdle(),
          "unsupported member of mixed batch changed transport state");
    check(audioSelectorRaw->canReplaceCalls == 1 && audioSelectorRaw->replaceCalls == 1,
          "supported member of mixed batch did not traverse selector");
    check(videoSelectorRaw->canReplaceCalls == 1 && videoSelectorRaw->replaceCalls == 0,
          "unsupported member of mixed batch reached selector replace");
}

static void testPlannedLocalReplaceDoesNotTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("planned@example.test/device")),
                       J::Origin::Initiator);
    auto       local
        = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("local-planned"));
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, local,
                              std::make_unique<TestSelector>());
    app->markReplacePlanned();

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Initiator,
                                                TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));

    check(ok && !isTieBreak(session), "unsent Planned replacement incorrectly won a tie-break");
    check(app->transport().data() != local.data() && app->replaceInProgress(),
          "peer replacement did not supersede an unsent Planned local replacement");
}

static void testAcknowledgedLocalReplaceDoesNotTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("in-progress@example.test/device")),
                       J::Origin::Initiator);
    auto       local = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local-acked"));
    auto       app   = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, local,
                                      std::make_unique<TestSelector>());
    app->markReplaceInProgress();

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Initiator,
                                                TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));

    check(ok && !isTieBreak(session), "already-acknowledged local replacement incorrectly caused tie-break");
    check(app->transport().data() != local.data() && app->replaceInProgress(),
          "peer replacement was not installed after local transport-replace ACK");
}

static void testResponderNeedAckStillYieldsToInitiator(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("responder-race@example.test/device")),
                       J::Origin::Responder);
    auto       local
        = makeTransport(session, J::Origin::Responder, J::State::ApprovedToSend, QStringLiteral("responder-local"));
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Responder, local,
                              std::make_unique<TestSelector>());
    app->markReplaceAwaitingAck();

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Responder, TestTransportManager::namespaceUri(),
                          QStringLiteral("initiator-winner") } }));

    check(ok && !isTieBreak(session), "responder rejected initiator winner while its own replace IQ was in flight");
    check(app->transport().data() != local.data() && app->transport()->creator() == J::Origin::Initiator,
          "responder did not install initiator winner during crossed transport-replace");
    check(app->replaceInProgress(), "responder winner install did not enter InProgress");
}

static void testTieBreakCarriesConflictCondition(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("tie-break-wire@example.test/device")),
                       J::Origin::Initiator);
    auto       local = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("local"));
    auto       app   = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, local,
                                      std::make_unique<TestSelector>());
    app->markReplaceAwaitingAck();

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Initiator,
                                                TestTransportManager::namespaceUri(), QStringLiteral("crossed") } }));

    const auto error = session.lastError();
    check(!ok && error, "crossed transport-replace did not return an error");
    check(error->condition == Stanza::Error::ErrorCond::Conflict,
          "transport-replace tie-break did not use the stanza conflict condition");
    check(J::ErrorUtil::jingleCondition(*error) == J::ErrorUtil::TieBreak,
          "transport-replace conflict did not carry the Jingle tie-break condition");
}

static void testReentrantSiblingMutationInvalidatesValidatedCandidate(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("reentrant@example.test/device")),
                       J::Origin::Initiator);

    auto audioOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-old"));
    auto audio    = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                   std::make_unique<TestSelector>());
    auto audioNewLocal
        = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("audio-new-local"));

    auto videoOld      = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-old"));
    auto videoSelector = std::make_unique<ReentrantCanReplaceSelector>();
    videoSelector->onCanReplace = [audio, audioNewLocal]() {
        check(audio->setTransport(audioNewLocal), "reentrant local transport change failed");
    };
    auto video
        = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld, std::move(videoSelector));

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-remote-stale") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-remote") } }));

    check(ok && !isTieBreak(session), "reentrant transport-replace batch was rejected");
    check(audio->transport().data() == audioNewLocal.data(),
          "second pass overwrote a newer reentrant local transport with a stale validated candidate");
    check(audio->replacePlanned(), "newer reentrant local transport lost its Planned replacement state");
    check(video->transport().data() != videoOld.data() && video->replaceInProgress(),
          "unrelated sibling stopped progressing after reentrant local mutation");
}

static void testTransportAcceptSkipsReentrantStaleSibling(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("accept-reentrant@example.test/device")),
                       J::Origin::Initiator);

    auto audioPad = session.transportPadFactory(TestTransportManager::namespaceUri());
    check(bool(audioPad), "test transport pad missing for reentrant transport-accept");
    auto audioTransport = QSharedPointer<ReentrantUpdateTransport>::create(audioPad, J::Origin::Initiator,
                                                                           QStringLiteral("audio-local"));
    audioTransport->forceState(J::State::Pending);
    auto audio = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioTransport,
                                std::make_unique<TestSelector>());
    audio->markReplaceInProgress();

    auto videoOld = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-old"));
    auto video    = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                   std::make_unique<TestSelector>());
    video->markReplaceInProgress();
    auto videoNewLocal
        = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("video-new-local"));

    audioTransport->onUpdate = [video, videoNewLocal]() {
        check(video->setTransport(videoNewLocal), "reentrant sibling replacement during transport-accept failed");
    };

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportAccept,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-accepted") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-stale-accept") } }));

    check(ok, "reentrant stale sibling made transport-accept reject the whole batch");
    check(audio->replaceIdle() && audioTransport->starts() == 1,
          "valid first transport-accept member did not complete normally");
    check(video->transport().data() == videoNewLocal.data(),
          "stale transport-accept overwrote the newer sibling transport");
    check(video->replacePlanned(), "newer sibling transport lost its Planned replacement state");
}

static void testReplacementValidationIdentity(Client &client)
{
    for (bool destroy : { false, true }) {
        J::Session session(client.jingleManager(), Jid("validation@example.test/device"), J::Origin::Initiator);
        auto       old      = makeTransport(session, J::Origin::Initiator, J::State::Pending, "old");
        auto       selector = std::make_unique<ReentrantCanReplaceSelector>();
        auto       raw      = selector.get();
        auto       app      = addApplication(session, "audio", J::Origin::Initiator, old, std::move(selector));
        QPointer<TestApplication> guard(app);
        raw->onCanReplace = [app, old, destroy] {
            if (destroy)
                delete app;
            else
                check(app->setTransport(old), "same-object reselection failed");
        };
        QDomDocument doc;
        check(
            session.updateFromXml(
                J::Action::TransportReplace,
                makeReplace(doc, { { "audio", J::Origin::Initiator, TestTransportManager::namespaceUri(), "stale" } })),
            "reentrant validation rejected the entire partial batch");
        if (destroy)
            check(!guard, "selector did not delete its owner");
        else
            check(app->transport() == old, "validation overwrote a new generation of the same transport");
    }
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testUnqualifiedTransportIsMalformed(client);
    testSelectorReplaceFailureKeepsCurrentTransport(client);
    testMixedBatchAcceptsSupportedAndRejectsUnsupported(client);
    testPlannedLocalReplaceDoesNotTieBreak(client);
    testAcknowledgedLocalReplaceDoesNotTieBreak(client);
    testResponderNeedAckStillYieldsToInitiator(client);
    testTieBreakCarriesConflictCondition(client);
    testReentrantSiblingMutationInvalidatesValidatedCandidate(client);
    testTransportAcceptSkipsReentrantStaleSibling(client);
    testReplacementValidationIdentity(client);

    qInfo("Transport-replace state matrix regressions passed");
    return 0;
}
