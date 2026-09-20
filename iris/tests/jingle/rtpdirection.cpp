// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QtCrypto>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>

#define private public
#include <iris/jingle-rtp.h>
#undef private

using namespace XMPP;
namespace J = XMPP::Jingle;
namespace R = XMPP::Jingle::RTP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static J::Origin peerRole(J::Origin role)
{
    return role == J::Origin::Initiator ? J::Origin::Responder : J::Origin::Initiator;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    auto             manager = client.jingleManager()->rtpManager();

    for (const auto localRole : { J::Origin::Initiator, J::Origin::Responder }) {
        J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), localRole);
        auto       pad
            = QSharedPointer<R::Pad>::create(manager, &session, std::shared_ptr<R::MediaProvider>(), QStringList());

        for (const auto senders : { J::Origin::None, J::Origin::Both, J::Origin::Initiator, J::Origin::Responder }) {
            R::Application application(pad, QStringLiteral("audio"), J::Origin::Initiator, senders);
            const bool     expectedSend    = senders == J::Origin::Both || senders == localRole;
            const bool     expectedReceive = senders == J::Origin::Both || senders == peerRole(localRole);

            check(application.allowsRtp(true) == expectedSend, "RTP outgoing sender-direction matrix mismatch");
            check(application.allowsRtp(false) == expectedReceive, "RTP incoming sender-direction matrix mismatch");
        }
    }

    qInfo("RTP sender direction matrix passed");
}
