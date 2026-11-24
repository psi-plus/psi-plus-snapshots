// SPDX-License-Identifier: LGPL-2.1-or-later
// OMEMO-verified DTLS fingerprints (OmemoDtlsVerification) between two clients: a whiteboard on an ICE
// SCTP data channel whose fingerprints travel encrypted, the end-to-end policy, and a tampered
// fingerprint. Reuses the OMEMO fixture with its in-memory PEP service; ICE runs on localhost.
#define main iris_omemoelement_main
#include "omemoelement.cpp"
#undef main

#include <iris/jingle-ice.h>
#include <iris/jingle-ft.h>
#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/jingle-jet.h>
#include <QBuffer>
#include <iris/jingle-dtls-omemo.h>
#include <iris/jingle-session.h>
#include <iris/jingle-sxe-omemo.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp-im/jingle-sxe.h>
#include <iris/xmpp_caps.h>

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>

namespace J   = XMPP::Jingle;
namespace SXE = XMPP::Jingle::SXE;
namespace FT = XMPP::Jingle::FileTransfer;

namespace {
const QString VerificationNs(QStringLiteral("http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification"));

void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    // Exercise DTLS verification rather than the alternative OMEMO SXE payload transport.
    features.removeAll(SXE::SxeOmemo::feature());
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
                                             [&run, peerRequires, proceed](J::Session *session) {
                                           run.peerSession = session;
                                           check(session->setEncryptionMethod(OmemoEncryption::methodId()),
                                                 "Incoming DTLS session could not select OMEMO");
                                           session->setEncryptionPolicy(peerRequires ? J::EncryptionPolicy::Required
                                               : (proceed ? J::EncryptionPolicy::Preferred : J::EncryptionPolicy::Disabled));
                                       });
    const auto sid = QStringLiteral("omemo-dtls-") + QString::number(QRandomGenerator::global()->bounded(1000000));
    if (proceed) {
        bob.client.jingleManager()->sendMessageInitiation(
            alice.jid, J::MessageInitiation(J::MessageInitiation::Action::Proceed, sid));
        pump(alice, bob);
    }
    run.session = alice.client.jingleManager()->newSession(bob.jid, sid);
    check(run.session->setEncryptionMethod(OmemoEncryption::methodId()),
          "Outgoing DTLS session could not select OMEMO");
    run.session->setEncryptionPolicy(requireEndToEnd ? J::EncryptionPolicy::Required
        : (proceed ? J::EncryptionPolicy::Preferred : J::EncryptionPolicy::Disabled));
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
        if (run.remote && run.peerSession->state() == J::State::Created)
            run.peerSession->accept();
    }
    QObject::disconnect(connection);
    return run;
}

