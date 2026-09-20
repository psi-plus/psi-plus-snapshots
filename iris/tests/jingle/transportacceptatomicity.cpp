// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the transport-replace fixture to verify that an invalid member of a
// transport-accept batch cannot partially commit an earlier valid member.
#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

static void testInvalidLaterAcceptIsAtomic(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("accept-atomic@example.test/device")),
                       J::Origin::Initiator);

    auto audio    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-local"));
    auto audioApp = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audio,
                                   std::make_unique<TestSelector>());
    audioApp->markReplaceInProgress();

    auto video    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-local"));
    auto videoApp = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, video,
                                   std::make_unique<TestSelector>());
    // The transport itself still looks Pending, but this replacement has not
    // been sent/acknowledged and therefore cannot be accepted by the peer.
    videoApp->markReplacePlanned();

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportAccept,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-accepted") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-out-of-order") } }));

    check(!ok, "transport-accept batch with an out-of-order member was accepted");
    check(audioApp->transport().data() == audio.data(),
          "invalid later transport-accept changed the earlier member's transport identity");
    check(audioApp->replaceInProgress(),
          "invalid later transport-accept partially completed an earlier replacement transaction");
    check(audio->starts() == 0, "invalid later transport-accept partially started an earlier replacement transport");
    check(videoApp->transport().data() == video.data() && videoApp->replacePlanned(),
          "invalid transport-accept member changed its application state");
}


static void testMalformedLaterPayloadDoesNotPartiallyCommit(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("accept-payload-atomic@example.test/device")),
                       J::Origin::Initiator);

    auto audio    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("audio-local"));
    auto audioApp = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audio,
                                   std::make_unique<TestSelector>());
    audioApp->markReplaceInProgress();

    auto video    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("video-local"));
    auto videoApp = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, video,
                                   std::make_unique<TestSelector>());
    videoApp->markReplaceInProgress();

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportAccept,
        makeReplace(doc,
                    { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                        QStringLiteral("audio-accepted") },
                      { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                        QStringLiteral("video-malformed"), false } }));

    check(!ok, "transport-accept batch with a malformed later payload was accepted");

    // Payload validation must be a prepare-only phase for the whole batch. The
    // first member may not become accepted/started merely because its parser ran
    // before a malformed sibling was discovered.
    check(audio->id() == QLatin1String("audio-local"),
          "malformed later payload left the earlier transport payload applied");
    check(audio->state() == J::State::Pending && audio->starts() == 0,
          "malformed later payload partially started/completed the earlier transport");
    check(audioApp->replaceInProgress(),
          "malformed later payload partially completed the earlier replacement transaction");

    check(video->id() == QLatin1String("video-local") && video->state() == J::State::Pending
              && video->starts() == 0 && videoApp->replaceInProgress(),
          "malformed transport-accept member changed its own replacement state");
}


static void testMalformedLaterTransportInfoDoesNotPartiallyCommit(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("info-payload-atomic@example.test/device")),
                       J::Origin::Initiator);

    auto audio    = makeTransport(session, J::Origin::Responder, J::State::Active, QStringLiteral("audio-current"));
    auto audioApp = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audio,
                                   std::make_unique<TestSelector>());

    auto video    = makeTransport(session, J::Origin::Responder, J::State::Active, QStringLiteral("video-current"));
    auto videoApp = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, video,
                                   std::make_unique<TestSelector>());

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportInfo,
        makeReplace(doc,
                    { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                        QStringLiteral("audio-update") },
                      { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                        QStringLiteral("video-malformed"), false } }));

    check(!ok, "transport-info batch with a malformed later payload was accepted");
    check(audioApp->transport().data() == audio.data() && audio->id() == QLatin1String("audio-current")
              && audio->state() == J::State::Active,
          "malformed later transport-info partially changed the earlier live transport");
    check(videoApp->transport().data() == video.data() && video->id() == QLatin1String("video-current")
              && video->state() == J::State::Active,
          "malformed transport-info member changed its own live transport");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testInvalidLaterAcceptIsAtomic(client);
    testMalformedLaterPayloadDoesNotPartiallyCommit(client);
    testMalformedLaterTransportInfoDoesNotPartiallyCommit(client);

    qInfo("Transport payload atomicity regressions passed");
    return 0;
}
