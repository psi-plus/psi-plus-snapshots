// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <iris/jingle-rtp.h>
#include <iris/xmpp_client.h>
#include <qca.h>
#define private public
#include <iris/jingle-session.h>
#undef private

using namespace XMPP;
using namespace XMPP::Jingle;
namespace R = XMPP::Jingle::RTP;
static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
static void pump()
{
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}
static const QString transportNs = QStringLiteral("urn:iris:test:rtp-transport");
struct Counters {
    int  sessions = 0, endpoints = 0, configured = 0, stopped = 0, liveEndpoints = 0, hints = 0;
    bool configOk = true, offerOk = true, answerOk = true;
};
class Endpoint : public R::MediaEndpoint {
public:
    Endpoint(std::shared_ptr<Counters> c, QString media) : c(std::move(c)), media(std::move(media))
    {
        ++this->c->endpoints;
        ++this->c->liveEndpoints;
    }
    ~Endpoint() override { --c->liveEndpoints; }
    R::Description localOffer() const override
    {
        if (!c->offerOk)
            return {};
        R::Description d;
        d.media   = media;
        d.rtcpMux = true;
        R::PayloadType p;
        p.id        = media == "audio" ? 111 : 96;
        p.name      = media == "audio" ? "opus" : "VP8";
        p.clockrate = media == "audio" ? 48000 : 90000;
        p.channels  = media == "audio" ? 2 : 1;
        d.payloads.append(p);
        return d;
    }
    std::optional<R::Description> makeAnswer(const R::Description &offer) const override
    {
        return c->answerOk ? std::optional<R::Description>(offer) : std::nullopt;
    }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &local, const R::Description &remote) override
    {
        check(local.media == media && remote.media == media, "wrong content routed to endpoint");
        ++c->configured;
        return c->configOk;
    }
    void stop() override { ++c->stopped; }
    void advisory(const R::Description &hint) override
    {
        ++c->hints;
        check(hint.payloads.size() == 1 && !hint.payloads.first().clockrate,
              "partial hint became a replacement description");
    }
    std::shared_ptr<Counters> c;
    QString                   media;
};
class MediaSession : public R::MediaSession {
public:
    explicit MediaSession(std::shared_ptr<Counters> c) : c(std::move(c)) { ++this->c->sessions; }

    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return std::make_unique<Endpoint>(c, media);
    }

    bool attachSecureRtpPacketIo(ProtectedPacketWriter writer) override
    {
        protectedWriter_ = std::move(writer);
        return true;
    }
    void detachSecureRtpPacketIo() override { protectedWriter_ = {}; }
    bool configureSecureRtpEndpoints(const QList<R::SecureRtpEndpoint> &endpoints) override
    {
        secureEndpoints_ = endpoints;
        return true;
    }
    bool configureSecureRtpAssociation(const R::SecureRtpParameters &parameters) override
    {
        return parameters.isValid();
    }
    void invalidateSecureRtpAssociation(const QByteArray &, quint64) override { }
    bool receiveProtectedRtpPacket(const R::SecureRtpPacket &) override { return true; }

    std::shared_ptr<Counters> c;

private:
    ProtectedPacketWriter       protectedWriter_;
    QList<R::SecureRtpEndpoint> secureEndpoints_;
};
class Provider : public R::MediaProvider {
public:
    explicit Provider(std::shared_ptr<Counters> c) : c(std::move(c)) { }
    std::unique_ptr<R::MediaSession> createSession() override { return std::make_unique<MediaSession>(c); }
    QStringList secureRtpProfiles() const override
    {
        return { QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80") };
    }
    std::shared_ptr<Counters> c;
};
class TransportPad : public TransportManagerPad {
public:
    explicit TransportPad(Session *s) : s(s) { }
    Session          *session() const override { return s; }
    QString           ns() const override { return transportNs; }
    TransportManager *manager() const override { return nullptr; }
    Session          *s;
};
class TestTransportBase : public Transport {
public:
    TestTransportBase(Session *s, Origin creator) :
        Transport(TransportManagerPad::Ptr(new TransportPad(s)), creator)
    {
    }

