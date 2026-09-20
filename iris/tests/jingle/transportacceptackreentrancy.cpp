// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

#include <functional>

class ReentrantAckTransport : public TestTransport {
public:
    using TestTransport::TestTransport;

    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement) override
    {
        Q_UNUSED(ensureTransportElement);
        auto el = pad()->doc()->createElementNS(pad()->ns(), QStringLiteral("transport"));
        el.setAttribute(QStringLiteral("id"), id());
        setHasUpdates(false);
        return { el, [self = QPointer<ReentrantAckTransport>(this)](Task *task) {
                    if (!self)
                        return;
                    if (task && task->success())
                        self->forceState(J::State::Pending);
                    auto callback = std::move(self->onAck);
                    if (callback)
                        callback();
                } };
    }

    std::function<void()> onAck;
};

struct AcceptFixture {
    explicit AcceptFixture(Client &client, const QString &peer = QStringLiteral("peer@example.test/device")) :
        session(client.jingleManager(), Jid(peer), J::Origin::Initiator),
        pad(session.transportPadFactory(TestTransportManager::namespaceUri())),
        remote(QSharedPointer<ReentrantAckTransport>::create(pad, J::Origin::Responder, QStringLiteral("remote"))),
        success(client.rootTask(), true)
    {
        check(bool(pad), "test transport pad missing");
        remote->forceState(J::State::ApprovedToSend);
        remote->setHasUpdates(true);
        app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, remote,
                             std::make_unique<TestSelector>(), nullptr, J::State::Connecting);
        app->markReplaceInProgress();
    }

    J::OutgoingUpdateCB takeAck()
    {
        check(app->evaluateOutgoingUpdate().action == J::Action::TransportAccept,
              "remote replacement did not evaluate to transport-accept");
        auto update = app->takeOutgoingUpdate();
        auto ack    = std::get<1>(update);
        check(bool(ack), "transport-accept had no ACK callback");
        return ack;
    }

    J::Session                           session;
    J::TransportManagerPad::Ptr          pad;
    QSharedPointer<ReentrantAckTransport> remote;
    TestApplication                     *app = nullptr;
    Result                               success;
};

static void testReentrantNewTransport(Client &client)
{
    AcceptFixture f(client, QStringLiteral("new-transport@example.test/device"));
    auto newerLocal
        = makeTransport(f.session, J::Origin::Initiator, J::State::Created, QStringLiteral("new-local"));

    f.remote->onAck = [app = f.app, newerLocal]() {
        check(app->setTransport(newerLocal), "reentrant transport-accept ACK could not install newer local transport");
        check(app->replacePlanned(), "newer local transport was not marked as a planned replacement");
    };

    auto ack = f.takeAck();
    ack(&f.success);

    check(f.app->transport().data() == newerLocal.data(),
          "old transport-accept ACK replaced the newer reentrant transport");
    check(f.app->replacePlanned(), "old transport-accept ACK cleared the newer replacement transaction");
    check(newerLocal->starts() == 0, "old transport-accept ACK started the newer replacement transport");
}

static void testDuplicateCompletionIsInert(Client &client)
{
    AcceptFixture f(client, QStringLiteral("duplicate@example.test/device"));
    auto          ack = f.takeAck();

    ack(&f.success);
    check(f.app->replaceIdle() && f.remote->state() == J::State::Active && f.remote->starts() == 1,
          "first transport-accept ACK did not complete the replacement");

    ack(&f.success);
    check(f.app->replaceIdle() && f.remote->state() == J::State::Active && f.remote->starts() == 1,
          "duplicate transport-accept ACK mutated an already completed replacement");
}

static void testStaleDifferentTransportCompletionIsInert(Client &client)
{
    AcceptFixture f(client, QStringLiteral("stale-transport@example.test/device"));
    auto          ack = f.takeAck();
    auto newerLocal
        = makeTransport(f.session, J::Origin::Initiator, J::State::Created, QStringLiteral("new-local"));

    check(f.app->setTransport(newerLocal), "could not install successor before stale transport-accept ACK");
    const auto oldState = f.remote->state();
    ack(&f.success);

    check(f.remote->state() == oldState, "stale transport-accept ACK still invoked the old transport callback");
    check(f.app->transport().data() == newerLocal.data() && f.app->replacePlanned() && newerLocal->starts() == 0,
          "stale transport-accept ACK changed the successor transaction");
}

