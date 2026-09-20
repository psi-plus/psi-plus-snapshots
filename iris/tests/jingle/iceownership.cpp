#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QtCrypto>
#include <iris/dtls.h>
#include <iris/jingle-session.h>
#include <iris/jingle-transport.h>
#include <iris/xmpp_client.h>

#include <type_traits>

// Inspect the internal per-session registry without publishing a BUNDLE API.
#define private public
#include <iris/jingle-ice.h>
#undef private

using namespace XMPP;
using namespace XMPP::Jingle::ICE;

static_assert(!std::is_copy_constructible_v<ConnectionMembership>);
static_assert(!std::is_copy_assignable_v<ConnectionMembership>);
static_assert(std::is_move_constructible_v<ConnectionMembership>);
static_assert(std::is_move_assignable_v<ConnectionMembership>);

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

class TestApplicationPad : public Jingle::ApplicationManagerPad {
public:
    explicit TestApplicationPad(Jingle::Session *session) : session_(session) { }
    QString ns() const override { return QStringLiteral("urn:iris:test:ice-ownership"); }
    Jingle::Session *session() const override { return session_; }
    Jingle::ApplicationManager *manager() const override { return nullptr; }
    QString generateContentName(Jingle::Origin) override { return {}; }

private:
    Jingle::Session *session_ = nullptr;
};

class TestTransportSelector : public Jingle::TransportSelector {
public:
    QSharedPointer<Jingle::Transport> getNextTransport() override { return {}; }
    QSharedPointer<Jingle::Transport> getAlikeTransport(QSharedPointer<Jingle::Transport>) override { return {}; }
    bool replace(QSharedPointer<Jingle::Transport>, QSharedPointer<Jingle::Transport> newer) override
    {
        return bool(newer);
    }
    void backupTransport(QSharedPointer<Jingle::Transport>) override { }
    bool hasMoreTransports() const override { return false; }
    bool hasTransport(QSharedPointer<Jingle::Transport>) const override { return false; }
    int compare(QSharedPointer<Jingle::Transport>, QSharedPointer<Jingle::Transport>) const override { return 0; }
};

class TestIceTransport final : public Transport {
public:
    using Transport::Transport;
    void forceState(Jingle::State state) { setState(state); }
};

class TestApplication : public Jingle::Application {
public:
    TestApplication(const Jingle::ApplicationManagerPad::Ptr &pad, QString name, Jingle::Origin creator)
    {
        _pad               = pad;
        _contentName       = std::move(name);
        _creator           = creator;
        _senders           = Jingle::Origin::Both;
        _transportSelector = std::make_unique<TestTransportSelector>();
    }

    void setState(Jingle::State state) override { _state = state; }
    const std::optional<XMPP::Stanza::Error> &lastError() const override { return error_; }
    Jingle::Reason lastReason() const override { return reason_; }
    SetDescError setRemoteOffer(const QDomElement &) override { return Ok; }
    SetDescError setRemoteAnswer(const QDomElement &) override { return Ok; }
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    void prepare() override { }
    void start() override { }
    bool supportsSharedTransport() const override { return true; }
    void remove(Jingle::Reason::Condition = Jingle::Reason::Success, const QString & = QString()) override { }
    void incomingRemove(const Jingle::Reason &) override { }

protected:
    void prepareTransport() override { }

private:
    std::optional<XMPP::Stanza::Error> error_;
    Jingle::Reason                     reason_;
};

