// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QThread>
#include <QtCrypto>

#include <iris/jingle-ice.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static void exerciseProfile(Client &client, const QString &transportNs)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       base = session.newOutgoingTransport(transportNs);
    auto       ice  = qSharedPointerDynamicCast<J::ICE::Transport>(base);
    check(bool(ice), "selected ICE namespace did not create an ICE transport");
    check(ice->state() == J::State::Created, "new ICE transport did not start in Created state");

    ice->prepare();
    check(ice->state() == J::State::ApprovedToSend, "preparing a local ICE transport did not enter ApprovedToSend");

    QElapsedTimer deadline;
    deadline.start();
    while (!ice->hasUpdates() && ice->state() < J::State::Finishing && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }

    check(ice->state() < J::State::Finishing, "ICE gathering failed before producing a signaling update");
    check(ice->hasUpdates(), "ICE transport did not produce an outgoing signaling update");

    auto [xml, ack] = ice->takeOutgoingUpdate(false);
    check(!xml.isNull() && xml.namespaceURI() == transportNs, "ICE serialized the wrong transport namespace");

    // This is the important invariant for transport-replace arbitration. ICE
    // signaling does not encode the lifetime of the containing Jingle IQ in
    // Transport::State. Application::PendingTransportReplace::NeedAck (and,
    // eventually, the Session transaction object) owns that lifetime instead.
    check(ice->state() == J::State::ApprovedToSend,
          "serializing an ICE update incorrectly changed Transport::State to Unacked");

    ice->stop();
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCA::Initializer qca;
    TcpPortReserver  reserver;
    Client           client;
    client.setTcpPortReserver(&reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);

    exerciseProfile(client, J::ICE::NS);
    exerciseProfile(client, J::ICE::NS_ICE_UDP);

    qInfo("Real ICE replacement signaling state regressions passed");
}
