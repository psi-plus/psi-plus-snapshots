// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <iris/ice176.h>
#include <iris/dtls.h>
#include <iris/jingle-rtp-srtp.h>
#include <iris/jingle-rtp.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>
#define private public
#include <iris/jingle-ice.h>
#undef private
#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <qca.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Ack final : public Task {
public:
    explicit Ack(Task *parent) : Task(parent) { setSuccess(); }
};

struct MediaState {
    QString                             media;
    quint8                              payload = 0;
    quint32                             ssrc    = 0;
    J::RTP::MediaEndpoint::PacketWriter writer;
    QList<QByteArray>                   received;
    int                                 configured = 0;
    int                                 attached   = 0;
};

class Endpoint final : public J::RTP::MediaEndpoint {
public:
    explicit Endpoint(std::shared_ptr<MediaState> state) : state_(std::move(state)) { }

    J::RTP::Description localOffer() const override
    {
        J::RTP::Description description;
        description.media   = state_->media;
        description.rtcpMux = true;
        description.ssrc    = state_->ssrc;
        J::RTP::PayloadType payload;
        payload.id        = state_->payload;
        payload.name      = state_->media == QLatin1String("audio") ? QStringLiteral("opus") : QStringLiteral("VP8");
        payload.clockrate = state_->media == QLatin1String("audio") ? 48000u : 90000u;
        if (state_->media == QLatin1String("audio"))
            payload.channels = 2;
        description.payloads.append(payload);
        return description;
    }

    std::optional<J::RTP::Description> makeAnswer(const J::RTP::Description &offer) const override
    {
        if (offer.media != state_->media || !offer.rtcpMux || offer.payloads.size() != 1
            || offer.payloads.first().id != state_->payload)
            return {};
        return localOffer();
    }

    bool acceptsAnswer(const J::RTP::Description &offer, const J::RTP::Description &answer) const override
    {
        return offer.media == state_->media && answer.media == state_->media && answer.rtcpMux
            && answer.payloads.size() == 1 && answer.payloads.first().id == state_->payload;
    }

    bool configure(const J::RTP::Description &local, const J::RTP::Description &remote) override
    {
        if (local.media != state_->media || remote.media != state_->media)
            return false;
        ++state_->configured;
        return true;
    }

    bool supportsPacketIo() const override { return true; }

    bool attachPacketIo(PacketWriter writer) override
    {
        ++state_->attached;
        state_->writer = std::move(writer);
        return true;
    }

    void receivePacket(const QByteArray &packet, J::RTP::SrtpContext::Packet kind) override
    {
        check(kind == J::RTP::SrtpContext::Packet::Rtp, "BUNDLE media endpoint received non-RTP");
        state_->received.append(packet);
    }

    void stop() override { state_->writer = {}; }

private:
    std::shared_ptr<MediaState> state_;
};

class MediaSession final : public J::RTP::MediaSession {
public:
    explicit MediaSession(QHash<QString, std::shared_ptr<MediaState>> states) : states_(std::move(states)) { }

    std::unique_ptr<J::RTP::MediaEndpoint> createEndpoint(const QString &contentName,
                                                           const QString &media) override
    {
        auto state = states_.value(contentName);
        if (!state || state->media != media)
            return {};
        return std::make_unique<Endpoint>(std::move(state));
    }

private:
    QHash<QString, std::shared_ptr<MediaState>> states_;
};

class Provider final : public J::RTP::MediaProvider {
public:
    explicit Provider(QHash<QString, std::shared_ptr<MediaState>> states) : states_(std::move(states)) { }

    std::unique_ptr<J::RTP::MediaSession> createSession() override
    {
        return std::make_unique<MediaSession>(states_);
    }

    QStringList mediaTypes() const override { return { QStringLiteral("audio"), QStringLiteral("video") }; }

private:
    QHash<QString, std::shared_ptr<MediaState>> states_;
};

// The lower-level jingle_bundleice regression already exercises the production
// BUNDLE membership guard in ICE::Transport::start(). This layered regression
// keeps that association-level start under harness control so it can isolate the
// production RTP::Application + BundleRouter path without fabricating Session
// peer-group state.
class TestIceTransport final : public J::ICE::Transport {
public:
    using J::ICE::Transport::Transport;

    void start() override
    {
        if (state() >= J::State::Finishing)
            return;
        setState(J::State::Connecting);
    }
};