int main(int argc, char **argv)
{
    QCoreApplication        app(argc, argv);
    QCA::Initializer        qca;
    auto                    audio       = QSharedPointer<IceConnection>::create();
    auto                    video       = audio;
    auto                    independent = QSharedPointer<IceConnection>::create();
    QPointer<IceConnection> group(audio.data());
    QPointer<IceConnection> other(independent.data());
    check(!group->parent(), "shared connection has a competing QObject owner");

    Component component;
    component.dtls = new Dtls(group, QStringLiteral("local"), QStringLiteral("peer"));
    component.srtp = new Jingle::RTP::SrtpSession(component.dtls, group);
    QPointer<Jingle::RTP::SrtpSession> srtp(component.srtp);
    group->components.append(component);
    QPointer<Dtls> dtls(component.dtls);
    audio.reset();
    check(group && dtls, "releasing one membership destroyed shared resources");
    check(other && other != group, "independent connection was affected");
    video.reset();
    check(!group && !dtls && !srtp, "last membership retained connection or DTLS/SRTP resources");
    check(other, "group teardown destroyed an independent connection");
    independent.reset();
    check(!other, "independent connection leaked");

    ConnectionRegistry       registry;
    ConnectionRegistry       separateRegistry;
    const Jingle::ContentKey audioKey { QStringLiteral("audio"), Jingle::Origin::Initiator };
    const Jingle::ContentKey videoKey { QStringLiteral("video"), Jingle::Origin::Initiator };
    const Jingle::ContentKey screenKey { QStringLiteral("screen"), Jingle::Origin::Initiator };
    auto                     audioMembership = registry.create(audioKey);
    check(audioMembership && audioMembership.associationId() != 0 && audioMembership.content() == audioKey,
          "registry did not create the first membership");
    const auto associationId = audioMembership.associationId();
    check(registry.contains(associationId) && registry.liveAssociationCount() == 1,
          "registry lost its live association");
    check(!separateRegistry.attach(associationId, videoKey), "association id escaped its session-local registry");
    QPointer<IceConnection> associationGuard(audioMembership.connection());
    audioMembership.connection()->generation.iceGeneration = 7;
    audioMembership.connection()->generation.dtlsEpoch     = 11;
    const auto beforeAttach                                = audioMembership.generation();

    auto videoMembership = registry.attach(associationId, videoKey);
    check(videoMembership && videoMembership.connection() == audioMembership.connection()
              && videoMembership.membershipCount() == 2,
          "second member did not attach to the existing association");
    check(videoMembership.associationId() == associationId && videoMembership.content() == videoKey,
          "attached membership identity was not retained");
    check(videoMembership.generation().iceGeneration == beforeAttach.iceGeneration
              && videoMembership.generation().dtlsEpoch == beforeAttach.dtlsEpoch
              && videoMembership.generation().membershipRevision == beforeAttach.membershipRevision + 1,
          "membership attach changed the wrong association generation");
    check(!registry.attach(associationId, videoKey), "duplicate logical membership was accepted");

    auto screenMembership = registry.create(screenKey);
    check(screenMembership && screenMembership.associationId() != associationId && registry.liveAssociationCount() == 2,
          "independent association was not created");
    check(!registry.create(audioKey), "live content was duplicated into a new association");
    check(!registry.attach(screenMembership.associationId(), audioKey),
          "live content was attached to a second association");
    screenMembership.reset();
    check(registry.liveAssociationCount() == 1, "released independent association remained live");

    const auto callbackGeneration = videoMembership.generation();
    audioMembership.reset();
    check(associationGuard && videoMembership.membershipCount() == 1,
          "releasing one explicit membership destroyed the shared association");
    check(videoMembership.generation().membershipRevision == callbackGeneration.membershipRevision + 1,
          "membership release did not invalidate stale association state");
    ConnectionMembership movedMembership(std::move(videoMembership));
    check(!videoMembership && movedMembership, "moving membership duplicated or lost its strong share");
    movedMembership.reset();
    check(!associationGuard, "last explicit membership did not release the shared association");
    check(!registry.contains(associationId) && registry.liveAssociationCount() == 0,
          "weak registry retained a released association");
    registry.prune();

    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);
    Jingle::Session sessionA(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));
    Jingle::Session sessionB(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));

    auto builtinIce = client.jingleICEManager();
    check(builtinIce && builtinIce->ns().contains(NS) && builtinIce->ns().contains(NS_ICE_UDP),
          "built-in ICE manager did not register both wire profiles");
    auto modernProfile = sessionA.newOutgoingTransport(NS);
    auto udpProfile    = sessionA.newOutgoingTransport(NS_ICE_UDP);
    check(modernProfile && modernProfile->pad()->ns() == NS, "ice:0 profile lost its namespace");
    check(udpProfile && udpProfile->pad()->ns() == NS_ICE_UDP, "ice-udp:1 profile lost its namespace");
    check(!sessionA.newOutgoingTransport(QStringLiteral("urn:example:unsupported")),
          "unsupported transport namespace created a transport");

    Manager manager;
    auto    padA = Pad::Ptr::create(&manager, &sessionA);
    auto    padB = Pad::Ptr::create(&manager, &sessionB);

    // Bind three logical contents to ICE transports. Pad::membershipFor() is the
    // private production seam used by Transport::ensureNetwork(); exercising it
    // here keeps the test about session/content ownership rather than the removed
    // transport-addressed connection cache.
    auto appPadA = Jingle::ApplicationManagerPad::Ptr(new TestApplicationPad(&sessionA));
    auto appPadB = Jingle::ApplicationManagerPad::Ptr(new TestApplicationPad(&sessionB));

    auto firstApp   = new TestApplication(appPadA, QStringLiteral("first"), Jingle::Origin::Initiator);
    auto siblingApp = new TestApplication(appPadA, QStringLiteral("sibling"), Jingle::Origin::Initiator);
    auto separateApp = new TestApplication(appPadB, QStringLiteral("separate"), Jingle::Origin::Initiator);
    sessionA.addContent(firstApp);
    sessionA.addContent(siblingApp);
    sessionB.addContent(separateApp);

    auto first    = QSharedPointer<Transport>::create(padA, Jingle::Origin::Initiator);
    auto sibling  = QSharedPointer<Transport>::create(padA, Jingle::Origin::Initiator);
    auto separate = QSharedPointer<Transport>::create(padB, Jingle::Origin::Initiator);
    check(firstApp->setTransport(first) && siblingApp->setTransport(sibling) && separateApp->setTransport(separate),
          "test contents did not bind their ICE transports");

    bool firstBound = false, siblingBound = false, separateBound = false;
    auto firstMembership    = padA->membershipFor(first.data(), &firstBound);
    auto siblingMembership  = padA->membershipFor(sibling.data(), &siblingBound);
    auto separateMembership = padB->membershipFor(separate.data(), &separateBound);
    check(firstBound && siblingBound && separateBound && firstMembership && siblingMembership && separateMembership,
          "content-bound ICE membership was not created");
    check(firstMembership.connection() != siblingMembership.connection(), "unbundled contents shared a connection");
    check(firstMembership.connection() != separateMembership.connection(),
          "sessions to the same peer shared a connection");
    check(padA->liveAssociationCount() == 2 && padB->liveAssociationCount() == 1,
          "session-local association accounting is wrong");

    bool foreignBound = true;
    check(!padA->membershipFor(separate.data(), &foreignBound) && !foreignBound,
          "registry accepted another session's transport");
    bool nullBound = true;
    check(!padA->membershipFor(nullptr, &nullBound) && !nullBound, "registry accepted a null transport");

    if (!Jingle::RTP::SrtpContext::supportedProfiles().isEmpty() && !Dtls::supportedSRTPProfiles().isEmpty()) {
        // A transport used directly without a Jingle Application remains on the
        // explicit standalone compatibility path and must not consume a content
        // association in the Pad registry.
        const auto beforeStandalone = padB->liveAssociationCount();
        auto media = QSharedPointer<Transport>::create(padB, Jingle::Origin::Initiator);
        check(media->enableRtpMux(), "explicit secure RTP mode rejected");
        check(padB->liveAssociationCount() == beforeStandalone,
              "standalone secure RTP transport polluted the content registry");
        check(!media->rtpSession(), "SRTP binding created before DTLS configuration");
        check(!media->sendRtpPacket({}, Jingle::RTP::SrtpContext::Packet::Rtp, 0), "unprepared media sent");
        check(!media->addChannel(Jingle::TransportFeature::MessageOriented, "raw", 0),
              "secure RTP exposed raw channel");
        media->setComponentsCount(2);
        media->stop();
        check(!media->enableRtpMux(), "stopped transport reconfigured");

        auto insecure = QSharedPointer<Transport>::create(padB, Jingle::Origin::Responder);
        check(insecure->enableRtpMux(), "incoming secure RTP mode rejected");
        insecure->prepare();
        check(insecure->state() >= Jingle::State::Finishing, "incoming RTP without fingerprint accepted");
        check(!insecure->hasUpdates(), "insecure RTP transport exposed ICE signaling after security failure");
        check(padB->liveAssociationCount() == beforeStandalone,
              "failed standalone RTP transport polluted the content registry");
    }

    QPointer<IceConnection> released(firstMembership.connection());
    QPointer<IceConnection> surviving(siblingMembership.connection());
    firstMembership.reset();
    check(!released, "released unbundled membership retained its ICE association");
    check(surviving && padA->liveAssociationCount() == 1,
          "releasing one unbundled member changed the sibling association");

    first.reset();
    check(surviving && siblingMembership.connection() == surviving,
          "destroying a released transport changed the sibling association");

    siblingMembership.reset();
    check(!surviving && padA->liveAssociationCount() == 0,
          "last session-A membership retained its association");
    separateMembership.reset();
    check(padB->liveAssociationCount() == 0, "session-B membership retained its association");

    // A committed ICE payload is intentionally applied on the next event-loop
    // turn. If transport-replace hands this content to a successor first, the
    // superseded Transport must not use that late callback to recreate a
    // standalone association or advance its signaling state.
    auto lateApp = new TestApplication(appPadA, QStringLiteral("late"), Jingle::Origin::Initiator);
    sessionA.addContent(lateApp);
    auto retired = QSharedPointer<TestIceTransport>::create(padA, Jingle::Origin::Initiator);
    auto current = QSharedPointer<TestIceTransport>::create(padA, Jingle::Origin::Initiator);
    check(lateApp->setTransport(retired), "late-callback fixture rejected initial ICE transport");
    retired->setComponentsCount(1); // establish active content ownership in the Pad
    check(padA->liveAssociationCount() == 1, "late-callback fixture did not allocate initial association");
    retired->forceState(Jingle::State::Pending);

    QDomDocument staleDoc;
    auto staleUpdate = staleDoc.createElementNS(NS, QStringLiteral("transport"));
    staleUpdate.setAttribute(QStringLiteral("ufrag"), QStringLiteral("stale-ufrag"));
    staleUpdate.setAttribute(QStringLiteral("pwd"), QStringLiteral("stale-password"));
    auto preparedStale = retired->prepareUpdate(staleUpdate);
    check(preparedStale && retired->commitPreparedUpdate(std::move(preparedStale.update)),
          "late-callback fixture could not queue a valid ICE update");

    check(lateApp->setTransport(current), "late-callback fixture rejected replacement ICE transport");
    current->setComponentsCount(1); // retires the previous transport ownership
    check(padA->liveAssociationCount() == 1, "transport replacement leaked the retired association");
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    check(retired->state() == Jingle::State::Pending,
          "late ICE update advanced a superseded transport after ownership retirement");
    check(lateApp->transport() == current, "late ICE update displaced the current transport");
    delete lateApp;
    retired.reset();
    current.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    check(padA->liveAssociationCount() == 0, "late-callback fixture retained an association after cleanup");

    Jingle::Session bundleSession(client.jingleManager(), Jid(QStringLiteral("bundle@example.org/device")));
    auto bundlePad = Pad::Ptr::create(&manager, &bundleSession);
    auto bundleAppPad = Jingle::ApplicationManagerPad::Ptr(new TestApplicationPad(&bundleSession));
    auto audioApp = new TestApplication(bundleAppPad, QStringLiteral("audio"), Jingle::Origin::Initiator);
    auto videoApp = new TestApplication(bundleAppPad, QStringLiteral("video"), Jingle::Origin::Initiator);
    bundleSession.addContent(audioApp);
    bundleSession.addContent(videoApp);
    auto audioTransport = QSharedPointer<Transport>::create(bundlePad, Jingle::Origin::Initiator);
    auto videoTransport = QSharedPointer<Transport>::create(bundlePad, Jingle::Origin::Initiator);
    check(audioApp->setTransport(audioTransport) && videoApp->setTransport(videoTransport),
          "BUNDLE ownership fixture rejected transports");
    check(bundleSession.setGroupings({ Jingle::ContentGroup { QStringLiteral("BUNDLE"),
                                                              { QStringLiteral("audio"), QStringLiteral("video") } } }),
          "BUNDLE ownership fixture rejected grouping");

    bool audioBound = false, audioGrouped = false, videoBound = false, videoGrouped = false;
    auto sharedAudio = bundlePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioGrouped);
    auto sharedVideo = bundlePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoGrouped);
    check(audioBound && videoBound && audioGrouped && videoGrouped && sharedAudio && sharedAudio == sharedVideo,
          "explicit BUNDLE group did not stage one shared association");
    check(bundlePad->liveAssociationCount() == 1, "staged BUNDLE created more than one association");
    check(audioTransport->enableRtpMux() && videoTransport->enableRtpMux(),
          "staged BUNDLE transports did not accept shared RTP mux");
    check(sharedAudio->components.size() == 1, "shared RTP association created duplicate components");
    auto &sharedComponent = sharedAudio->components[0];
    sharedComponent.dtls = new Dtls(sharedAudio, QStringLiteral("local"), QStringLiteral("peer"));
    sharedComponent.srtp = new Jingle::RTP::SrtpSession(sharedComponent.dtls, sharedAudio);
    QPointer<Jingle::RTP::SrtpSession> sharedSrtp(sharedComponent.srtp);
    const auto sharedEpoch = sharedSrtp->epoch();
    int invalidations = 0;
    QObject::connect(sharedSrtp, &Jingle::RTP::SrtpSession::invalidated, &app, [&invalidations]() {
        ++invalidations;
    });
    check(audioTransport->rtpSession() == sharedSrtp && videoTransport->rtpSession() == sharedSrtp,
          "BUNDLE members did not expose the same SRTP association");

    QPointer<IceConnection> stagedGuard(sharedAudio);
    audioTransport->stop();
    check(sharedSrtp && invalidations == 0 && sharedSrtp->epoch() == sharedEpoch
              && videoTransport->rtpSession() == sharedSrtp,
          "stopping one BUNDLE member invalidated the shared SRTP association");
    delete audioApp;
    check(stagedGuard && sharedSrtp && bundlePad->liveAssociationCount() == 1,
          "removing one staged BUNDLE member destroyed the surviving association");
    delete videoApp;
    check(!stagedGuard && !sharedSrtp && bundlePad->liveAssociationCount() == 0,
          "last staged BUNDLE member retained its association");
    check(!audioTransport->rtpSession() && !videoTransport->rtpSession(),
          "live Transport retained a dangling BUNDLE association view");

    qInfo("ICE resource ownership regressions passed");
}
