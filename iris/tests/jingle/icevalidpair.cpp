// SPDX-License-Identifier: LGPL-2.1-or-later

#include <iris/ice176.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>
#include <QtCrypto>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static void runPair(bool usingProtocolSignalsIce2)
{
    Ice176 first;
    Ice176 second;

    const QList<Ice176::LocalAddress> localAddresses { Ice176::LocalAddress { QHostAddress::LocalHost } };
    for (auto *ice : { &first, &second }) {
        ice->setLocalAddresses(localAddresses);
        ice->setComponentCount(1);
        Ice176::Features features = Ice176::Trickle;
        if (usingProtocolSignalsIce2)
            features |= Ice176::NotNominatedData;
        ice->setLocalFeatures(features);
    }

    QList<Ice176::Candidate> firstCandidates;
    QList<Ice176::Candidate> secondCandidates;
    bool firstStarted = false;
    bool secondStarted = false;

    QObject::connect(&first, &Ice176::localCandidatesReady, [&](const QList<Ice176::Candidate> &candidates) {
        firstCandidates += candidates;
    });
    QObject::connect(&second, &Ice176::localCandidatesReady, [&](const QList<Ice176::Candidate> &candidates) {
        secondCandidates += candidates;
    });
    QObject::connect(&first, &Ice176::started, [&]() { firstStarted = true; });
    QObject::connect(&second, &Ice176::started, [&]() { secondStarted = true; });

    first.start(Ice176::Initiator);
    second.start(Ice176::Responder);

    QEventLoop gatherLoop;
    QTimer gatherPoll;
    QTimer gatherDeadline;
    gatherDeadline.setSingleShot(true);
    QObject::connect(&gatherDeadline, &QTimer::timeout, &gatherLoop, &QEventLoop::quit);
    QObject::connect(&gatherPoll, &QTimer::timeout, &gatherLoop, [&]() {
        if (firstStarted && secondStarted && !firstCandidates.isEmpty() && !secondCandidates.isEmpty())
            gatherLoop.quit();
    });
    gatherPoll.start(5);
    gatherDeadline.start(3000);
    gatherLoop.exec();
    gatherPoll.stop();

    check(firstStarted && secondStarted, "ICE agents did not start");
    check(!firstCandidates.isEmpty() && !secondCandidates.isEmpty(), "ICE agents did not gather host candidates");

    first.setRemoteCredentials(second.localUfrag(), second.localPassword());
    second.setRemoteCredentials(first.localUfrag(), first.localPassword());
    first.addRemoteCandidates(secondCandidates);
    second.addRemoteCandidates(firstCandidates);

    bool firstSelected = false;
    bool secondSelected = false;
    bool firstReady = false;
    bool secondReady = false;
    bool firstReadyBeforeSelection = false;
    bool secondReadyBeforeSelection = false;
    bool earlySent = false;
    bool earlyReceived = false;
    bool selectedSent = false;
    bool selectedReceived = false;

    const QByteArray early = QByteArrayLiteral("valid-pair-before-nomination");
    const QByteArray final = QByteArrayLiteral("selected-pair-after-nomination");

    QObject::connect(&first, &Ice176::componentReady, [&](int component) {
        check(component == 0, "unexpected first ICE component");
        firstSelected = true;
    });
    QObject::connect(&second, &Ice176::componentReady, [&](int component) {
        check(component == 0, "unexpected second ICE component");
        secondSelected = true;
    });

    QObject::connect(&first, &Ice176::readyToSendMedia, [&]() {
        firstReady = true;
        firstReadyBeforeSelection = !firstSelected;
        if (usingProtocolSignalsIce2) {
            earlySent = true;
            first.writeDatagram(0, early);
        }
    });
    QObject::connect(&second, &Ice176::readyToSendMedia, [&]() {
        secondReady = true;
        secondReadyBeforeSelection = !secondSelected;
    });

    QObject::connect(&second, &Ice176::readyRead, [&](int component) {
        check(component == 0, "unexpected readable ICE component");
        while (second.hasPendingDatagrams(component)) {
            const auto datagram = second.readDatagram(component);
            if (datagram == early)
                earlyReceived = true;
            else if (datagram == final)
                selectedReceived = true;
        }
    });

    first.startChecks();
    second.startChecks();

    QEventLoop exchangeLoop;
    QTimer exchangePoll;
    QTimer exchangeDeadline;
    exchangeDeadline.setSingleShot(true);
    QObject::connect(&exchangeDeadline, &QTimer::timeout, &exchangeLoop, &QEventLoop::quit);
    QObject::connect(&exchangePoll, &QTimer::timeout, &exchangeLoop, [&]() {
        if (!selectedSent && firstSelected && secondSelected) {
            selectedSent = true;
            first.writeDatagram(0, final);
        }

        const bool earlyDone = usingProtocolSignalsIce2 ? earlyReceived : true;
        if (firstReady && secondReady && earlyDone && selectedReceived)
            exchangeLoop.quit();
    });
    exchangePoll.start(5);
    exchangeDeadline.start(8000);
    exchangeLoop.exec();
    exchangePoll.stop();

    check(firstReady && secondReady, "ICE never became writable");
    check(firstSelected && secondSelected, "ICE nomination did not eventually select a pair");
    check(selectedSent && selectedReceived, "data did not cross the selected pair");

    if (usingProtocolSignalsIce2) {
        check(firstReadyBeforeSelection && secondReadyBeforeSelection,
              "ice2-signaled ICE did not expose the valid pair before nomination");
        check(earlySent && earlyReceived, "data did not cross the pre-nomination valid pair");
    } else {
        check(!firstReadyBeforeSelection && !secondReadyBeforeSelection,
              "non-ice2 ICE usage received a premature writable signal");
        check(!earlySent && !earlyReceived, "application data was sent before nomination without local ice2 signaling");
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    runPair(false);
    runPair(true);

    qInfo("ICE valid-pair compatibility regressions passed");
    return 0;
}
