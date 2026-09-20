// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-ibb.h"
#include <QCoreApplication>
#include <QDebug>
#include <QtCrypto>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>
using namespace XMPP;
namespace J = XMPP::Jingle;
static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
class Result : public Task {
public:
    Result(Task *parent, bool ok) : Task(parent)
    {
        if (ok)
            setSuccess();
        else
            setError(500);
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    J::Session       session(client.jingleManager(), Jid("peer@example.test/device"));
    J::IBB::Manager  manager;
    manager.setJingleManager(client.jingleManager());
    auto pad       = J::TransportManagerPad::Ptr(manager.pad(&session));
    auto transport = manager.newTransport(pad, J::Origin::Initiator);
    check(bool(transport->addChannel(J::TransportFeature::DataOriented, "test")), "IBB channel missing");
    transport->prepare();
    auto [xml, ack] = transport->takeOutgoingUpdate(false);
    check(!xml.isNull() && bool(ack) && transport->state() == J::State::Unacked, "IBB offer not awaiting ACK");
    Result failure(client.rootTask(), false), success(client.rootTask(), true);
    ack(&failure);
    check(transport->state() == J::State::Unacked, "failed IQ acknowledged IBB offer");
    ack(nullptr);
    check(transport->state() == J::State::Unacked, "null IQ acknowledged IBB offer");
    ack(&success);
    check(transport->state() == J::State::Pending, "successful IQ did not acknowledge IBB offer");
    transport.reset();
    ack(&success); // Callback may outlive its transport.
    auto reentrant = manager.newTransport(pad, J::Origin::Initiator);
    check(bool(reentrant->addChannel(J::TransportFeature::DataOriented, "reentrant")), "second IBB channel missing");
    reentrant->prepare();
    auto update = reentrant->takeOutgoingUpdate(false);
    QObject::connect(reentrant.data(), &J::Transport::stateChanged, &app, [&]() {
        if (reentrant && reentrant->state() == J::State::Pending)
            reentrant.reset();
    });
    std::get<1>(update)(&success);
    check(!reentrant, "IBB acknowledgement did not exercise reentrant destruction");
    qInfo("Transport acknowledgement regressions passed");
}