struct Side {
    Client                             client;
    J::Session                         session;
    J::TransportManagerPad::Ptr        baseIcePad;
    QSharedPointer<J::ICE::Pad>        icePad;
    QSharedPointer<J::RTP::Pad>        rtpPad;
    std::shared_ptr<MediaState>        audioState;
    std::shared_ptr<MediaState>        videoState;
    J::RTP::Application               *audioApp = nullptr;
    J::RTP::Application               *videoApp = nullptr;
    QSharedPointer<TestIceTransport>   audioTransport;
    QSharedPointer<TestIceTransport>   videoTransport;
    J::ICE::IceConnection             *network = nullptr;
    bool                               localApplications = false;

    Side(const Jid &peer, bool local, quint32 audioSsrc, quint32 videoSsrc, TcpPortReserver *reserver) :
        session(client.jingleManager(), peer, J::Origin::Initiator), localApplications(local)
    {
        client.setTcpPortReserver(reserver);
        client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);

        audioState = std::make_shared<MediaState>();
        audioState->media   = QStringLiteral("audio");
        audioState->payload = 96;
        audioState->ssrc    = audioSsrc;
        videoState = std::make_shared<MediaState>();
        videoState->media   = QStringLiteral("video");
        videoState->payload = 97;
        videoState->ssrc    = videoSsrc;

        QHash<QString, std::shared_ptr<MediaState>> states;
        states.insert(QStringLiteral("audio"), audioState);
        states.insert(QStringLiteral("video"), videoState);
        rtpPad = QSharedPointer<J::RTP::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                      std::make_shared<Provider>(states),
                                                      QStringList { J::ICE::NS });

        const auto creator = local ? J::Origin::Initiator : J::Origin::Responder;
        audioApp = new J::RTP::Application(rtpPad, QStringLiteral("audio"), creator, J::Origin::Both);
        videoApp = new J::RTP::Application(rtpPad, QStringLiteral("video"), creator, J::Origin::Both);
        if (local) {
            check(audioApp->initializeOutgoing(QStringLiteral("audio"))
                      && videoApp->initializeOutgoing(QStringLiteral("video")),
                  "outgoing RTP applications failed to initialize");
        }
        session.addContent(audioApp);
        session.addContent(videoApp);

        auto *rawPad = client.jingleManager()->transportPad(&session, J::ICE::NS);
        check(rawPad, "ICE pad unavailable");
        baseIcePad.reset(rawPad);
        icePad = qSharedPointerDynamicCast<J::ICE::Pad>(baseIcePad);
        check(bool(icePad), "wrong ICE pad type");

        const auto transportCreator = local ? J::Origin::Initiator : J::Origin::Responder;
        audioTransport = QSharedPointer<TestIceTransport>::create(baseIcePad, transportCreator);
        videoTransport = QSharedPointer<TestIceTransport>::create(baseIcePad, transportCreator);
        check(audioApp->setTransport(audioTransport) && videoApp->setTransport(videoTransport),
              "RTP applications rejected ICE transports");
        check(session.setGroupings({ J::ContentGroup { QStringLiteral("BUNDLE"),
                                                        { QStringLiteral("audio"), QStringLiteral("video") } } }),
              "RTP BUNDLE grouping rejected");

        bool audioBound = false, audioGrouped = false, videoBound = false, videoGrouped = false;
        auto *audioNetwork = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioGrouped);
        auto *videoNetwork = icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoGrouped);
        check(audioBound && videoBound && audioGrouped && videoGrouped && audioNetwork && audioNetwork == videoNetwork,
              "RTP BUNDLE applications did not stage one association");
        network = audioNetwork;
        check(icePad->liveAssociationCount() == 1, "RTP BUNDLE side staged multiple associations");
    }
};

