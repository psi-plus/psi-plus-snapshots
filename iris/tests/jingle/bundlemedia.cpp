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
    QString                           media;
    quint8                            payload = 0;
    quint32                           ssrc    = 0;
    std::function<bool(const QByteArray &)> writer;
    QList<QByteArray>                 received;
    int                               configured = 0;
    int                               attached   = 0;
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

    bool attachSecureRtpPacketIo(ProtectedPacketWriter writer) override
    {
        protectedWriter_ = std::move(writer);
        return bool(protectedWriter_);
    }

    void detachSecureRtpPacketIo() override
    {
        protectedWriter_ = {};
        for (const auto &state : states_)
            state->writer = {};
    }

    bool configureSecureRtpEndpoints(const QList<J::RTP::SecureRtpEndpoint> &endpoints) override
    {
        QSet<QByteArray> seen;
        for (const auto &endpoint : endpoints) {
            if (!endpoint.isValid() || seen.contains(endpoint.endpointId))
                return false;
            seen.insert(endpoint.endpointId);
        }

        endpoints_ = endpoints;
        for (const auto &endpoint : endpoints_) {
            auto state = stateForMedia(endpoint.media);
            if (!state)
                return false;
            if (!attachedEndpoints_.contains(endpoint.endpointId)) {
                attachedEndpoints_.insert(endpoint.endpointId);
                ++state->attached;
            }
            const auto endpointId = endpoint.endpointId;
            state->writer = [this, endpointId](const QByteArray &packet) {
                auto it = std::find_if(endpoints_.cbegin(), endpoints_.cend(),
                                       [&endpointId](const auto &value) { return value.endpointId == endpointId; });
                if (it == endpoints_.cend() || !protectedWriter_)
                    return false;
                const auto epoch = epochs_.value(it->associationId);
                if (!epoch)
                    return false;
                J::RTP::SecureRtpPacket protectedPacket;
                protectedPacket.associationId = it->associationId;
                protectedPacket.epoch         = epoch;
                protectedPacket.data          = packet;
                protectedPacket.kind          = J::RTP::PacketKind::Rtp;
                return protectedWriter_(protectedPacket);
            };
        }

        // Removed endpoints must immediately lose producer access.
        for (const auto &state : states_) {
            const bool alive = std::any_of(endpoints_.cbegin(), endpoints_.cend(),
                                           [&](const auto &endpoint) { return endpoint.media == state->media; });
            if (!alive)
                state->writer = {};
        }
        return true;
    }

    bool configureSecureRtpAssociation(const J::RTP::SecureRtpParameters &parameters) override
    {
        if (!parameters.isValid())
            return false;
        epochs_.insert(parameters.associationId, parameters.epoch);
        return true;
    }

    void invalidateSecureRtpAssociation(const QByteArray &associationId, quint64 epoch) override
    {
        if (epochs_.value(associationId) == epoch)
            epochs_.remove(associationId);
    }

    bool associationReady(const QByteArray &associationId, quint64 epoch) const
    {
        return epochs_.value(associationId) == epoch;
    }

    bool receiveProtectedRtpPacket(const J::RTP::SecureRtpPacket &packet) override
    {
        if (packet.kind != J::RTP::PacketKind::Rtp || epochs_.value(packet.associationId) != packet.epoch
            || packet.data.size() < 12 || (quint8(packet.data[0]) >> 6) != 2)
            return false;

        const quint8 payload = quint8(packet.data[1]) & 0x7f;
        const auto *bytes = reinterpret_cast<const uchar *>(packet.data.constData());
        const quint32 ssrc = (quint32(bytes[8]) << 24) | (quint32(bytes[9]) << 16)
            | (quint32(bytes[10]) << 8) | quint32(bytes[11]);

        const J::RTP::SecureRtpEndpoint *selected = nullptr;
        for (const auto &endpoint : endpoints_) {
            if (endpoint.associationId != packet.associationId)
                continue;
            if (endpoint.incomingSsrcs.contains(ssrc)) {
                if (selected)
                    return false;
                selected = &endpoint;
            }
        }
        if (!selected) {
            for (const auto &endpoint : endpoints_) {
                if (endpoint.associationId != packet.associationId
                    || !endpoint.incomingPayloadTypes.contains(payload))
                    continue;
                if (selected)
                    return false;
                selected = &endpoint;
            }
        }
        if (!selected)
            return false;

        auto state = stateForMedia(selected->media);
        if (!state)
            return false;
        state->received.append(packet.data);
        return true;
    }

