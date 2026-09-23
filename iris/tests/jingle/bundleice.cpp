// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <iris/dtls.h>
#include <iris/ice176.h>
#include <iris/jingle-application.h>
#include <iris/jingle-rtp-srtp.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>
#define private public
#include <iris/jingle-ice.h>
#undef private
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
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

class TestApplicationPad final : public J::ApplicationManagerPad {
public:
    explicit TestApplicationPad(J::Session *session) : session_(session) { }
    J::Session *session() const override { return session_; }
    QString ns() const override { return QStringLiteral("urn:iris:test:bundle-runtime"); }
    J::ApplicationManager *manager() const override { return nullptr; }
    QString generateContentName(J::Origin) override { return {}; }

private:
    J::Session *session_ = nullptr;
};

class TestTransportSelector final : public J::TransportSelector {
public:
    QSharedPointer<J::Transport> getNextTransport() override { return {}; }
    QSharedPointer<J::Transport> getAlikeTransport(QSharedPointer<J::Transport>) override { return {}; }
    bool replace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport> newer) override { return bool(newer); }
    void backupTransport(QSharedPointer<J::Transport>) override { }
    bool hasMoreTransports() const override { return false; }
    bool hasTransport(QSharedPointer<J::Transport>) const override { return false; }
    int compare(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) const override { return 0; }
};

class TestApplication final : public J::Application {
public:
    TestApplication(const J::ApplicationManagerPad::Ptr &pad, QString name)
    {
        _pad               = pad;
        _contentName       = std::move(name);
        _creator           = J::Origin::Initiator;
        _senders           = J::Origin::Both;
        _transportSelector = std::make_unique<TestTransportSelector>();
    }

    void setState(J::State state) override { _state = state; }
    const std::optional<XMPP::Stanza::Error> &lastError() const override { return error_; }
    J::Reason lastReason() const override { return reason_; }
    SetDescError setRemoteOffer(const QDomElement &) override { return Ok; }
    SetDescError setRemoteAnswer(const QDomElement &) override { return Ok; }
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    void prepare() override { }
    void start() override { }
    bool allowsSharedTransport() const override { return true; }
    void remove(J::Reason::Condition = J::Reason::Success, const QString & = {}) override { }
    void incomingRemove(const J::Reason &) override { }

protected:
    void prepareTransport() override { }

private:
    std::optional<XMPP::Stanza::Error> error_;
    J::Reason reason_;
};

class TestIceTransport final : public J::ICE::Transport {
public:
    using J::ICE::Transport::Transport;
    void forceState(J::State state) { setState(state); }
};

struct Side {
    Client client;
    J::Session session;
    J::TransportManagerPad::Ptr basePad;
    QSharedPointer<J::ICE::Pad> icePad;
    J::ApplicationManagerPad::Ptr appPad;
    TestApplication *audioApp = nullptr;
    TestApplication *videoApp = nullptr;
    QSharedPointer<TestIceTransport> audio;
    QSharedPointer<TestIceTransport> video;
    J::ICE::IceConnection *network = nullptr;

    Side(const Jid &peer, J::Origin transportCreator, TcpPortReserver *reserver) :
        session(client.jingleManager(), peer, J::Origin::Initiator)
    {
        client.setTcpPortReserver(reserver);
        client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);

        auto *rawPad = client.jingleManager()->transportPad(&session, J::ICE::NS);
        check(rawPad, "ICE pad unavailable");
        basePad.reset(rawPad);
        icePad = qSharedPointerDynamicCast<J::ICE::Pad>(basePad);
        check(bool(icePad), "wrong ICE pad type");

        appPad.reset(new TestApplicationPad(&session));
        audioApp = new TestApplication(appPad, QStringLiteral("audio"));
        videoApp = new TestApplication(appPad, QStringLiteral("video"));
        session.addContent(audioApp);
        session.addContent(videoApp);

        audio = QSharedPointer<TestIceTransport>::create(basePad, transportCreator);
        video = QSharedPointer<TestIceTransport>::create(basePad, transportCreator);
        check(audioApp->setTransport(audio) && videoApp->setTransport(video),
              "BUNDLE applications rejected ICE transports");
        check(session.setGroupings({ J::ContentGroup { QStringLiteral("BUNDLE"),
                                                        { QStringLiteral("audio"), QStringLiteral("video") } } }),
              "BUNDLE grouping rejected");