static void testSameObjectNewGenerationIsDistinct(Client &client)
{
    AcceptFixture f(client, QStringLiteral("same-object@example.test/device"));
    auto          ack = f.takeAck();

    check(f.app->setTransport(f.remote), "same-object reselection was rejected");
    check(f.app->replaceInProgress(), "same-object reselection did not create a fresh replacement generation");
    const auto state = f.remote->state();
    ack(&f.success);

    check(f.app->transport().data() == f.remote.data() && f.app->replaceInProgress(),
          "old ACK completed a same-pointer newer replacement generation");
    check(f.remote->state() == state && f.remote->starts() == 0,
          "old ACK invoked or started a same-pointer newer replacement generation");
}

static void testFinishingApplicationRejectsLateCompletion(Client &client)
{
    AcceptFixture f(client, QStringLiteral("finishing@example.test/device"));
    auto          ack   = f.takeAck();
    const auto    state = f.remote->state();

    f.app->setState(J::State::Finishing);
    ack(&f.success);

    check(f.remote->state() == state && f.remote->starts() == 0,
          "late transport-accept ACK mutated transport after application termination");
}

static void testDeletedApplicationRejectsLateCompletion(Client &client)
{
    AcceptFixture f(client, QStringLiteral("deleted@example.test/device"));
    auto          ack = f.takeAck();

    QDomDocument doc;
    auto         jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
    J::ContentBase::setCreatorAttr(content, J::Origin::Initiator);
    content.setAttribute(QStringLiteral("name"), QStringLiteral("audio"));
    jingle.appendChild(content);

    check(f.session.updateFromXml(J::Action::ContentRemove, jingle), "could not remove application before late ACK");
    check(!f.session.content(QStringLiteral("audio"), J::Origin::Initiator),
          "content-remove did not delete the application");

    // Content removal is allowed to finish/change the retained transport itself.
    // Snapshot the post-removal state: only mutations after this point belong to
    // the stale ACK under test.
    const auto state  = f.remote->state();
    const auto starts = f.remote->starts();

    ack(&f.success);
    check(f.remote->state() == state && f.remote->starts() == starts,
          "late transport-accept ACK mutated retained transport after application deletion");
}

static void testAttemptRetiredBeforeNestedAccept(Client &client)
{
    AcceptFixture f(client, QStringLiteral("nested-accept@example.test/device"));
    bool          nestedAccepted = true;

    f.remote->onAck = [&]() {
        QDomDocument doc;
        auto transport = doc.createElementNS(TestTransportManager::namespaceUri(), QStringLiteral("transport"));
        transport.setAttribute(QStringLiteral("id"), QStringLiteral("nested"));
        nestedAccepted = f.app->incomingTransportAccept(transport);
    };

    auto ack = f.takeAck();
    ack(&f.success);

    check(!nestedAccepted, "completed transport-accept remained visible to a nested accept");
    check(f.app->replaceIdle() && f.remote->id() == QLatin1String("remote")
              && f.remote->state() == J::State::Active && f.remote->starts() == 1,
          "nested accept mutated a retired transport-accept attempt");
}

int main(int argc, char **argv)
{
    QCoreApplication     app(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testReentrantNewTransport(client);
    testDuplicateCompletionIsInert(client);
    testStaleDifferentTransportCompletionIsInert(client);
    testSameObjectNewGenerationIsDistinct(client);
    testFinishingApplicationRejectsLateCompletion(client);
    testDeletedApplicationRejectsLateCompletion(client);
    testAttemptRetiredBeforeNestedAccept(client);

    qInfo("Transport-accept ACK lifetime regressions passed");
    return 0;
}