private:
    std::shared_ptr<MediaState> stateForMedia(const QString &media) const
    {
        for (const auto &state : states_)
            if (state->media == media)
                return state;
        return {};
    }

    QHash<QString, std::shared_ptr<MediaState>> states_;
    QList<J::RTP::SecureRtpEndpoint>            endpoints_;
    QSet<QByteArray>                            attachedEndpoints_;
    QHash<QByteArray, quint64>                  epochs_;
    ProtectedPacketWriter                       protectedWriter_;
};

class Provider final : public J::RTP::MediaProvider {
public:
    explicit Provider(QHash<QString, std::shared_ptr<MediaState>> states) : states_(std::move(states)) { }

    std::unique_ptr<J::RTP::MediaSession> createSession() override
    {
        return std::make_unique<MediaSession>(states_);
    }

    QStringList mediaTypes() const override { return { QStringLiteral("audio"), QStringLiteral("video") }; }
    QStringList secureRtpProfiles() const override
    {
        return { QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80") };
    }

private:
    QHash<QString, std::shared_ptr<MediaState>> states_;
};

// The lower-level jingle_bundleice regression exercises ICE association
// ownership. This layered regression keeps association start under harness
// control and verifies the production RTP::Application -> group-level MediaSession
// boundary without routing plaintext RTP inside Iris.
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
    check(!Dtls::supportedSRTPProfiles().isEmpty(), "DTLS-SRTP backend required");

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
            check(first.audioState->writer && first.audioState->writer(audioPacket),
                  "audio RTP BUNDLE writer failed");
            check(first.videoState->writer && first.videoState->writer(videoPacket),
                  "video RTP BUNDLE writer failed");
        }

        if (!membersRemoved && initialSent && second.audioState->received.contains(audioPacket)
            && second.videoState->received.contains(videoPacket)) {
            check(!second.audioState->received.contains(videoPacket)
                      && !second.videoState->received.contains(audioPacket),
                  "backend group router cross-routed audio/video RTP");
            membersRemoved = true;
            secondAudioBeforeRemoval = second.audioState->received.size();
            secondVideoBeforeSurvivor = second.videoState->received.size();

            first.audioApp->remove(J::Reason::Success);
            second.audioApp->incomingRemove(J::Reason(J::Reason::Success));
            check(first.videoApp->state() == J::State::Active && second.videoApp->state() == J::State::Active,
                  "removing audio deactivated surviving video application");
            check(first.videoTransport->rtpAssociation() && first.videoTransport->rtpAssociation()->isReady()
                      && second.videoTransport->rtpAssociation() && second.videoTransport->rtpAssociation()->isReady(),
                  "removing audio invalidated shared video secure RTP");
        }

        if (membersRemoved && !staleSent) {
            staleSent = true;
            auto *association = first.videoTransport->rtpAssociation();
            check(association
                      && first.videoTransport->sendProtectedRtpPacket(
                          staleAudio, J::RTP::PacketKind::Rtp, association->epoch()),
                  "authenticated stale-audio probe failed to enter shared secure RTP");
        }

        if (staleSent && !survivorSent && second.audioState->received.size() == secondAudioBeforeRemoval
            && second.videoState->received.size() == secondVideoBeforeSurvivor) {
            survivorSent = true;
            check(first.videoState->writer && first.videoState->writer(survivor),
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
          "production RTP backend-routing regression failed or timed out");
    check(first.icePad->liveAssociationCount() == 1 && second.icePad->liveAssociationCount() == 1,
          "removing one RTP BUNDLE member changed association count");

    // An association QObject can disappear without first emitting invalidated
    // (for example while the ICE connection itself is being torn down). The
    // media backend must still lose the exported keys immediately rather than
    // keeping them staged until the whole MediaSession is destroyed.
    auto *firstBackend = dynamic_cast<MediaSession *>(first.rtpPad->mediaSession());
    auto *dyingAssociation = first.videoTransport->rtpAssociation();
    check(firstBackend && dyingAssociation, "secure RTP destruction fixture unavailable");
    const auto dyingId    = dyingAssociation->associationId();
    const auto dyingEpoch = dyingAssociation->epoch();
    check(firstBackend->associationReady(dyingId, dyingEpoch),
          "backend lost association before destruction regression");

    check(first.network && !first.network->components.isEmpty()
              && first.network->components[0].secureRtp == dyingAssociation,
          "unexpected BUNDLE association ownership before destruction");
    first.network->components[0].secureRtp = nullptr; // prevent IceConnection's later destructor from double deleting
    delete dyingAssociation;
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    check(!firstBackend->associationReady(dyingId, dyingEpoch),
          "destroyed secure RTP association left key material staged in backend");
    check(first.videoApp->state() >= J::State::Finishing,
          "destroyed secure RTP association left its application active");

    qInfo("Production RTP backend-routing regression passed");
    return 0;
}
