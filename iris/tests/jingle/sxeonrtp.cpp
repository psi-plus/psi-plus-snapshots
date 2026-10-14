// SPDX-License-Identifier: LGPL-2.1-or-later
// The first SCTP channel added after an RTP association has completed DTLS.
#include "sxeconnectionfixture.h"
#include <iris/dtls.h>
#include <iris/jingle-rtp.h>
namespace R = J::RTP;
class AudioEndpoint : public R::MediaEndpoint {
public:
    R::Description localOffer() const override
    {
        R::Description d;
        d.media   = "audio";
        d.rtcpMux = true;
        R::PayloadType p;
        p.id        = 96;
        p.name      = "opus";
        p.clockrate = 48000;
        p.channels  = 2;
        d.payloads.append(p);
        return d;
    }
    std::optional<R::Description> makeAnswer(const R::Description &d) const override { return d; }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &, const R::Description &) override { return true; }
    void stop() override { }
};
// The codec backend is a fixture; Jingle, DTLS, SCTP and SXE traffic are real.
class AudioSession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &) override
    {
        return std::make_unique<AudioEndpoint>();
    }
    bool configureSecureRtpEndpoints(const QList<R::SecureRtpEndpoint> &) override { return true; }
    bool configureSecureRtpAssociation(const R::SecureRtpParameters &) override { return true; }
    bool attachSecureRtpPacketIo(ProtectedPacketWriter) override { return true; }
};
class AudioProvider : public R::MediaProvider {
public:
    std::unique_ptr<R::MediaSession> createSession() override { return std::make_unique<AudioSession>(); }
    QStringList                      mediaTypes() const override { return { "audio" }; }
    QStringList                      secureRtpProfiles() const override { return { "SRTP_AES128_CM_HMAC_SHA1_80" }; }
};
static QDomElement rectangle(SXE::Application *board)
{
    return board->document()->document().documentElement().firstChildElement(QStringLiteral("rect"));
}

static void run(bool responderAdds, const QString &transportNs)
{
    auto            provider = std::make_shared<AudioProvider>();
    TcpPortReserver reserver;
    Endpoint        alice("alice"), bob("bob");
    for (auto e : { &alice, &bob }) {
        e->client.setTcpPortReserver(&reserver);
        e->client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
        e->client.jingleManager()->rtpManager()->setMediaProvider(provider);
        e->client.jingleManager()->rtpManager()->setTransportNamespaces({ transportNs });
    }
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures());
    setPeerFeatures(bob.client, alice.jid, alice.client.jingleManager()->discoFeatures());
    QPointer<J::Session> bobSession;
    QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                     [&](J::Session *s) { bobSession = s; });
    auto session = alice.client.jingleManager()->newSession(bob.jid);
    auto audio   = qobject_cast<R::Application *>(session->newContent(R::Description::ns(), J::Origin::Both));
    check(audio && audio->initializeOutgoing("audio"), "cannot create audio");
    session->addContent(audio);
    session->initiate();
    check(pumpUntil(alice, bob, [&] { return bool(bobSession); }), "no incoming audio");
    for (auto a : bobSession->contentList())
        a->prepare();
    bobSession->accept();
    check(pumpUntil(alice, bob,
                    [&] {
                        return session->state() == J::State::Active && bobSession->state() == J::State::Active
                            && audio->state() == J::State::Active;
                    }),
          "audio not active");
    auto                       source   = responderAdds ? bobSession.data() : session;
    auto                       receiver = responderAdds ? session : bobSession.data();
    QPointer<SXE::Application> remote;
    QObject::connect(receiver, &J::Session::newContentReceived, receiver, [&] {
        for (auto a : receiver->contentList())
            if (auto w = qobject_cast<SXE::Application *>(a); w && w->isRemote()) {
                remote = w;
                w->prepare();
            }
    });
    auto pad = source->applicationPadFactory(SXE::NS_SVG_WHITEBOARD);
    check(pad, "no whiteboard application pad");
    auto local = qobject_cast<SXE::Application *>(pad->manager()->startApplication(
        pad, responderAdds ? QStringLiteral("whiteboard-from-responder") : QStringLiteral("whiteboard-from-initiator"),
        source->role(), J::Origin::Both));
    check(local && local->setTransport(source->newOutgoingTransport(transportNs)), "cannot add whiteboard transport");
    const int messagesBefore = sxeMessages();
    source->addContent(local);
    const bool synced = pumpUntil(alice, bob, [&] { return synchronized(local, remote); });
    qInfo() << "responder adds=" << responderAdds << "synced=" << synced << "local state=" << int(local->state())
            << "remote=" << bool(remote) << "remote state=" << (remote ? int(remote->state()) : -1)
            << "local sync=" << int(local->syncState()) << "remote sync=" << (remote ? int(remote->syncState()) : -1);
    check(synced, "whiteboard added to RTP did not synchronize");
    check(dump(local->document()->document()) == dump(remote->document()->document()), "document mismatch");
    check(bundled(source, audio->contentName(), local->contentName())
              && bundled(receiver, audio->contentName(), local->contentName()),
          "whiteboard did not reuse RTP BUNDLE");
    check(dataChannel(local) && dataChannel(remote), "whiteboard has no SCTP channel");
    check(sxeMessages() == messagesBefore, "whiteboard fell back to messages instead of using the RTP association");
    check(audio->state() == J::State::Active, "adding the whiteboard disturbed RTP");
    QDomDocument temp;
    const auto   shape
        = parse(temp, QStringLiteral("<rect xmlns='http://www.w3.org/2000/svg' id='r' width='10' height='10'/>"));
    local->document()->insertNodeAfter(shape, local->document()->document().documentElement());
    local->document()->flush();
    check(
        pumpUntil(alice, bob, [&] { return rectangle(remote).attribute(QStringLiteral("id")) == QStringLiteral("r"); }),
        "host drawing did not reach the joiner");
    auto receivedShape = rectangle(remote);
    remote->document()->setAttribute(receivedShape, QStringLiteral("fill"), QStringLiteral("red"));
    remote->document()->flush();
    check(pumpUntil(alice, bob,
                    [&] { return rectangle(local).attribute(QStringLiteral("fill")) == QStringLiteral("red"); }),
          "joiner drawing did not reach the host");
    check(dump(local->document()->document()) == dump(remote->document()->document()), "drawings diverged");
    session->terminate(J::Reason::Success);
    pumpUntil(alice, bob, [] { return false; }, 200);
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    if (!Dtls::isSupported() || Dtls::supportedSRTPProfiles().isEmpty()) {
        qInfo("SXE on RTP tests skipped: no DTLS-SRTP backend");
        return 0;
    }
    for (const auto &transportNs : { J::ICE::NS, J::ICE::NS_ICE_UDP }) {
        for (bool responderAdds : { false, true })
            run(responderAdds, transportNs);
    }
    qInfo("SXE added to an active RTP association regressions passed");
}
