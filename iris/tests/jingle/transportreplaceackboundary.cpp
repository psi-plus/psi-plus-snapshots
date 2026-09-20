// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_transportreplace_correctness_main
#include "transportreplacecorrectness.cpp"
#undef main

class CallbackTransport : public TestTransport {
public:
    using TestTransport::TestTransport;
    int                            calls = 0;
    std::function<void()>          onAck;
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool) override
    {
        auto el = pad()->doc()->createElementNS(pad()->ns(), QStringLiteral("transport"));
        setHasUpdates(false);
        return { el, [self = QPointer<CallbackTransport>(this)](Task *) {
                    if (!self)
                        return;
                    ++self->calls;
                    auto callback = self->onAck;
                    if (callback)
                        callback();
                } };
    }
};

class SuccessorSelector : public TestSelector {
public:
    QSharedPointer<J::Transport> next;
    bool                         hasMoreTransports() const override { return bool(next); }
    QSharedPointer<J::Transport> getNextTransport() override { return std::exchange(next, {}); }
};

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager manager;
    Client               client;
    client.jingleManager()->registerTransport(&manager);
    Result success(client.rootTask(), true), failure(client.rootTask(), false);
    for (bool ok : { true, false }) {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       local = QSharedPointer<CallbackTransport>::create(
            session.transportPadFactory(TestTransportManager::namespaceUri()), J::Origin::Initiator, "local");
        local->forceState(J::State::ApprovedToSend);
        local->setHasUpdates(true);
        auto app = addApplication(session, local, std::make_unique<TestSelector>());
        app->markReplacePlanned();
        check(app->evaluateOutgoingUpdate().action == J::Action::TransportReplace, "missing replace proposal");
        const auto update   = app->takeOutgoingUpdate();
        const auto callback = std::get<1>(update);
        local->onAck        = [&] {
            check(!app->transportReplaceAwaitingAck(), "completed IQ still collides inside transport ACK callback");
            callback(&success); // duplicate delivery reentered before the owner callback has returned
            check(local->calls == 1, "reentrant duplicate completion reentered transport code");
            QDomDocument doc;
            check(session.updateFromXml(J::Action::TransportReplace, makeReplace(doc, { "remote" })),
                         "post-completion remote replacement received a stale tie-break");
        };
        callback(ok ? &success : &failure);
        check(app->transport() != local && app->replaceInProgress(), "old ACK displaced the nested remote proposal");
        callback(&success);
        callback(&failure);
        check(local->calls == 1, "duplicate completion invoked transport callback again");
    }
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       local = QSharedPointer<CallbackTransport>::create(
            session.transportPadFactory(TestTransportManager::namespaceUri()), J::Origin::Initiator, "local");
        local->forceState(J::State::ApprovedToSend);
        local->setHasUpdates(true);
        auto app = addApplication(session, local, std::make_unique<TestSelector>());
        app->markReplacePlanned();
        app->evaluateOutgoingUpdate();
        const auto callback = std::get<1>(app->takeOutgoingUpdate());
        auto       remote   = makeTransport(session, J::Origin::Responder, J::State::Pending, "new-remote");
        check(app->setTransport(remote), "fixture could not supersede pending proposal");
        callback(&success);
        check(local->calls == 0 && app->transport() == remote && app->replaceInProgress(),
              "stale completion called the replaced transport or modified its successor");
    }
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       local = QSharedPointer<CallbackTransport>::create(
            session.transportPadFactory(TestTransportManager::namespaceUri()), J::Origin::Initiator, "local");
        local->forceState(J::State::ApprovedToSend);
        local->setHasUpdates(true);
        auto app = addApplication(session, local, std::make_unique<TestSelector>());
        app->markReplacePlanned();
        app->evaluateOutgoingUpdate();
        const auto callback = std::get<1>(app->takeOutgoingUpdate());
        app->setState(J::State::Finished);
        callback(&success);
        check(local->calls == 0, "completion ran transport code after content termination");
        delete app;
        callback(&success);
        check(local->calls == 0, "completion ran transport code after Application destruction");
    }
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       local = QSharedPointer<CallbackTransport>::create(
            session.transportPadFactory(TestTransportManager::namespaceUri()), J::Origin::Initiator, "local");
        local->forceState(J::State::ApprovedToSend);
        local->setHasUpdates(true);
        auto app = addApplication(session, local, std::make_unique<TestSelector>());
        app->markReplacePlanned();
        app->evaluateOutgoingUpdate();
        const auto                callback = std::get<1>(app->takeOutgoingUpdate());
        QPointer<TestApplication> guard(app);
        local->onAck = [app] { delete app; };
        callback(&failure);
        callback(&success);
        check(!guard && local->calls == 1, "reentrant destruction did not retire completion");
    }
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       local = QSharedPointer<CallbackTransport>::create(
            session.transportPadFactory(TestTransportManager::namespaceUri()), J::Origin::Initiator, "local");
        local->forceState(J::State::ApprovedToSend);
        local->setHasUpdates(true);
        auto app = addApplication(session, local, std::make_unique<TestSelector>());
        app->markReplacePlanned();
        app->evaluateOutgoingUpdate();
        const auto callback = std::get<1>(app->takeOutgoingUpdate());
        local->onAck
            = [app, local] { check(app->setTransport(local), "fixture could not reselect the same transport"); };
        callback(&failure);
        check(app->state() == J::State::Connecting && app->transport() == local,
              "failed old IQ fell back over a new generation of the same transport");
        local->onAck = {}; // release the test callback's shared reference
    }
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       local = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, "local", true);
        local->setHasUpdates(true);
        auto next = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, "successor");
        next->setHasUpdates(true);
        auto selector  = std::make_unique<SuccessorSelector>();
        selector->next = next;
        auto app       = addApplication(session, local, std::move(selector));
        app->markReplacePlanned();
        app->evaluateOutgoingUpdate();
        const auto callback = std::get<1>(app->takeOutgoingUpdate());
        check(local->state() == J::State::Unacked, "fixture did not model an IBB-like transport");
        callback(&failure);
        check(app->transport() == next && !app->transportReplaceAwaitingAck(),
              "unsent successor inherited the failed predecessor's IQ lifetime");
        check(app->evaluateOutgoingUpdate().action == J::Action::TransportReplace,
              "fallback successor was not scheduled as a fresh replacement");
    }
    return 0;
}
