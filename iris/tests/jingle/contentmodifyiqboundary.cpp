// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main

// This regression deliberately drives the Application ACK callback directly: it
// isolates the semantic boundary between an already completed IQ transaction and
// reentrant application notifications executed from that completion.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    Result           success(client.rootTask(), true);

    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       content = new TestApplication(&session, J::Origin::Both, QStringLiteral("audio"), J::Origin::Initiator);
    session.addContent(content);
    content->activate();

    check(content->requestSenders(J::Origin::Responder), "direction request rejected");
    check(content->evaluateOutgoingUpdate().action == J::Action::ContentModify,
          "direction request was not evaluated as content-modify");
    auto       update        = content->takeOutgoingUpdate();
    const auto remotePayload = payload(update);
    const auto transaction   = startContentModify(session, { &update });

    bool ackNotificationEntered = false;
    bool staleTieBreak          = false;
    QObject::connect(content, &J::Application::sendersChanged, &app, [&](J::Origin senders) {
        if (senders != J::Origin::Responder)
            return;
        ackNotificationEntered = true;

        // The IQ result has already completed the outgoing Jingle action before
        // Application callbacks are entered. A peer content-modify delivered
        // reentrantly from this notification is a later action, not a crossed one.
        staleTieBreak = session.tieBreaker()->resolveIncoming(J::Action::ContentModify, remotePayload).solution
            != J::TieBreaker::Solution::Continue;
    });

    session.tieBreaker()->outgoingFinished(transaction, std::nullopt);
    acknowledge(update, &success);
    session.tieBreaker()->outgoingCallbacksFinished(transaction);

    check(ackNotificationEntered, "successful ACK did not notify the negotiated direction");
    check(!staleTieBreak, "completed content-modify IQ remained collision-active inside its ACK callback");
    check(session.tieBreaker()->resolveIncoming(J::Action::ContentModify, remotePayload).solution
              == J::TieBreaker::Solution::Continue,
          "completed content-modify left stale collision state after its callback");

    qInfo("Content-modify IQ lifetime regression passed");
    return 0;
}
