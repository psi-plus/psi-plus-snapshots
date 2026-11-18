// SPDX-License-Identifier: LGPL-2.1-or-later

#include <iris/ice176.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>
#include <QtCrypto>

#include <functional>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static bool waitUntil(const std::function<bool()> &condition, int timeoutMs)
{
    if (condition())
        return true;
    QEventLoop loop;
    QTimer poll;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (condition())
            loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(5);
    deadline.start(timeoutMs);
    loop.exec();
    return condition();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    // A two-socket UDP forwarder models two independently mapped endpoints:
    // the relay address advertised to the host-only initiator and the NAT
    // address observed by the responder. No public candidate is signaled by
    // the initiator, and the responder initially knows no remote candidates.
    // This deliberately tests ICE/prflx, NOT actual TURN permissions.
    QUdpSocket relayFront;
    QUdpSocket natBack;
    check(relayFront.bind(QHostAddress::LocalHost), "Cannot bind simulated relay");
    check(natBack.bind(QHostAddress::LocalHost), "Cannot bind simulated NAT mapping");

    Ice176 sender;
    Ice176 receiver;
    const QList<Ice176::LocalAddress> loopback { Ice176::LocalAddress { QHostAddress::LocalHost } };
    for (Ice176 *ice : { &sender, &receiver }) {
        ice->setLocalAddresses(loopback);
        ice->setComponentCount(1);
        ice->setLocalFeatures(Ice176::Features(Ice176::Trickle | Ice176::NotNominatedData));
    }

    QList<Ice176::Candidate> senderCandidates;
    QList<Ice176::Candidate> receiverCandidates;
    bool senderStarted = false;
    bool receiverStarted = false;
    QObject::connect(&sender, &Ice176::localCandidatesReady,
                     [&](const auto &list) { senderCandidates += list; });
    QObject::connect(&receiver, &Ice176::localCandidatesReady,
                     [&](const auto &list) { receiverCandidates += list; });
    QObject::connect(&sender, &Ice176::started, [&]() { senderStarted = true; });
    QObject::connect(&receiver, &Ice176::started, [&]() { receiverStarted = true; });

    sender.start(Ice176::Initiator);
    receiver.start(Ice176::Responder);
    check(waitUntil([&]() {
              return senderStarted && receiverStarted && !senderCandidates.isEmpty() && !receiverCandidates.isEmpty();
          }, 3000),
          "ICE did not gather the host candidates");

    // The sender advertises host candidates only, never its translated port.
    check(senderCandidates.size() == 1 && senderCandidates.first().type == QLatin1String("host"),
          "Sender unexpectedly advertised a public candidate");
    const auto realReceiver = receiverCandidates.first();

    QHostAddress senderHost;
    quint16 senderPort = 0;
    int frontForwarded = 0;
    int backForwarded = 0;

    QObject::connect(&relayFront, &QUdpSocket::readyRead, [&]() {
        while (relayFront.hasPendingDatagrams()) {
            const auto datagram = relayFront.receiveDatagram();
            senderHost = datagram.senderAddress();
            senderPort = datagram.senderPort();
            const auto size = natBack.writeDatagram(datagram.data(), realReceiver.ip, realReceiver.port);
            check(size == datagram.data().size(), "Simulated NAT failed to forward outgoing datagram");
            ++frontForwarded;
        }
    });
    QObject::connect(&natBack, &QUdpSocket::readyRead, [&]() {
        while (natBack.hasPendingDatagrams()) {
            const auto datagram = natBack.receiveDatagram();
            check(senderPort != 0, "Simulated NAT has no initiator endpoint");
            const auto size = relayFront.writeDatagram(datagram.data(), senderHost, senderPort);
            check(size == datagram.data().size(), "Simulated NAT failed to forward incoming datagram");
            ++backForwarded;
        }
    });

    // The only candidate available to the initiator appears to be a relay.
    // Its advertised socket is the front of our forwarder, not the real peer.
    auto relayCandidate = realReceiver;
    relayCandidate.ip = QHostAddress::LocalHost;
    relayCandidate.port = relayFront.localPort();
    relayCandidate.type = QStringLiteral("relay");
    relayCandidate.priority = 16777215;
    relayCandidate.foundation = QStringLiteral("simulated-relay");
    relayCandidate.rel_addr = QHostAddress();
    relayCandidate.rel_port = -1;

    sender.setRemoteCredentials(receiver.localUfrag(), receiver.localPassword());
    receiver.setRemoteCredentials(sender.localUfrag(), sender.localPassword());
    sender.addRemoteCandidates({ relayCandidate });
    // The receiver has no signaled remote candidates: it must create a remote
    // prflx upon the authenticated request from the translated sender.
    sender.setRemoteGatheringComplete();
    receiver.setRemoteGatheringComplete();

    int senderActivity = 0;
    int receiverActivity = 0;
    bool senderSelected = false;
    bool receiverSelected = false;
    bool receivedData = false;
    bool sentData = false;
    const QByteArray payload("host-only-prflx-over-relay");

    QObject::connect(&sender, &Ice176::peerActivity, [&]() { ++senderActivity; });
    QObject::connect(&receiver, &Ice176::peerActivity, [&]() { ++receiverActivity; });
    QObject::connect(&sender, &Ice176::componentReady, [&](int id) {
        check(id == 0, "Invalid sender component");
        senderSelected = true;
    });
    QObject::connect(&receiver, &Ice176::componentReady, [&](int id) {
        check(id == 0, "Invalid receiver component");
        receiverSelected = true;
    });
    QObject::connect(&receiver, &Ice176::readyRead, [&](int id) {
        while (receiver.hasPendingDatagrams(id)) {
            if (receiver.readDatagram(id) == payload)
                receivedData = true;
        }
    });

    sender.startChecks();
    receiver.startChecks();

    check(waitUntil([&]() {
              if (!sentData && senderSelected && receiverSelected) {
                  sentData = true;
                  sender.writeDatagram(0, payload);
              }
              return receivedData;
          }, 8000),
          "Host-only initiator did not reach receiver through the translated relay route");
    check(senderActivity > 0 && receiverActivity > 0, "ICE peer checks did not pass on both sides");
    check(frontForwarded > 0 && backForwarded > 0, "UDP forwarder did not exercise both directions");
    check(senderSelected && receiverSelected, "ICE did not select nominated pairs");
    const auto localSelection = sender.selectedCandidates();
    check(localSelection.size() == 1 && localSelection.first().port == natBack.localPort(),
          "ICE did not discover and select the initiator's local peer-reflexive address");

    qInfo("Host-only/prflx ICE relay-path regression passed");
    return 0;
}