        bool audioBound = false, audioGrouped = false, videoBound = false, videoGrouped = false;
        auto *audioNetwork = icePad->groupedConnectionFor(audio.data(), &audioBound, &audioGrouped);
        auto *videoNetwork = icePad->groupedConnectionFor(video.data(), &videoBound, &videoGrouped);
        check(audioBound && videoBound && audioGrouped && videoGrouped && audioNetwork && audioNetwork == videoNetwork,
              "BUNDLE members did not stage one association");
        network = audioNetwork;
        check(icePad->liveAssociationCount() == 1, "BUNDLE side staged multiple associations");
        const auto profiles = Dtls::supportedSRTPProfiles();
        check(!profiles.isEmpty() && audio->enableRtpMux(profiles) && video->enableRtpMux(profiles),
              "BUNDLE RTP mux setup failed");
    }

    ~Side()
    {
        if (audioApp)
            delete audioApp;
        if (videoApp)
            delete videoApp;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    check(!Dtls::supportedSRTPProfiles().isEmpty(), "DTLS-SRTP backend required");

    TcpPortReserver reserver;
    Side first(Jid(QStringLiteral("second@example.test/device")), J::Origin::Initiator, &reserver);
    Side second(Jid(QStringLiteral("first@example.test/device")), J::Origin::Responder, &reserver);

    check(first.network != second.network, "sessions shared an ICE association");
    QPointer<J::ICE::IceConnection> firstGuard(first.network), secondGuard(second.network);

    bool failed = false;
    auto fail = [&]() { failed = true; };
    for (const auto &transport : { first.audio, first.video, second.audio, second.video })
        QObject::connect(transport.data(), &J::Transport::failed, &app, fail);

    first.audio->prepare();
    first.video->prepare();
    check(first.audio->state() == J::State::ApprovedToSend && first.video->state() == J::State::ApprovedToSend,
          "initiator BUNDLE transports did not prepare");
    check(first.audio->rtpAssociation() && first.audio->rtpAssociation() == first.video->rtpAssociation(),
          "initiator did not share secure RTP");

    auto exchange = [](TestIceTransport *from, TestIceTransport *to) {
        if (!from->hasUpdates())
            return false;
        auto [xml, ack] = from->takeOutgoingUpdate(false);
        check(!xml.isNull() && xml.namespaceURI() == J::ICE::NS, "wrong BUNDLE ICE namespace");
        check(to->update(xml), "BUNDLE ICE signaling update rejected");
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

    bool firstAudioSignaled = false, firstVideoSignaled = false, secondPrepared = false;
    bool secondAudioSignaled = false, secondVideoSignaled = false, checksStarted = false;
    bool audioReceived = false, videoReceived = false, survivorReceived = false, replyReceived = false;
    QElapsedTimer answerSettled;

    const QByteArray audioPacket
        = QByteArray::fromHex("806000010000000111111111") + QByteArrayLiteral("bundle-audio");
    const QByteArray videoPacket
        = QByteArray::fromHex("806100010000000222222222") + QByteArrayLiteral("bundle-video");
    QByteArray survivorPacket
        = QByteArray::fromHex("806100020000000322222222") + QByteArrayLiteral("bundle-survivor");
    const QByteArray replyPacket
        = QByteArray::fromHex("806100030000000433333333") + QByteArrayLiteral("bundle-reply");

    QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
        firstAudioSignaled = exchange(first.audio.data(), second.audio.data()) || firstAudioSignaled;
        firstVideoSignaled = exchange(first.video.data(), second.video.data()) || firstVideoSignaled;

        if (!secondPrepared && firstAudioSignaled && firstVideoSignaled
            && second.audio->state() == J::State::Pending && second.video->state() == J::State::Pending) {
            secondPrepared = true;
            second.audio->prepare();
            second.video->prepare();
            check(second.audio->state() == J::State::ApprovedToSend
                      && second.video->state() == J::State::ApprovedToSend,
                  "responder BUNDLE transports did not prepare");
            check(second.audio->rtpAssociation() && second.audio->rtpAssociation() == second.video->rtpAssociation(),
                  "responder did not share secure RTP");
        }

        if (secondPrepared) {
            secondAudioSignaled = exchange(second.audio.data(), first.audio.data()) || secondAudioSignaled;
            secondVideoSignaled = exchange(second.video.data(), first.video.data()) || secondVideoSignaled;
        }

        if (!checksStarted && secondAudioSignaled && secondVideoSignaled) {
            if (!answerSettled.isValid())
                answerSettled.start();
            if (answerSettled.elapsed() >= 50) {
                checksStarted = true;
                first.audio->forceState(J::State::Connecting);
                first.video->forceState(J::State::Connecting);
                second.audio->forceState(J::State::Connecting);
                second.video->forceState(J::State::Connecting);
                first.network->ice->startChecks();
                second.network->ice->startChecks();
            }
        }

        auto firstSecure = first.video->rtpAssociation();
        auto secondSecure = second.video->rtpAssociation();
        if (!checksStarted || !firstSecure || !secondSecure || !firstSecure->isReady() || !secondSecure->isReady())
            return;

        static bool handlersInstalled = false;
        static bool initialSent = false;
        static bool survivorSent = false;
        static bool replySent = false;
        if (!handlersInstalled) {
            handlersInstalled = true;
            QObject::connect(secondSecure, &J::RTP::SecureRtpAssociation::protectedPacketReceived, &loop,
                             [&](const QByteArray &packet, J::RTP::PacketKind kind, quint64) {
                                 check(kind == J::RTP::PacketKind::Rtp, "BUNDLE delivered non-RTP");
                                 if (packet == audioPacket)
                                     audioReceived = true;
                                 else if (packet == videoPacket)
                                     videoReceived = true;
                                 else if (packet == survivorPacket)
                                     survivorReceived = true;
                             });
            QObject::connect(firstSecure, &J::RTP::SecureRtpAssociation::protectedPacketReceived, &loop,
                             [&](const QByteArray &packet, J::RTP::PacketKind kind, quint64) {
                                 if (kind == J::RTP::PacketKind::Rtp && packet == replyPacket)
                                     replyReceived = true;
                             });
        }

        if (!initialSent) {
            initialSent = true;
            const auto epoch = firstSecure->epoch();
            check(first.audio->sendProtectedRtpPacket(audioPacket, J::RTP::PacketKind::Rtp, epoch),
                  "audio BUNDLE member failed to send");
            check(first.video->sendProtectedRtpPacket(videoPacket, J::RTP::PacketKind::Rtp, epoch),
                  "video BUNDLE member failed to send");
        }

        if (!survivorSent && audioReceived && videoReceived) {
            survivorSent = true;
            auto *shared = first.video->rtpAssociation();
            const auto epoch = shared->epoch();
            first.audio->stop();
            check(first.audio->state() == J::State::Finished, "audio member did not stop");
            check(first.video->rtpAssociation() == shared && shared->isReady() && shared->epoch() == epoch,
                  "stopping audio invalidated shared secure RTP");
            check(first.video->sendProtectedRtpPacket(survivorPacket, J::RTP::PacketKind::Rtp, epoch),
                  "surviving video member failed to send");
        }

        if (!replySent && survivorReceived) {
            replySent = true;
            auto *shared = second.video->rtpAssociation();
            check(shared && shared->isReady(), "responder shared secure RTP disappeared");
            check(second.video->sendProtectedRtpPacket(replyPacket, J::RTP::PacketKind::Rtp, shared->epoch()),
                  "responder video reply failed");
        }

        if (replyReceived)
            loop.quit();
    });

    tick.start(5);
    deadline.start(12000);
    loop.exec();
    tick.stop();

    check(!failed && checksStarted && audioReceived && videoReceived && survivorReceived && replyReceived,
          "shared ICE/DTLS/secure-RTP BUNDLE loopback failed");

    auto *survivingSecure = first.video->rtpAssociation();
    check(survivingSecure && survivingSecure->isReady(), "shared secure RTP not ready before member removal");
    delete first.audioApp;
    first.audioApp = nullptr;
    check(firstGuard && first.icePad->liveAssociationCount() == 1 && first.video->rtpAssociation() == survivingSecure
              && survivingSecure->isReady(),
          "removing stopped audio destroyed the surviving association");

    delete first.videoApp;
    first.videoApp = nullptr;
    check(!firstGuard && first.icePad->liveAssociationCount() == 0 && !first.audio->rtpAssociation()
              && !first.video->rtpAssociation(),
          "last initiator BUNDLE member retained or dangled the association");

    delete second.audioApp;
    second.audioApp = nullptr;
    check(secondGuard && second.icePad->liveAssociationCount() == 1,
          "removing responder audio destroyed video association");
    delete second.videoApp;
    second.videoApp = nullptr;
    check(!secondGuard && second.icePad->liveAssociationCount() == 0,
          "last responder BUNDLE member retained the association");

    qInfo("Shared ICE/DTLS/secure-RTP BUNDLE runtime regression passed");
    return 0;
}