// Direct IQ file requests must not need JMI or an extra JET layer.
void fileRanges(Endpoint &alice, Endpoint &bob, bool native = true)
{
    auto features = [native](Endpoint &endpoint) {
        auto result = endpoint.client.jingleManager()->discoFeatures()
            + QStringList { OmemoEncryption::namespaceUri(),
                            OmemoEncryption::legacyDevicesNode() + QStringLiteral("+notify") };
        if (!native)
            result.removeAll(J::OmemoDtlsVerification::directFeature());
        return result;
    };
    setPeerFeatures(alice.client, bob.jid, features(bob));
    setPeerFeatures(bob.client, alice.jid, features(alice));

    const QByteArray payload = QByteArray(8192, 'a') + QByteArray(8192, 'b');
    FT::File file;
    file.setName(QStringLiteral("ranges.bin"));
    file.setSize(quint64(payload.size()));
    file.addHash(Hash::from(Hash::Sha256, payload));

    QPointer<J::Session> remoteSession;
    QList<QPointer<FT::Application>> senders;
    Wire initialWire, extensionWire;
    int jet = 0, receipts = 0, sessions = 0;
    bool extending = false;
    onStanza = [&](const Endpoint &, QDomElement &stanza) {
        const auto seen = inspect(stanza);
        auto &wire = extending ? extensionWire : initialWire;
        wire.verified += seen.verified;
        wire.plain += seen.plain;
        const auto jingle = stanza.firstChildElement(QStringLiteral("jingle"));
        jet += jingle.elementsByTagNameNS(J::JET::Manager::ns(), QStringLiteral("security")).size();
        if (!jingle.firstChildElement(QStringLiteral("received")).isNull())
            ++receipts;
    };
    auto incoming = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
        [&](J::Session *session) {
            remoteSession = session;
            ++sessions;
            check(session->setEncryptionMethod(OmemoEncryption::methodId()), "missing incoming OMEMO");
            session->setEncryptionPolicy(J::EncryptionPolicy::Required);
            auto accept = [&, session]() {
                for (auto *content : session->contentList()) {
                    if (content->state() != J::State::Created)
                        continue;
                    auto *a = qobject_cast<FT::Application *>(content);
                    check(a && a->senders() == session->role(), "unexpected file offer");
                    senders.append(a);
                    const bool independent = a->transport()->pad()->ns() == J::IBB::NS;
                    if (independent)
                        check(a->setEncryptionPolicy(J::EncryptionPolicy::Disabled),
                              "could not override incoming content policy");
                    a->setAcceptFile(a->file());
                    a->setKeepTransportUntilReceipt();
                    QObject::connect(a, &FT::Application::deviceRequested, a,
                        [&, a, independent](quint64 offset, std::optional<quint64>) {
                            check(a->isTransportEncrypted() == (!native && !independent),
                                  "incorrect payload protection on sender");
                            auto *source = new QBuffer(a);
                            source->setData(payload);
                            source->open(QIODevice::ReadOnly);
                            check(source->seek(qint64(offset)), "source seek failed");
                            a->setDevice(source);
                        });
                    if (session->state() != J::State::Created)
                        a->prepare();
                }
            };
            QObject::connect(session, &J::Session::newContentReceived, session, accept);
            accept();
            session->accept();
        });

    QPointer<J::Session> session = alice.client.jingleManager()->newSession(bob.jid);
    check(session->setEncryptionMethod(OmemoEncryption::methodId()), "missing outgoing OMEMO");
    session->setEncryptionPolicy(J::EncryptionPolicy::Required);
    QByteArray received[3];
    bool verified[3] = { false, false, false };
    QObject consumer;
    auto request = [&](int i) {
        auto *a = qobject_cast<FT::Application *>(session->newContent(FT::NS, session->peerRole()));
        check(a, "could not create range request");
        auto range = file;
        range.setRange(FT::Range(quint64((i % 2) * 8192), 8192));
        a->setFile(range);
        a->setStreamingMode();
        a->setKeepTransportUntilReceipt();
        a->setReceiptDeferred();
        const bool independent = i == 2;
        if (independent)
            check(a->setEncryptionPolicy(J::EncryptionPolicy::Disabled), "could not override content policy");
        check(a->setTransport(session->newOutgoingTransport(independent ? J::IBB::NS : J::ICE::NS)),
              "could not select range transport");
        QObject::connect(a, &FT::Application::connectionReady, &consumer, [&, a, i] {
            check(a->isTransportEncrypted() == (!native && i != 2), "incorrect payload protection on receiver");
            auto connection = a->connection();
            auto read = [&, i, connection] { received[i] += connection->readAll(); };
            QObject::connect(connection.data(), &QIODevice::readyRead, &consumer, read);
            read();
        });
        QObject::connect(a, &FT::Application::payloadVerified, &consumer, [&, i] { verified[i] = true; });
        session->addContent(a);
        return QPointer<FT::Application>(a);
    };
    auto first = request(0);
    session->initiate();
    check(pumpUntil(alice, bob, [&] { return verified[0]; }), "direct encrypted range did not complete");
    if (!native) {
        check(first && received[0] == payload.left(8192) && jet > 0
                  && initialWire.verified == 0 && initialWire.plain >= 2,
              "required JET over ICE did not use independent payload protection");
        check(first->acknowledgeReceived(), "JET range receipt was lost");
        check(pumpUntil(alice, bob, [&] { return receipts == 1; }), "JET range receipt did not arrive");
        if (session)
            session->terminate(J::Reason::Success);
        pumpUntil(alice, bob, [] { return false; }, 200);
        QObject::disconnect(incoming);
        onStanza = nullptr;
        return;
    }
    check(first && senders.size() == 1 && senders[0] && senders[0]->state() < J::State::Finished
              && receipts == 0 && initialWire.verified >= 2 && initialWire.plain == 0 && jet == 0,
          "initial range did not use authenticated DTLS and deferred receipt");
    check(session && remoteSession && session->peerAuthentication().isValid()
              && remoteSession->peerAuthentication().isValid(), "DTLS authentication missing");

    extending = true;
    // An unrelated content with its own transport and an explicit Disabled
    // policy must not change the authenticated ICE association or its policy.
    auto independent = request(2);
    check(pumpUntil(alice, bob, [&] { return verified[2]; }), "independent content did not complete");
    check(independent && received[2] == payload.left(8192)
              && independent->encryptionPolicy() == J::EncryptionPolicy::Disabled
              && session->encryptionPolicy() == J::EncryptionPolicy::Required
              && first && first->transport()->hasAuthenticatedEncryption()
              && jet == 0 && extensionWire.verified == 0,
          "independent content altered the protected association");
    check(independent->acknowledgeReceived(), "independent receipt was lost");
    check(pumpUntil(alice, bob, [&] { return receipts == 1; }), "independent receipt did not arrive");

    auto second = request(1);
    check(pumpUntil(alice, bob, [&] {
        if (!session || !second)
            return false;
        for (const auto &group : session->negotiatedGroupings())
            if (group.semantics == QLatin1String("BUNDLE") && group.contents.contains(second->contentName()))
                return true;
        return false;
    }), "authenticated BUNDLE extension did not commit");
    check(first && first->acknowledgeReceived(), "predecessor receipt was lost");
    check(pumpUntil(alice, bob, [&] { return verified[1]; }), "authenticated BUNDLE range did not complete");
    check(second && sessions == 1 && jet == 0 && extensionWire.verified == 0,
          "BUNDLE repeated OMEMO key exchange or opened another session");
    check(received[0] == payload.left(8192) && received[1] == payload.mid(8192),
          "DTLS range payload mismatch");
    check(second->acknowledgeReceived(), "successor receipt was lost");
    check(pumpUntil(alice, bob, [&] { return receipts == 3; }), "range receipts did not arrive");
    if (session)
        session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
    QObject::disconnect(incoming);
    onStanza = nullptr;
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

    // A tampered fingerprint is refused, never retried on an unprotected transport.
    int replacements = 0;
    onStanza = [&replacements](const Endpoint &, QDomElement &stanza) {
        const auto jingle = stanza.firstChildElement(QStringLiteral("jingle"));
        if (jingle.attribute(QStringLiteral("action")) == QLatin1String("transport-replace"))
            ++replacements;
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
    check(!run.peerSession || run.peerSession->state() == J::State::Finished,
          "a tampered fingerprint was allowed to start a transport");
    check(!synchronized(run.local, run.remote) && replacements == 0,
          "tampered DTLS authentication exposed application data or triggered fallback");
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
    check(alice.client.jingleManager()->dtlsVerification()
              && bob.client.jingleManager()->dtlsVerification(),
          "The Jingle manager did not register the DTLS verification service");

    const auto protocols = alice.omemo->supportedProtocols();
    if (protocols.testFlag(OmemoProtocol::Legacy))
        exercise(alice, bob, false);
    if (protocols.testFlag(OmemoProtocol::Omemo2))
        exercise(alice, bob, true);

    fileRanges(alice, bob);
    fileRanges(alice, bob, false); // JET over ICE without direct fingerprint verification.

    // A peer whose OMEMO 2 bundle the server no longer holds (one item per node, as old ejabberd) is
    // reached through its legacy OMEMO bundle, and the fingerprints are still verified.
    if (protocols.testFlag(OmemoProtocol::Legacy) && protocols.testFlag(OmemoProtocol::Omemo2)) {
        Endpoint carol(QStringLiteral("carol"));
        carol.client.setTcpPortReserver(&reserver);
        carol.client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
        carol.client.jingleManager()->setMessageInitiationEnabled(true);
        auto setUp = wait(carol.omemo->setUp(), alice, carol);
        check(setUp->success(), "OMEMO was not set up");
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

    // Simulate an unavailable DTLS verifier while keeping the OMEMO method registered.
    delete alice.client.jingleManager()->dtlsVerification();
    Wire unfiltered;
    onStanza = [&unfiltered](const Endpoint &, QDomElement &stanza) { unfiltered.plain += inspect(stanza).plain; };
    auto run = startSession(alice, bob, false, true);
    pumpUntil(alice, bob, [&run] { return !run.session || run.session->state() == J::State::Finished; }, 5000);
    check(unfiltered.plain == 0 && !run.peerSession && (!run.session || run.session->state() == J::State::Finished),
          "a session requiring end-to-end encryption ran without a DTLS verifier");
    onStanza = nullptr;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("OMEMO DTLS verification tests passed");
    pep.clear();
    return 0;
}
