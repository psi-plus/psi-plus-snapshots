// SPDX-License-Identifier: LGPL-2.1-or-later
#include "sxeconnectionfixture.h"
#include <iris/dtls.h>
#include <iris/jingle-rtp.h>

namespace R = J::RTP;

class AudioEndpoint : public R::MediaEndpoint {
public:
    R::Description localOffer() const override
    {
        R::Description offer;
        offer.media   = "audio";
        offer.rtcpMux = true;
        R::PayloadType payload;
        payload.id        = 96;
        payload.name      = "opus";
        payload.clockrate = 48000;
        payload.channels  = 2;
        offer.payloads.append(payload);
        return offer;
    }
    std::optional<R::Description> makeAnswer(const R::Description &offer) const override { return offer; }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &, const R::Description &) override { return true; }
    void stop() override { }
};

class VideoEndpoint : public AudioEndpoint {
public:
    R::Description localOffer() const override
    {
        auto offer                  = AudioEndpoint::localOffer();
        offer.media                 = "video";
        offer.payloads[0].name      = "VP8";
        offer.payloads[0].clockrate = 90000;
        offer.payloads[0].channels  = 1;
        return offer;
    }
};
// Codec/capture are fixtures; signaling, ICE, DTLS and BUNDLE are production.
class RaceSession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        if (media == "video")
            return std::make_unique<VideoEndpoint>();
        return std::make_unique<AudioEndpoint>();
    }
    bool configureSecureRtpEndpoints(const QList<R::SecureRtpEndpoint> &) override { return true; }
    bool configureSecureRtpAssociation(const R::SecureRtpParameters &) override { return true; }
    bool attachSecureRtpPacketIo(ProtectedPacketWriter) override { return true; }
};
class RaceProvider : public R::MediaProvider {
public:
    std::unique_ptr<R::MediaSession> createSession() override { return std::make_unique<RaceSession>(); }
    QStringList                      mediaTypes() const override { return { "audio", "video" }; }
    QStringList                      secureRtpProfiles() const override { return { "SRTP_AES128_CM_HMAC_SHA1_80" }; }
};