    void prepare() override
    {
        setState(State::ApprovedToSend);
        emit updated();
    }
    void start() override
    {
        ++starts;
        setState(State::Active);
    }
    void stop() override
    {
        ++stops;
        Transport::stop();
        auto callback = std::move(onStop);
        onStop        = {};
        if (callback)
            callback();
    }
    bool update(const QDomElement &) override
    {
        setState(State::Accepted);
        return true;
    }
    bool hasUpdates() const override { return _state == State::ApprovedToSend; }
    OutgoingTransportInfoUpdate takeOutgoingUpdate(bool = false) override
    {
        auto xml = _pad->doc()->createElementNS(transportNs, "transport");
        return { xml, {} };
    }
    bool isValid() const override { return true; }
    TransportFeatures features() const override
    {
        return TransportFeature::LiveOriented | TransportFeature::MessageOriented;
    }
    Connection::Ptr addChannel(TransportFeatures, const QString &, int = -1) override { return {}; }
    QList<Connection::Ptr> channels() const override { return {}; }

    int                  starts = 0, stops = 0;
    std::function<void()> onStop;
};

class TestTransport final : public TestTransportBase, public R::PacketTransport {
public:
    TestTransport(Session *s, Origin creator) :
        TestTransportBase(s, creator),
        association_(nullptr, QByteArrayLiteral("rtpapplication-test"))
    {
    }

    bool enableRtpMux(const QStringList &profiles) override
    {
        secureEnabled_ = !profiles.isEmpty() && state() < State::Finishing;
        return secureEnabled_;
    }
    R::SecureRtpAssociation *rtpAssociation() const override
    {
        return secureEnabled_ && state() >= State::ApprovedToSend
            ? const_cast<R::SecureRtpAssociation *>(&association_)
            : nullptr;
    }
    bool sendProtectedRtpPacket(QByteArray, R::PacketKind, quint64) override { return false; }

private:
    mutable R::SecureRtpAssociation association_;
    bool                            secureEnabled_ = false;
};

class UnprotectedTransport final : public TestTransportBase {
public:
    using TestTransportBase::TestTransportBase;
};

struct OwnedXml {
    QDomDocument doc;
    QDomElement  root;