static QByteArray rtpPacket(quint8 payload, quint16 sequence, quint32 timestamp, quint32 ssrc, const char *text)
{
    QByteArray packet(12, '\0');
    packet[0] = char(0x80);
    packet[1] = char(payload & 0x7f);
    packet[2] = char(sequence >> 8);
    packet[3] = char(sequence & 0xff);
    packet[4] = char(timestamp >> 24);
    packet[5] = char(timestamp >> 16);
    packet[6] = char(timestamp >> 8);
    packet[7] = char(timestamp);
    packet[8] = char(ssrc >> 24);
    packet[9] = char(ssrc >> 16);
    packet[10] = char(ssrc >> 8);
    packet[11] = char(ssrc);
    packet += text;
    return packet;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    check(!J::RTP::SrtpContext::supportedProfiles().isEmpty() && !Dtls::supportedSRTPProfiles().isEmpty(),
          "DTLS-SRTP backend required");

    TcpPortReserver reserver;
    Side first(Jid(QStringLiteral("second@example.test/device")), true, 0x11111111u, 0x22222222u, &reserver);
    Side second(Jid(QStringLiteral("first@example.test/device")), false, 0x33333333u, 0x44444444u, &reserver);

    bool failed = false;
    auto fail = [&]() { failed = true; };
    for (const auto &transport :
         { first.audioTransport, first.videoTransport, second.audioTransport, second.videoTransport })
        QObject::connect(transport.data(), &J::Transport::failed, &app, fail);

    first.audioApp->prepare();
    first.videoApp->prepare();

    auto exchange = [](TestIceTransport *from, TestIceTransport *to) {
        if (!from->hasUpdates())
            return false;
        auto [xml, ack] = from->takeOutgoingUpdate(false);
        check(!xml.isNull() && xml.namespaceURI() == J::ICE::NS, "wrong RTP BUNDLE ICE namespace");
        check(to->update(xml), "RTP BUNDLE ICE signaling update rejected");
        if (ack) {
            Ack result(from->pad()->session()->manager()->client()->rootTask());
            ack(&result);
        }
        return true;
    };

    QEventLoop loop;
    QTimer tick, deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    bool descriptionsDelivered = false;
    bool firstAudioSignaled = false, firstVideoSignaled = false, secondPrepared = false;
    bool secondAudioSignaled = false, secondVideoSignaled = false, answersApplied = false;
    bool checksStarted = false, initialSent = false, membersRemoved = false, staleSent = false, survivorSent = false;
    int secondAudioBeforeRemoval = 0, secondVideoBeforeSurvivor = 0;

    const auto audioPacket = rtpPacket(96, 1, 1, first.audioState->ssrc, "bundle-audio");
    const auto videoPacket = rtpPacket(97, 1, 2, first.videoState->ssrc, "bundle-video");
    const auto staleAudio  = rtpPacket(96, 2, 3, first.audioState->ssrc, "stale-audio");
    const auto survivor    = rtpPacket(97, 2, 4, first.videoState->ssrc, "bundle-survivor");

    QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
        if (!descriptionsDelivered && first.audioApp->localDescription() && first.videoApp->localDescription()) {
            check(second.audioApp->setRemoteOffer(first.audioApp->makeLocalOffer()) == J::Application::Ok
                      && second.videoApp->setRemoteOffer(first.videoApp->makeLocalOffer()) == J::Application::Ok,
                  "remote RTP BUNDLE offers were rejected");
            descriptionsDelivered = true;
        }

        firstAudioSignaled = exchange(first.audioTransport.data(), second.audioTransport.data()) || firstAudioSignaled;
        firstVideoSignaled = exchange(first.videoTransport.data(), second.videoTransport.data()) || firstVideoSignaled;

        if (!secondPrepared && descriptionsDelivered && firstAudioSignaled && firstVideoSignaled
            && second.audioTransport->state() == J::State::Pending
            && second.videoTransport->state() == J::State::Pending) {
            secondPrepared = true;
            first.audioApp->setState(J::State::Pending);
            first.videoApp->setState(J::State::Pending);
            second.audioApp->prepare();
            second.videoApp->prepare();
        }

        if (secondPrepared) {
            secondAudioSignaled
                = exchange(second.audioTransport.data(), first.audioTransport.data()) || secondAudioSignaled;
            secondVideoSignaled
                = exchange(second.videoTransport.data(), first.videoTransport.data()) || secondVideoSignaled;
        }

        if (!answersApplied && secondAudioSignaled && secondVideoSignaled && second.audioApp->localDescription()
            && second.videoApp->localDescription()) {
            answersApplied = true;
            check(first.audioApp->setRemoteAnswer(second.audioApp->makeLocalAnswer()) == J::Application::Ok
                      && first.videoApp->setRemoteAnswer(second.videoApp->makeLocalAnswer()) == J::Application::Ok,
                  "RTP BUNDLE answers were rejected");
            second.audioApp->setState(J::State::Accepted);
            second.videoApp->setState(J::State::Accepted);
            first.audioApp->start();
            first.videoApp->start();
            second.audioApp->start();
            second.videoApp->start();
        }

        if (!checksStarted && answersApplied
            && first.audioApp->state() >= J::State::Connecting && first.videoApp->state() >= J::State::Connecting
            && second.audioApp->state() >= J::State::Connecting && second.videoApp->state() >= J::State::Connecting
            && first.network->ice && second.network->ice) {
            checksStarted = true;
            first.network->ice->startChecks();
            second.network->ice->startChecks();
        }

        if (!initialSent && checksStarted && first.audioApp->state() == J::State::Active
            && first.videoApp->state() == J::State::Active && second.audioApp->state() == J::State::Active
            && second.videoApp->state() == J::State::Active) {
            initialSent = true;
            check(first.audioState->attached == 1 && first.videoState->attached == 1
                      && second.audioState->attached == 1 && second.videoState->attached == 1,
                  "RTP BUNDLE endpoints did not attach exactly once");
            check(first.audioState->writer(audioPacket, J::RTP::SrtpContext::Packet::Rtp),
                  "audio RTP BUNDLE writer failed");
            check(first.videoState->writer(videoPacket, J::RTP::SrtpContext::Packet::Rtp),
                  "video RTP BUNDLE writer failed");
        }

        if (!membersRemoved && initialSent && second.audioState->received.contains(audioPacket)
            && second.videoState->received.contains(videoPacket)) {
            check(!second.audioState->received.contains(videoPacket)
                      && !second.videoState->received.contains(audioPacket),
                  "BundleRouter cross-routed audio/video RTP");
            membersRemoved = true;
            secondAudioBeforeRemoval = second.audioState->received.size();
            secondVideoBeforeSurvivor = second.videoState->received.size();

            first.audioApp->remove(J::Reason::Success);
            second.audioApp->incomingRemove(J::Reason(J::Reason::Success));
            check(first.videoApp->state() == J::State::Active && second.videoApp->state() == J::State::Active,
                  "removing audio deactivated surviving video application");
            check(first.videoTransport->rtpSession() && first.videoTransport->rtpSession()->isReady()
                      && second.videoTransport->rtpSession() && second.videoTransport->rtpSession()->isReady(),
                  "removing audio invalidated shared video SRTP");
        }

        if (membersRemoved && !staleSent) {
            staleSent = true;
            auto *srtp = first.videoTransport->rtpSession();
            check(srtp && first.videoTransport->sendRtpPacket(staleAudio, J::RTP::SrtpContext::Packet::Rtp,
                                                              srtp->epoch()),
                  "authenticated stale-audio probe failed to enter shared SRTP");
        }

        if (staleSent && !survivorSent && second.audioState->received.size() == secondAudioBeforeRemoval
            && second.videoState->received.size() == secondVideoBeforeSurvivor) {
            survivorSent = true;
            check(first.videoState->writer(survivor, J::RTP::SrtpContext::Packet::Rtp),
                  "surviving video writer failed after audio removal");
        }

        if (survivorSent && second.videoState->received.contains(survivor)) {
            check(second.audioState->received.size() == secondAudioBeforeRemoval,
                  "removed audio route received post-removal traffic");
            check(second.videoState->received.size() == secondVideoBeforeSurvivor + 1,
                  "surviving video route received unexpected traffic");
            loop.quit();
        }
    });

    tick.start(5);
    deadline.start(15000);
    loop.exec();
    tick.stop();

    if (failed || !survivorSent || !second.videoState->received.contains(survivor))
        qWarning() << "RTP BUNDLE state" << int(first.audioApp->state()) << int(first.videoApp->state())
                   << int(second.audioApp->state()) << int(second.videoApp->state()) << descriptionsDelivered
                   << firstAudioSignaled << firstVideoSignaled << secondPrepared << secondAudioSignaled
                   << secondVideoSignaled << answersApplied << checksStarted << initialSent << membersRemoved
                   << staleSent << survivorSent;

    check(!failed && survivorSent && second.videoState->received.contains(survivor),
          "production RTP BundleRouter regression failed or timed out");
    check(first.icePad->liveAssociationCount() == 1 && second.icePad->liveAssociationCount() == 1,
          "removing one RTP BUNDLE member changed association count");

    qInfo("Production RTP BundleRouter regression passed");
    return 0;
}
