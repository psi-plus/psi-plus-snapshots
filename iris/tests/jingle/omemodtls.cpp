// SPDX-License-Identifier: LGPL-2.1-or-later
// OMEMO-verified DTLS fingerprints (OmemoDtlsVerification) between two clients: a whiteboard on an ICE
// SCTP data channel whose fingerprints travel encrypted, the end-to-end policy, and a tampered
// fingerprint. Reuses the OMEMO fixture with its in-memory PEP service; ICE runs on localhost.
#define main iris_omemoelement_main
#include "omemoelement.cpp"
#undef main

#include <iris/jingle-ice.h>
#include <iris/jingle-omemo-verification.h>
#include <iris/jingle-session.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp-im/jingle-sxe.h>
#include <iris/xmpp_caps.h>

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>

namespace J   = XMPP::Jingle;
namespace SXE = XMPP::Jingle::SXE;

namespace {
const QString VerificationNs(QStringLiteral("http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification"));

void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-omemo-dtls"));
    disco.setFeatures(Features(features));
    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

bool pumpUntil(Endpoint &a, Endpoint &b, const std::function<bool()> &condition, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        pump(a, b);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    pump(a, b);
    return condition();
}

// Fingerprints seen in Jingle IQs: plain DTLS ones and OMEMO-verified ones.
struct Wire {
    int plain    = 0;
    int verified = 0;
};

Wire inspect(const QDomElement &stanza)
{
    Wire       wire;
    const auto jingle = stanza.firstChildElement(QStringLiteral("jingle"));
    if (stanza.tagName() != QLatin1String("iq") || jingle.isNull())
        return wire;
    const auto all = jingle.elementsByTagName(QStringLiteral("fingerprint"));
    for (int i = 0; i < all.size(); ++i) {
        const auto ns = all.at(i).toElement().namespaceURI();
        if (ns == QLatin1String("urn:xmpp:jingle:apps:dtls:0"))
            ++wire.plain;
        else if (ns == VerificationNs)
            ++wire.verified;
    }
    return wire;
}

bool synchronized(SXE::Application *a, SXE::Application *b)
{
    return a && b && a->syncState() == SXE::Application::SyncState::Synchronized
        && b->syncState() == SXE::Application::SyncState::Synchronized;
}

// One whiteboard session from alice to bob after bob's <proceed/>; returns false if it did not run.
struct Run {
    QPointer<J::Session>       session;
    QPointer<J::Session>       peerSession;
    QPointer<SXE::Application> local;
    QPointer<SXE::Application> remote;
};

Run startSession(Endpoint &alice, Endpoint &bob, bool proceed, bool requireEndToEnd, bool peerRequires = false)
{
    Run        run;
    auto       connection = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                             [&run, peerRequires](J::Session *session) {
                                           run.peerSession = session;
                                           session->setEndToEndRequired(peerRequires);
                                       });
    const auto sid = QStringLiteral("omemo-dtls-") + QString::number(QRandomGenerator::global()->bounded(1000000));
    if (proceed) {
        bob.client.jingleManager()->sendMessageInitiation(
            alice.jid, J::MessageInitiation(J::MessageInitiation::Action::Proceed, sid));
        pump(alice, bob);
    }
    run.session = alice.client.jingleManager()->newSession(bob.jid, sid);
    run.session->setEndToEndRequired(requireEndToEnd);
    run.local = qobject_cast<SXE::Application *>(run.session->newContent(SXE::NS_SVG_WHITEBOARD));
    check(run.local, "whiteboard content was not created");
    run.session->addContent(run.local);
    run.session->initiate();
    pumpUntil(
        alice, bob, [&run] { return run.peerSession || !run.session || run.session->state() == J::State::Finished; },
        5000);
    if (run.peerSession) {
        for (auto content : run.peerSession->contentList())
            if (auto whiteboard = qobject_cast<SXE::Application *>(content))
                run.remote = whiteboard;
        if (run.remote) {
            run.remote->prepare();
            run.peerSession->accept();
        }
    }
    QObject::disconnect(connection);
    return run;
}