static void crossedAdds(const QString &transportNs, const QString &kind, bool cancel, bool errorFirst = false)
{
    auto            provider = std::make_shared<RaceProvider>();
    TcpPortReserver reserver;
    Endpoint        alice("alice"), bob("bob");
    for (auto endpoint : { &alice, &bob }) {
        endpoint->client.setTcpPortReserver(&reserver);
        endpoint->client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
        auto manager = endpoint->client.jingleManager()->rtpManager();
        manager->setMediaProvider(provider);
        manager->setTransportNamespaces({ transportNs });
    }
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures());
    setPeerFeatures(bob.client, alice.jid, alice.client.jingleManager()->discoFeatures());
    QPointer<J::Session> incoming;
    auto observer = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                     [&](J::Session *session) { incoming = session; });
    auto session  = alice.client.jingleManager()->newSession(bob.jid);
    auto audio    = alice.client.jingleManager()->rtpManager()->createOutgoing(session, R::Media::Audio);
    check(audio, "no initial audio");
    session->initiate();
    check(pumpUntil(alice, bob, [&] { return bool(incoming); }), "no incoming audio session");
    QObject::disconnect(observer);
    for (auto content : incoming->contentList())
        content->prepare();
    incoming->accept();
    check(pumpUntil(alice, bob,
                    [&] {
                        return session->state() == J::State::Active && incoming->state() == J::State::Active
                            && audio->state() == J::State::Active;
                    }),
          "initial audio not active");
    for (auto owner : { session, incoming.data() })
        QObject::connect(owner, &J::Session::newContentReceived, owner, [owner] {
            for (auto content : owner->contentList())
                if (content->isRemote()
                    && (content->state() == J::State::Created || content->state() == J::State::Pending))
                    content->prepare();
        });
    const auto aliceName = kind + "-alice";
    const auto bobName   = kind + "-bob";
    int        aliceAdds = 0, bobAdds = 0, tieBreaks = 0, otherErrors = 0;
    onStanza = [&](const Endpoint &from, QDomElement &stanza) {
        const auto jingle = stanza.firstChildElement("jingle");
        if (jingle.attribute("action") == "content-add") {
            if (from.jid == alice.jid)
                ++aliceAdds;
            else
                ++bobAdds;
        }
        if (stanza.attribute("type") == "error") {
            if (stanza.firstChildElement("error").elementsByTagName("tie-break").isEmpty())
                ++otherErrors;
            else
                ++tieBreaks;
        }
    };
    const auto media = kind == "microphone" ? R::Media::Audio : R::Media::Video;
    auto       localAlice
        = alice.client.jingleManager()->rtpManager()->createOutgoing(session, media, J::Origin::Initiator, aliceName);
    QPointer<R::Application> localBob
        = bob.client.jingleManager()->rtpManager()->createOutgoing(incoming, media, J::Origin::Responder, bobName);
    check(localAlice && localBob, "crossed media contents not created");
    auto       directions     = static_cast<R::Pad *>(localBob->pad())->directionController();
    const auto policyRevision = directions->setLocalSending(localBob, true);
    check(policyRevision, "local capture intention not recorded");
    bool       ended         = false;
    const auto stateObserver = QObject::connect(localBob, &J::Application::stateChanged, incoming,
                                                [&](J::State state) { ended = ended || state >= J::State::Finishing; });
    // Prepare and serialize both offers before delivering either IQ.
    QElapsedTimer timer;
    timer.start();
    auto offered = [](const Endpoint &endpoint) {
        for (const auto &stanza : endpoint.stream.written)
            if (stanza.firstChildElement("jingle").attribute("action") == "content-add")
                return true;
        return false;
    };
    while ((!offered(alice) || !offered(bob)) && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(offered(alice) && offered(bob), "both content-adds were not in flight");
    if (errorFirst) {
        // Deliver the loser's IQ and its error before the winner's offer.
        auto winningOffer = std::move(alice.stream.written);
        alice.stream.written.clear();
        pump(alice, bob);
        check(tieBreaks == 1 && bobAdds == 1 && localBob && localBob->state() == J::State::Unacked,
              "error-before-offer prematurely ended or replayed the local intention");
        alice.stream.written = std::move(winningOffer);
    }
    if (cancel)
        localBob->remove(J::Reason::Cancel);
    check(pumpUntil(alice, bob,
                    [&] {
                        auto remoteAlice = incoming->content(aliceName, J::Origin::Initiator);
                        auto remoteBob   = session->content(bobName, J::Origin::Responder);
                        return localAlice->state() == J::State::Active && remoteAlice
                            && remoteAlice->state() == J::State::Active
                            && (cancel ? (!localBob || localBob->state() >= J::State::Finishing)
                                       : (localBob && localBob->state() == J::State::Active && remoteBob
                                          && remoteBob->state() == J::State::Active));
                    }),
          "crossed media intentions did not converge");
    check(aliceAdds == 1 && bobAdds == (cancel ? 1 : 2) && tieBreaks == 1 && otherErrors == 0,
          "collision did not produce exactly one bounded retry");
    if (!cancel) {
        check(!ended, "retry ended the application and discarded capture policy");
        check(bundled(session, audio->contentName(), bobName) && bundled(incoming, audio->contentName(), bobName),
              "retried content did not extend committed BUNDLE");
        check(localBob->senders() == J::Origin::Responder, "retry changed the local send intention");
        const auto policy = directions->policy(localBob);
        check(policy && policy->revision == policyRevision && policy->desiredSending
                  && directions->allowsLocalSending(localBob),
              "retry lost durable local-send policy");
    }
    check(audio->state() == J::State::Active, "crossed adds disturbed the original audio");
    QObject::disconnect(stateObserver);
    onStanza = nullptr;
    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    if (!Dtls::isSupported() || Dtls::supportedSRTPProfiles().isEmpty()) {
        qInfo("RTP content-add race tests skipped: no DTLS-SRTP backend");
        return 0;
    }
    for (const auto &ns : { J::ICE::NS, J::ICE::NS_ICE_UDP }) {
        for (const auto &kind : { QStringLiteral("camera"), QStringLiteral("screen"), QStringLiteral("microphone") })
            crossedAdds(ns, kind, false);
        crossedAdds(ns, QStringLiteral("camera"), true);
        crossedAdds(ns, QStringLiteral("screen"), false, true);
        crossedAdds(ns, QStringLiteral("camera"), true, true);
    }
    qInfo("RTP content-add intention recovery regressions passed");
}