    operator QDomElement() const { return root; }
    QDomElement firstChildElement() const { return root.firstChildElement(); }
    QDomNode cloneNode(bool deep = true) const { return root.cloneNode(deep); }
};

static OwnedXml infoXml(const QString &body)
{
    OwnedXml xml;
    check(xml.doc.setContent("<jingle xmlns='urn:xmpp:jingle:1'>" + body + "</jingle>", true), "invalid test XML");
    xml.root = xml.doc.documentElement();
    return xml;
}

int main(int argc, char **argv)
{
    QCoreApplication eventLoop(argc, argv);
    QCA::Initializer qca;
    Client           client;
    auto             manager = client.jingleManager()->rtpManager();
    check(manager && manager->discoFeatures().isEmpty(), "unfinished RTP advertised");
    auto counters = std::make_shared<Counters>();
    {
        Session session(client.jingleManager(), Jid("peer@example.org/device"));
        check(!manager->createOutgoing(&session, "audio"), "RTP started without media provider");
        manager->setMediaProvider(std::make_shared<Provider>(counters));
        manager->setTransportNamespaces({ transportNs });
        auto audio = manager->createOutgoing(&session, "audio");
        auto video = manager->createOutgoing(&session, "video");
        check(audio && video && audio->contentName() != video->contentName(), "audio/video construction failed");
        check(audio->pad() == video->pad() && counters->sessions == 1 && counters->endpoints == 2,
              "audio/video do not share one media session");
        check(counters->configured == 0 && !audio->localDescription(), "offer creation touched media backend inline");
        check(!manager->createOutgoing(&session, "data"), "unsupported media accepted");
        check(audio->supportsContentModify(), "RTP cannot modify direction");
        audio->incomingContentModify(Origin::None);
        check(audio->senders() == Origin::None && counters->configured == 0, "direction change activated media");
        audio->incomingContentModify(Origin::Both);
        auto transport = QSharedPointer<TestTransport>::create(&session, Origin::Initiator);
        check(audio->setTransport(transport), "test transport rejected");
        audio->prepare();
        check(audio->evaluateOutgoingUpdate().action == Action::NoAction && !audio->localDescription(),
              "initial RTP offer became ready before backend completion");
        pump();
        check(audio->evaluateOutgoingUpdate().action == Action::ContentAdd, "initial RTP offer not scheduled");
        auto outgoing = audio->takeOutgoingUpdate();
        check(std::get<0>(outgoing).size() == 1
                  && !std::get<0>(outgoing).first().firstChildElement("description").isNull(),
              "RTP content offer missing description");
        check(audio->state() == State::Unacked, "offer did not enter unacked state");
        audio->setState(State::Pending); // emulate successful session-initiate IQ result
        QDomDocument doc;
        auto         accepted = doc.createElementNS(XMPP::Jingle::NS, "jingle");
        auto         content  = doc.createElementNS(XMPP::Jingle::NS, "content");
        content.setAttribute("creator", "initiator");
        content.setAttribute("name", audio->contentName());
        content.appendChild(audio->localDescription()->toXml(doc));
        content.appendChild(doc.createElementNS(transportNs, "transport"));
        accepted.appendChild(content);
        auto malformed = accepted.cloneNode(true).toElement();
        auto missing   = doc.createElementNS(XMPP::Jingle::NS, "content");
        missing.setAttribute("creator", "initiator");
        missing.setAttribute("name", "missing");
        malformed.appendChild(missing);
        check(!session.updateFromXml(Action::SessionAccept, malformed), "malformed batch accepted");
        check(audio->state() == State::Pending && !audio->remoteDescription(),
              "rejected batch retained committed RTP answer");
        check(session.updateFromXml(Action::SessionAccept, accepted), "RTP session-accept failed");
        check(counters->configured == 0 && transport->starts == 0, "RTP started before acceptance returned");
        pump();
        check(counters->configured == 1 && transport->starts == 1, "accepted RTP not configured");
        check(audio->state() == State::Connecting, "bare connectivity authorized protected media");
        auto rtpPad = qSharedPointerDynamicCast<R::Pad>(audio->pad());
        int  infos  = 0;
        QObject::connect(rtpPad.data(), &R::Pad::informationReceived, rtpPad.data(),
                         [&](const R::SessionInfo &) { ++infos; });
        const auto ringing = infoXml("<ringing xmlns='urn:xmpp:jingle:apps:rtp:info:1'/>");
        check(session.updateFromXml(Action::SessionInfo, ringing), "RTP auxiliary info namespace not routed");
        check(infos == 1 && session.allApplicationTypes().size() == 1, "RTP info created a second application pad");
        auto badInfo = ringing.cloneNode(true).toElement();
        badInfo.appendChild(doc.createElementNS("urn:unknown", "unknown"));
        check(!session.updateFromXml(Action::SessionInfo, badInfo) && infos == 1,
              "mixed unsupported info batch partially dispatched");
        const auto mute
            = infoXml(QStringLiteral("<mute xmlns='urn:xmpp:jingle:apps:rtp:info:1' creator='initiator' name='%1'/>")
                          .arg(audio->contentName()));
        check(session.updateFromXml(Action::SessionInfo, mute) && infos == 2, "named mute not routed");
        check(!session.updateFromXml(
                  Action::SessionInfo,
                  infoXml("<mute xmlns='urn:xmpp:jingle:apps:rtp:info:1' creator='initiator' name='missing'/>")),
              "mute for unknown content accepted");
        check(!session.updateFromXml(Action::SessionInfo,
                                     infoXml("<mute xmlns='urn:xmpp:jingle:apps:rtp:info:1' name='missing'/>")),
              "mute without creator accepted");
        check(audio->senders() == Origin::Both && counters->configured == 1,
              "informational event changed media negotiation");
        const auto hint
            = infoXml(QStringLiteral("<content creator='initiator' name='%1'>"
                                     "<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"
                                     "<payload-type id='111'><parameter name='minptime' value='20'/></payload-type>"
                                     "</description></content>")
                          .arg(audio->contentName()));
        check(session.updateFromXml(Action::DescriptionInfo, hint) && counters->hints == 1,
              "advisory hint not delivered");
        check(audio->remoteDescription()->payloads.first().clockrate == 48000,
              "advisory hint replaced the negotiated description");
        audio->start();
        pump();
        check(counters->configured == 1 && transport->starts == 1, "duplicate start repeated media configuration");
        video->remove();
        check(counters->stopped > 0 && audio->state() == State::Connecting, "removing video stopped audio");

        Session separate(client.jingleManager(), Jid("peer@example.org/device"));
        auto    other = manager->createOutgoing(&separate, "audio");
        check(other && other->pad() != audio->pad() && counters->sessions == 2,
              "separate calls share media session by peer JID");
        manager->setMediaProvider({});
        // Existing session retains its provider and media session.
        auto extra = manager->createOutgoing(&session, "video");
        check(extra, "provider removal invalidated existing media session");
        Session disabled(client.jingleManager(), Jid("peer@example.org/device"));
        check(!manager->createOutgoing(&disabled, "audio"), "removed provider created a new media session");
        manager->closeAll();
        check(audio->state() == State::Finishing && transport->stops > 0, "manager shutdown retained live transport");
    }
    check(counters->liveEndpoints == 0, "session teardown leaked endpoints");
    manager->setMediaProvider(std::make_shared<Provider>(counters));
    {
        Session incoming(client.jingleManager(), Jid("peer@example.org/device"), Origin::Responder);
        auto    pad = incoming.applicationPadFactory(R::Description::ns());
        std::unique_ptr<R::Application> audio(manager->startApplication(pad, "voice", Origin::Initiator, Origin::Both));
        QDomDocument                    doc;
        Endpoint                        fake(counters, "audio");
        check(audio && audio->setRemoteOffer(fake.localOffer().toXml(doc)) == XMPP::Jingle::Application::Ok,
              "incoming RTP offer rejected");
        check(!audio->localDescription(), "incoming RTP answer was prepared during stanza parsing");
        auto transport = QSharedPointer<TestTransport>::create(&incoming, Origin::Initiator);
        check(audio->setTransport(transport), "incoming test transport rejected");
        audio->prepare();
        check(audio->evaluateOutgoingUpdate().action == Action::NoAction && audio->makeLocalAnswer().isNull(),
              "incoming RTP answer became ready before backend completion");
        pump();
        check(audio->evaluateOutgoingUpdate().action == Action::ContentAccept, "incoming RTP answer not scheduled");
        check(!audio->makeLocalAnswer().isNull(), "local RTP answer empty");
        audio->takeOutgoingUpdate();
        audio->setState(State::Connecting); // emulate successful session-accept IQ result
        counters->configOk         = false;
        const int configuredBefore = counters->configured;
        audio->start();
        check(counters->configured == configuredBefore && transport->starts == 0,
              "media configuration ran inline from start");
        pump();
        check(audio->state() == State::Finishing && transport->starts == 0,
              "failed media configuration started transport");
        counters->configOk = true;
    }
    check(counters->liveEndpoints == 0, "incoming teardown leaked endpoint");
    for (auto kind : { R::SessionInfo::Kind::Active, R::SessionInfo::Kind::Hold, R::SessionInfo::Kind::Unhold,
                       R::SessionInfo::Kind::Mute, R::SessionInfo::Kind::Unmute, R::SessionInfo::Kind::Ringing }) {
        R::SessionInfo value;
        value.kind = kind;
        if (kind == R::SessionInfo::Kind::Mute || kind == R::SessionInfo::Kind::Unmute)
            value.creator = Origin::Initiator;
        QDomDocument doc;
        auto         parsed = R::SessionInfo::fromXml(value.toXml(doc));
        check(parsed && parsed->kind == kind && parsed->creator == value.creator, "RTP info roundtrip failed");
    }
    check(!R::SessionInfo::fromXml(
              infoXml("<active xmlns='urn:xmpp:jingle:apps:rtp:info:1' name='voice'/>").firstChildElement()),
          "active was scoped to one content");
    check(!R::SessionInfo::fromXml(
              infoXml("<ringing xmlns='urn:xmpp:jingle:apps:rtp:info:1'><unknown/></ringing>").firstChildElement()),
          "nested info payload accepted");
    {
        auto              disposable = new Session(client.jingleManager(), Jid("peer@example.org/device"));
        QPointer<Session> guard(disposable);
        auto              audio = manager->createOutgoing(disposable, "audio");
        auto              pad   = qSharedPointerDynamicCast<R::Pad>(audio->pad());
        QObject::connect(pad.data(), &R::Pad::informationReceived, pad.data(),
                         [=](const R::SessionInfo &) { delete disposable; });
        check(disposable->updateFromXml(Action::SessionInfo,
                                        infoXml("<ringing xmlns='urn:xmpp:jingle:apps:rtp:info:1'/>")),
              "info callback teardown failed");
        check(!guard && !pad->session(), "pad retained a destroyed session");
        pad.clear(); // custom deleter must not access a destroyed Session
    }
    {
        auto packetCounters = std::make_shared<Counters>();
        manager->setMediaProvider(std::make_shared<Provider>(packetCounters));
        Session session(client.jingleManager(), Jid("peer@example.org/device"));
        auto    audio       = manager->createOutgoing(&session, "audio");
        auto    unprotected = QSharedPointer<UnprotectedTransport>::create(&session, Origin::Initiator);
        check(audio && audio->setTransport(unprotected), "packet test setup failed");
        audio->prepare();
        check(audio->state() == State::Created, "packet preparation completed inline");
        pump();
        check(audio->state() >= State::Finishing && unprotected->starts == 0 && packetCounters->configured == 0,
              "packet backend accepted an unprotected transport");
        auto other          = manager->createOutgoing(&session, "audio");
        auto otherTransport = QSharedPointer<TestTransport>::create(&session, Origin::Initiator);
        check(other && other->setTransport(otherTransport), "mux-refusal fixture rejected transport");
        other->prepare();
        pump();
        check(bool(other->localDescription()), "mux-refusal fixture did not prepare local description");
        other->setState(State::Pending);
        auto answer    = *other->localDescription();
        answer.rtcpMux = false;
        QDomDocument doc;
        check(other->setRemoteAnswer(answer.toXml(doc)) == XMPP::Jingle::Application::IncompatibleParameters,
              "secure mux application accepted mux refusal");
    }
    {
        auto life = std::make_shared<Counters>();
        manager->setMediaProvider(std::make_shared<Provider>(life));
        Session session(client.jingleManager(), Jid("peer@example.org/device"));
        auto    pad  = session.applicationPadFactory(R::Description::ns());
        auto    make = [&]() {
            auto result = std::unique_ptr<R::Application>(
                manager->startApplication(pad, "lifecycle", Origin::Initiator, Origin::Both));
            check(result && result->initializeOutgoing("audio"), "lifecycle fixture failed");
            return result;
        };
        auto audio     = make();
        auto transport = QSharedPointer<TestTransport>::create(&session, Origin::Initiator);
        check(audio->setTransport(transport), "lifecycle transport rejected");
        audio->prepare();
        pump();
        audio->setState(State::Pending);
        QDomDocument doc;
        check(audio->setRemoteAnswer(audio->localDescription()->toXml(doc)) == XMPP::Jingle::Application::Ok,
              "lifecycle answer rejected");
        QObject::connect(audio.get(), &R::Application::stateChanged, &eventLoop, [&](State state) {
            if (state == State::Connecting)
                audio->remove();
        });
        audio->start();
        check(transport->starts == 0, "transport started before asynchronous media apply completed");
        pump();
        check(audio->state() >= State::Finishing && transport->starts == 0,
              "Connecting callback stopped media but transport still started");
        audio.reset();

        auto cancelled   = make();
        auto cancelledTr = QSharedPointer<TestTransport>::create(&session, Origin::Initiator);
        check(cancelled->setTransport(cancelledTr), "cancelled-apply fixture rejected transport");
        cancelled->prepare();
        pump();
        cancelled->setState(State::Pending);
        check(cancelled->setRemoteAnswer(cancelled->localDescription()->toXml(doc)) == XMPP::Jingle::Application::Ok,
              "cancelled-apply answer rejected");
        const int configuredBefore = life->configured;
        cancelled->start();
        cancelled->remove();
        pump();
        check(life->configured == configuredBefore && cancelledTr->starts == 0
                  && cancelled->state() >= State::Finishing,
              "cancelled asynchronous apply revived media");
        cancelled.reset();

        for (bool incoming : { false, true }) {
            auto disposable = make();
            auto tr         = QSharedPointer<TestTransport>::create(&session, Origin::Initiator);
            check(disposable->setTransport(tr), "destruction fixture rejected transport");
            QPointer<R::Application> guard(disposable.get());
            tr->onStop = [&]() { disposable.reset(); };
            if (incoming)
                disposable->incomingRemove(Reason(Reason::Success));
            else
                disposable->remove();
            check(!guard && !disposable, "transport stop did not exercise application destruction");
        }
    }
    {
        auto failures = std::make_shared<Counters>();
        manager->setMediaProvider(std::make_shared<Provider>(failures));
        Session outgoing(client.jingleManager(), Jid("peer@example.org/device"));
        failures->offerOk = false;
        auto failedOffer  = manager->createOutgoing(&outgoing, "audio");
        auto failedTr     = QSharedPointer<TestTransport>::create(&outgoing, Origin::Initiator);
        check(failedOffer && failedOffer->setTransport(failedTr), "failed-offer fixture rejected transport");
        failedOffer->prepare();
        pump();
        check(failedOffer->state() == State::Finishing
                  && failedOffer->evaluateOutgoingUpdate().action == Action::ContentRemove,
              "asynchronous local-offer failure did not terminate unsent content");
        failures->offerOk = true;

        Session incoming(client.jingleManager(), Jid("peer@example.org/device"), Origin::Responder);
        auto    pad = incoming.applicationPadFactory(R::Description::ns());
        std::unique_ptr<R::Application> failedAnswer(
            manager->startApplication(pad, "failed-answer", Origin::Initiator, Origin::Both));
        Endpoint fake(failures, "audio");
        failures->answerOk = false;
        QDomDocument doc;
        check(failedAnswer && failedAnswer->setRemoteOffer(fake.localOffer().toXml(doc)) == R::Application::Ok,
              "failed-answer fixture rejected remote offer too early");
        auto incomingTr = QSharedPointer<TestTransport>::create(&incoming, Origin::Initiator);
        check(failedAnswer->setTransport(incomingTr), "failed-answer fixture rejected transport");
        failedAnswer->prepare();
        pump();
        check(failedAnswer->state() == State::Finishing
                  && failedAnswer->evaluateOutgoingUpdate().action == Action::ContentReject,
              "asynchronous remote-offer failure did not reject unsent content");
        failures->answerOk = true;
    }
    qInfo("RTP application regressions passed");
}
