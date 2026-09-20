// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <iris/ice176.h>
#include <iris/jingle-rtp-srtp.h>
#include <iris/jingle-rtp.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>
#define private public
#include <iris/jingle-ice.h>
#undef private
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

using namespace XMPP;
namespace J = XMPP::Jingle;
static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
class Ack : public Task {
public:
    explicit Ack(Task *parent, bool ok = true) : Task(parent)
    {
        if (ok)
            setSuccess();
        else
            setError(500);
    }
};
struct MediaState {
    J::RTP::MediaEndpoint::PacketWriter writer;
    QList<QByteArray>                   received;
    int                                 configured = 0, attached = 0;
};
class Endpoint : public J::RTP::MediaEndpoint {
public:
    explicit Endpoint(std::shared_ptr<MediaState> state) : state(std::move(state)) { }
    J::RTP::Description localOffer() const override
    {
        J::RTP::Description d;
        d.media   = "audio";
        d.rtcpMux = true;
        J::RTP::PayloadType p;
        p.id        = 96;
        p.name      = "opus";
        p.clockrate = 48000;
        p.channels  = 2;
        d.payloads.append(p);
        return d;
    }
    std::optional<J::RTP::Description> makeAnswer(const J::RTP::Description &d) const override { return d; }
    bool acceptsAnswer(const J::RTP::Description &, const J::RTP::Description &) const override { return true; }
    bool configure(const J::RTP::Description &, const J::RTP::Description &) override
    {
        ++state->configured;
        return true;
    }
    bool supportsPacketIo() const override { return true; }
    bool attachPacketIo(PacketWriter writer) override
    {
        ++state->attached;
        state->writer = std::move(writer);
        return true;
    }
    void receivePacket(const QByteArray &packet, J::RTP::SrtpContext::Packet) override
    {
        state->received.append(packet);
    }
    void                        stop() override { state->writer = {}; }
    std::shared_ptr<MediaState> state;
};
class MediaSession : public J::RTP::MediaSession {
public:
    explicit MediaSession(std::shared_ptr<MediaState> state) : state(std::move(state)) { }
    std::unique_ptr<J::RTP::MediaEndpoint> createEndpoint(const QString &, const QString &) override
    {
        return std::make_unique<Endpoint>(state);
    }
    std::shared_ptr<MediaState> state;
};
class Provider : public J::RTP::MediaProvider {
public:
    explicit Provider(std::shared_ptr<MediaState> state) : state(std::move(state)) { }
    std::unique_ptr<J::RTP::MediaSession> createSession() override { return std::make_unique<MediaSession>(state); }
    std::shared_ptr<MediaState>           state;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    const bool       applicationMode = app.arguments().contains("--application");
    const bool       iceUdpMode      = app.arguments().contains("--ice-udp");
    const QString    transportNs     = iceUdpMode ? J::ICE::NS_ICE_UDP : J::ICE::NS;

    // A standalone Iris Client is allowed to omit TcpPortReserver entirely.
    // Creating an ICE pad must not dereference that optional integration hook.
    {
        Client     clientWithoutReserver;
        J::Session sessionWithoutReserver(clientWithoutReserver.jingleManager(),
                                          Jid("peer-no-reserver@example.test/device"),
                                          J::Origin::Initiator);
        auto *rawPad = clientWithoutReserver.jingleManager()->transportPad(&sessionWithoutReserver, transportNs);
        check(rawPad, "ICE pad creation without TcpPortReserver failed");
        J::TransportManagerPad::Ptr pad(rawPad);
        auto icePad = qSharedPointerDynamicCast<J::ICE::Pad>(pad);
        check(bool(icePad), "ICE pad without TcpPortReserver has wrong type");
        check(icePad->discoScope() == nullptr, "ICE pad unexpectedly fabricated a TCP discovery scope");
    }