void exercise(Endpoint &alice, Endpoint &bob, bool omemo2)
{
    const auto  aliceFeatures = alice.client.jingleManager()->discoFeatures();
    QStringList omemoFeatures { OmemoEncryption::legacyDevicesNode() + QStringLiteral("+notify") };
    if (omemo2)
        omemoFeatures << OmemoEncryption::namespaceUri();
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures() + omemoFeatures);
    setPeerFeatures(bob.client, alice.jid, aliceFeatures + omemoFeatures);

    Wire wire;
    onStanza = [&wire](const Endpoint &, QDomElement &stanza) {
        const auto seen = inspect(stanza);
        wire.plain += seen.plain;
        wire.verified += seen.verified;
    };

    // Verified: bob's <proceed/> names his device, alice encrypts her fingerprint for it, and bob answers
    // for the device that sent it.
    auto run = startSession(alice, bob, true, false);
    check(pumpUntil(alice, bob, [&run] { return synchronized(run.local, run.remote); }),
          "the whiteboard with verified fingerprints did not synchronize");
    check(wire.verified >= 2 && wire.plain == 0, "DTLS fingerprints travelled unencrypted");
    const auto toBob   = run.peerSession->peerAuthentication();
    const auto toAlice = run.session->peerAuthentication();
    check(toBob.isValid() && toBob.deviceId == alice.omemo->ownDeviceId() && !toBob.identityKey.isEmpty(),
          "bob did not verify alice's fingerprint");
    check(toAlice.isValid() && toAlice.deviceId == bob.omemo->ownDeviceId(), "alice did not verify bob's fingerprint");
    run.session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);

    // End-to-end required without a known device of the peer: nothing is sent unprotected.
    wire = {};
    run  = startSession(alice, bob, false, true);
    pumpUntil(alice, bob, [&run] { return !run.session || run.session->state() == J::State::Finished; }, 5000);
    check(wire.plain == 0, "a session requiring end-to-end encryption sent an unprotected fingerprint");
    check(!run.peerSession, "a session without a verifiable fingerprint reached the peer");
    check(!run.session || run.session->state() == J::State::Finished, "the unprotectable session did not end");

    // Not required and no device: plain fingerprints, as without verification.
    wire = {};
    run  = startSession(alice, bob, false, false);
    check(pumpUntil(alice, bob, [&run] { return synchronized(run.local, run.remote); }),
          "the whiteboard without verification did not synchronize");
    check(wire.plain > 0 && wire.verified == 0 && !run.peerSession->peerAuthentication().isValid(),
          "an unverified session claims verification");
    run.session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);

    // The responder requires end-to-end encryption but the initiator's fingerprint is plain: the
    // responder does not answer with its own.
    int accepts = 0;
    onStanza    = [&accepts](const Endpoint &, QDomElement &stanza) {
        if (stanza.firstChildElement(QStringLiteral("jingle")).attribute(QStringLiteral("action"))
            == QLatin1String("session-accept"))
            ++accepts;
    };
    run = startSession(alice, bob, false, false, true);
    pumpUntil(alice, bob, [&run] { return !run.peerSession || run.peerSession->state() == J::State::Finished; }, 5000);
    check(accepts == 0, "a responder requiring end-to-end encryption accepted an unverified fingerprint");
    check(!run.peerSession || run.peerSession->state() == J::State::Finished,
          "the responder's unverifiable session did not end");
    if (run.session)
        run.session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);

    // A tampered fingerprint is refused.
    onStanza = [](const Endpoint &, QDomElement &stanza) {
        const auto jingle = stanza.firstChildElement(QStringLiteral("jingle"));
        if (jingle.attribute(QStringLiteral("action")) != QLatin1String("session-initiate"))
            return;
        const auto payloads = jingle.elementsByTagName(QStringLiteral("payload"));
        const auto keys     = jingle.elementsByTagName(QStringLiteral("key"));
        auto       target   = payloads.size() ? payloads.at(0) : keys.at(0);
        auto       text     = target.firstChild().toText();
        auto       data     = QByteArray::fromBase64(text.data().toLatin1());
        if (!data.isEmpty())
            data[data.size() / 2] = char(data[data.size() / 2] ^ 0x5a);
        text.setData(QString::fromLatin1(data.toBase64()));
    };
    run = startSession(alice, bob, true, false);
    pumpUntil(alice, bob, [&run] { return !run.session || run.session->state() == J::State::Finished; }, 5000);
    check(!run.peerSession, "a tampered fingerprint was accepted");
    onStanza = nullptr;
    qInfo("OMEMO %s DTLS verification passed", omemo2 ? "2" : "legacy");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    TcpPortReserver  reserver;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        endpoint->client.setTcpPortReserver(&reserver);
        endpoint->client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
        endpoint->client.jingleManager()->setMessageInitiationEnabled(true);
        auto setUp = wait(endpoint->omemo->setUp(), alice, bob);
        check(setUp->success(), "OMEMO was not set up");
    }
    if (!alice.client.jingleManager()->discoFeatures().contains(SXE::NS_CONNECTIONS)) {
        qInfo("OMEMO DTLS verification tests skipped: no DTLS data channels");
        pep.clear();
        return 0;
    }
    J::OmemoDtlsVerification aliceVerification(alice.client.jingleManager(), alice.omemo);
    J::OmemoDtlsVerification bobVerification(bob.client.jingleManager(), bob.omemo);

    const auto protocols = alice.omemo->supportedProtocols();
    if (protocols.testFlag(OmemoProtocol::Legacy))
        exercise(alice, bob, false);
    if (protocols.testFlag(OmemoProtocol::Omemo2))
        exercise(alice, bob, true);

    // A peer whose OMEMO 2 bundle the server no longer holds (one item per node, as old ejabberd) is
    // reached through its legacy OMEMO bundle, and the fingerprints are still verified.
    if (protocols.testFlag(OmemoProtocol::Legacy) && protocols.testFlag(OmemoProtocol::Omemo2)) {
        Endpoint carol(QStringLiteral("carol"));
        carol.client.setTcpPortReserver(&reserver);
        carol.client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
        carol.client.jingleManager()->setMessageInitiationEnabled(true);
        auto setUp = wait(carol.omemo->setUp(), alice, carol);
        check(setUp->success(), "OMEMO was not set up");
        J::OmemoDtlsVerification carolVerification(carol.client.jingleManager(), carol.omemo);
        const QStringList        omemoFeatures { OmemoEncryption::legacyDevicesNode() + QStringLiteral("+notify"),
                                          OmemoEncryption::namespaceUri() };
        setPeerFeatures(alice.client, carol.jid, carol.client.jingleManager()->discoFeatures() + omemoFeatures);
        setPeerFeatures(carol.client, alice.jid, alice.client.jingleManager()->discoFeatures() + omemoFeatures);
        pep[carol.jid.bare()][OmemoEncryption::bundlesNode()].clear();

        int legacy = 0, omemo2 = 0;
        onStanza = [&](const Endpoint &, QDomElement &stanza) {
            const auto fingerprints = stanza.elementsByTagNameNS(VerificationNs, QStringLiteral("fingerprint"));
            for (int i = 0; i < fingerprints.size(); ++i) {
                const auto encrypted = fingerprints.at(i).firstChildElement(QStringLiteral("encrypted"));
                if (encrypted.namespaceURI() == OmemoEncryption::namespaceUri())
                    ++omemo2;
                else if (!encrypted.isNull())
                    ++legacy;
            }
        };
        auto run = startSession(alice, carol, true, false);
        check(pumpUntil(alice, carol, [&run] { return synchronized(run.local, run.remote); }),
              "the whiteboard with a peer without an OMEMO 2 bundle did not synchronize");
        check(run.peerSession->peerAuthentication().deviceId == alice.omemo->ownDeviceId(),
              "the fingerprint to a peer without an OMEMO 2 bundle was not verified");
        check(legacy > 0, "the fingerprint did not fall back to legacy OMEMO");
        run.session->terminate(J::Reason::Success);
        pumpUntil(alice, carol, [] { return false; }, 200);
        onStanza = nullptr;
        qInfo("OMEMO DTLS verification falls back to legacy OMEMO without an OMEMO 2 bundle");
    }

    // Without a filter nothing can protect the transports: a session requiring it sends nothing.
    alice.client.jingleManager()->removeSignalingFilter(&aliceVerification);
    Wire unfiltered;
    onStanza = [&unfiltered](const Endpoint &, QDomElement &stanza) { unfiltered.plain += inspect(stanza).plain; };
    auto run = startSession(alice, bob, false, true);
    pumpUntil(alice, bob, [&run] { return !run.session || run.session->state() == J::State::Finished; }, 5000);
    check(unfiltered.plain == 0 && !run.peerSession && (!run.session || run.session->state() == J::State::Finished),
          "a session requiring end-to-end encryption ran without a signaling filter");
    onStanza = nullptr;
    alice.client.jingleManager()->addSignalingFilter(&aliceVerification);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("OMEMO DTLS verification tests passed");
    pep.clear();
    return 0;
}
