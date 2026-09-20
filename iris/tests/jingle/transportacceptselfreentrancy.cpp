// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Reuse the transport-replace fixture so this regression exercises the same
// Application replacement state machine as the protocol tests.
#include <functional>

#define main iris_transportreplace_legacy_main
#include "transportreplace.cpp"
#undef main

class SelfReentrantUpdateTransport : public TestTransport {
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

static void testAcceptedTransportCannotCompleteNewerReplacement(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("self-reentrant@example.test/device")),
                       J::Origin::Initiator);

    auto pad = session.transportPadFactory(TestTransportManager::namespaceUri());
    check(bool(pad), "test transport pad missing for self-reentrant transport-accept");

    auto oldTransport
        = QSharedPointer<SelfReentrantUpdateTransport>::create(pad, J::Origin::Initiator, QStringLiteral("old-local"));
    oldTransport->forceState(J::State::Pending);
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, oldTransport,
                              std::make_unique<TestSelector>());
    app->markReplaceInProgress();

    auto newTransport = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("new-local"));
    oldTransport->onUpdate = [app, newTransport]() {
        check(app->setTransport(newTransport), "self-reentrant replacement selection failed");
    };

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportAccept,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("accepted-old") } }));

    check(ok, "valid transport-accept became an error after a newer local replacement was selected");
    check(app->transport().data() == newTransport.data(),
          "transport-accept restored or replaced the newer local transport");
    check(app->replacePlanned(), "transport-accept for the old transport completed the newer replacement transaction");
    check(newTransport->starts() == 0, "transport-accept for the old transport started the newer local transport");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    testAcceptedTransportCannotCompleteNewerReplacement(client);

    qInfo("Transport-accept self-reentrancy regression passed");
    return 0;
}