    TcpPortReserver  reserver;
    Client           firstClient, secondClient;
    firstClient.setTcpPortReserver(&reserver);
    secondClient.setTcpPortReserver(&reserver);
    J::Session firstSession(firstClient.jingleManager(), Jid("second@example.test/device"), J::Origin::Initiator);
    J::Session secondSession(secondClient.jingleManager(), Jid("first@example.test/device"), J::Origin::Responder);
    firstClient.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    secondClient.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);

    auto makeTransport = [&](Client &client, J::Session &session, J::Origin creator) {
        auto rawPad = client.jingleManager()->transportPad(&session, transportNs);
        check(rawPad, "selected ICE namespace was not registered");
        J::TransportManagerPad::Ptr pad(rawPad);
        check(pad->ns() == transportNs, "selected ICE namespace was not retained by pad");
        auto base = pad->manager()->newTransport(pad, creator);
        auto ice  = qSharedPointerDynamicCast<J::ICE::Transport>(base);
        check(bool(ice), "selected ICE namespace did not create an ICE transport");
        return ice;
    };
    auto first     = makeTransport(firstClient, firstSession, J::Origin::Initiator);
    auto second    = makeTransport(secondClient, secondSession, J::Origin::Initiator);
    auto firstPad  = first->pad().staticCast<J::ICE::Pad>();
    auto secondPad = second->pad().staticCast<J::ICE::Pad>();

    // Transport construction itself must not allocate an ICE association.
    // Association selection is delayed until the Transport is already bound to
    // a Jingle content (or otherwise first used), which is required for BUNDLE.
    check(firstPad->liveAssociationCount() == 0 && secondPad->liveAssociationCount() == 0,
          "ICE association was allocated before content binding");

    std::unique_ptr<J::RTP::Application> firstApp, secondApp;
    auto firstMedia = std::make_shared<MediaState>(), secondMedia = std::make_shared<MediaState>();
    if (applicationMode) {
        auto makeApp = [&transportNs](Client &client, J::Session &session, std::shared_ptr<MediaState> state) {
            auto pad
                = QSharedPointer<J::RTP::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                      std::make_shared<Provider>(state), QStringList { transportNs });
            return std::make_unique<J::RTP::Application>(pad, "audio", J::Origin::Initiator, J::Origin::Both);
        };
        firstApp  = makeApp(firstClient, firstSession, firstMedia);
        secondApp = makeApp(secondClient, secondSession, secondMedia);
        check(firstApp->initializeOutgoing("audio"), "RTP application offer failed");
        check(firstApp->setTransport(first) && secondApp->setTransport(second), "RTP application rejected ICE");
    } else {
        check(first->enableRtpMux() && second->enableRtpMux(), "DTLS-SRTP backend required");
    }

    QEventLoop loop;
    QTimer     tick, deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool                offered = false, answered = false, started = false, sent = false;
    bool                received = false, replied = false, failed = false, applicationOfferDelivered = false;
    J::OutgoingUpdateCB delayedAck;
    bool                rejectionChecked = false;
    QElapsedTimer       rejectionWindow;
    auto                fail = [&]() {
        failed = true;
        loop.quit();
    };
    QObject::connect(first.data(), &J::Transport::failed, &loop, fail);
    QObject::connect(second.data(), &J::Transport::failed, &loop, fail);
    const auto rtp      = QByteArray::fromHex("806000010000000100000001") + QByteArrayLiteral("ICE media");
    const auto rtcp     = QByteArray::fromHex("80c9000100000002");
    auto       exchange = [&](J::ICE::Transport *from, J::ICE::Transport *to) {
        if (!from->hasUpdates())
            return false;
        auto [xml, ack] = from->takeOutgoingUpdate(false);
        check(!xml.isNull() && xml.namespaceURI() == transportNs, "wrong ICE wire namespace");
        if (iceUdpMode)
            check(xml.firstChildElement(QStringLiteral("gathering-complete")).isNull(),
                        "gathering-complete leaked into XEP-0176");
        check(to->update(xml), "ICE signaling update rejected");
        if (ack) {
            const bool reject = from == second.data() && !rejectionChecked && !delayedAck;
            Ack        result(from->pad()->session()->manager()->client()->rootTask(), !reject);
            ack(&result);
            if (reject)
                delayedAck = ack;
        }
        return true;
    };
    QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
        if (applicationMode && !applicationOfferDelivered && firstApp->localDescription()) {
            check(secondApp->setRemoteOffer(firstApp->makeLocalOffer()) == J::Application::Ok,
                  "RTP application offer rejected");
            applicationOfferDelivered = true;
        }
        offered = exchange(first.data(), second.data()) || offered;
        if (offered && second->state() == J::State::Pending) {
            if (applicationMode) {
                firstApp->setState(J::State::Pending);
                secondApp->setState(J::State::Pending);
                secondApp->prepare();
            } else
                second->prepare();
        }
        answered = exchange(second.data(), first.data()) || answered;
        if (answered && !started) {
            started = true;
            // update() consumes peer signaling in a queued callback.
            QTimer::singleShot(0, &loop, [&]() {
                if (applicationMode) {
                    check(firstApp->setRemoteAnswer(secondApp->makeLocalAnswer()) == J::Application::Ok,
                          "RTP application answer rejected");
                    secondApp->setState(J::State::Accepted);
                    firstApp->start();
                    secondApp->start();
                } else {
                    first->start();
                    second->start();
                }
            });
        }
        auto a = first->rtpSession(), b = second->rtpSession();
        if (delayedAck && a && b) {
            check(!a->isReady() && !b->isReady(), "failed fingerprint IQ started DTLS-SRTP");
            if (first->iceCanSendMedia() && second->iceCanSendMedia()) {
                if (!rejectionWindow.isValid())
                    rejectionWindow.start();
                if (rejectionWindow.elapsed() >= 500) {
                    rejectionChecked = true;
                    Ack  result(secondClient.rootTask());
                    auto callback = std::move(delayedAck);
                    delayedAck    = {};
                    callback(&result);
                }
            }
        }
        if (!sent && started && a && b && a->isReady() && b->isReady()) {
            sent = true;
            if (applicationMode) {
                check(firstApp->state() == J::State::Active && secondApp->state() == J::State::Active,
                      "authenticated media applications did not activate");
                check(firstMedia->configured == 1 && secondMedia->configured == 1 && firstMedia->attached == 1
                          && secondMedia->attached == 1,
                      "media attached more than once");
                check(firstMedia->writer(rtp, J::RTP::SrtpContext::Packet::Rtp), "media writer failed");
                return;
            }
            QObject::connect(b, &J::RTP::SrtpSession::packetReceived, &loop,
                             [&](const QByteArray &data, J::RTP::SrtpContext::Packet kind, quint64 epoch) {
                                 check(data == rtp && kind == J::RTP::SrtpContext::Packet::Rtp,
                                       "incorrect media over ICE");
                                 check(epoch == second->rtpSession()->epoch(), "stale received media epoch");
                                 received = true;
                                 check(second->sendRtpPacket(rtcp, J::RTP::SrtpContext::Packet::Rtcp, epoch),
                                       "ICE RTCP write rejected");
                             });
            QObject::connect(a, &J::RTP::SrtpSession::packetReceived, &loop,
                             [&](const QByteArray &data, J::RTP::SrtpContext::Packet kind, quint64) {
                                 replied = data == rtcp && kind == J::RTP::SrtpContext::Packet::Rtcp;
                                 loop.quit();
                             });
            check(first->sendRtpPacket(rtp, J::RTP::SrtpContext::Packet::Rtp, a->epoch()), "ICE RTP write rejected");
        }
        if (applicationMode && sent) {
            if (!received && !secondMedia->received.isEmpty()) {
                received = secondMedia->received.first() == rtp;
                check(received, "media backend received incorrect RTP");
                check(secondMedia->writer(rtcp, J::RTP::SrtpContext::Packet::Rtcp), "media RTCP writer failed");
            }
            if (!firstMedia->received.isEmpty()) {
                replied = firstMedia->received.first() == rtcp;
                loop.quit();
            }
        }
    });
    if (applicationMode)
        firstApp->prepare();
    else
        first->prepare();
    tick.start(5);
    deadline.start(10000);
    if (!failed)
        loop.exec();
    tick.stop();
    if (failed || !replied)
        qWarning() << "ICE test state" << int(first->state()) << int(second->state()) << offered << answered << started
                   << "applications" << (firstApp ? int(firstApp->state()) : -1)
                   << (secondApp ? int(secondApp->state()) : -1);
    check(!failed && rejectionChecked && received && replied, "ICE/DTLS/SRTP exchange failed or timed out");
    const auto                          epoch = first->rtpSession()->epoch();
    J::RTP::MediaEndpoint::PacketWriter retainedWriter;
    if (applicationMode) {
        retainedWriter = firstMedia->writer;
        firstApp->incomingContentModify(J::Origin::None);
        check(!retainedWriter(rtp, J::RTP::SrtpContext::Packet::Rtp), "senders=none allowed RTP");
        firstApp->incomingContentModify(J::Origin::Both);
        auto unknown = rtp;
        unknown[1]   = char(97);
        check(!retainedWriter(unknown, J::RTP::SrtpContext::Packet::Rtp), "unnegotiated payload sent");
        auto deliverAuthenticated = [&](QByteArray packet) {
            QEventLoop arrival;
            QTimer     timeout;
            timeout.setSingleShot(true);
            bool authenticated = false;
            QObject::connect(&timeout, &QTimer::timeout, &arrival, &QEventLoop::quit);
            QObject::connect(second->rtpSession(), &J::RTP::SrtpSession::packetReceived, &arrival,
                             [&](const QByteArray &bytes, J::RTP::SrtpContext::Packet kind, quint64) {
                                 check(bytes == packet && kind == J::RTP::SrtpContext::Packet::Rtp,
                                       "wrong authenticated probe");
                                 authenticated = true;
                                 arrival.quit();
                             });
            check(first->sendRtpPacket(packet, J::RTP::SrtpContext::Packet::Rtp, epoch), "probe did not enter SRTP");
            timeout.start(2000);
            if (!authenticated)
                arrival.exec();
            check(authenticated, "probe was not authenticated: filtering test is inconclusive");
        };
        const auto before = secondMedia->received.size();
        unknown[3]        = char(2);
        deliverAuthenticated(unknown);
        check(secondMedia->received.size() == before, "unnegotiated payload reached backend");
        secondApp->incomingContentModify(J::Origin::None);
        auto probe = rtp;
        probe[3]   = char(3);
        deliverAuthenticated(probe);
        check(secondMedia->received.size() == before, "senders=none delivered incoming RTP");
        secondApp->incomingContentModify(J::Origin::Both);
        probe[3] = char(4);
        deliverAuthenticated(probe);
        check(secondMedia->received.size() == before + 1 && secondMedia->received.last() == probe,
              "restoring senders did not restore permitted media");
    }
    first->stop();
    check(!first->rtpSession()->isReady(), "stopping ICE transport retained SRTP keys");
    check(!first->sendRtpPacket(rtp, J::RTP::SrtpContext::Packet::Rtp, epoch), "stopped ICE transport sent RTP");
    if (applicationMode) {
        check(!retainedWriter(rtp, J::RTP::SrtpContext::Packet::Rtp), "retained media writer survived transport stop");
        firstApp.reset();
        check(!retainedWriter(rtp, J::RTP::SrtpContext::Packet::Rtp),
              "retained media writer survived application deletion");
    }
    second->stop();
    qInfo() << "Loopback ICE/DTLS/SRTP integration passed for" << transportNs;
}
