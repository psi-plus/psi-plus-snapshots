// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp.h"
#include "dtls.h"
#include "jingle-nstransportslist.h"
#include "jingle-rtp-router_p.h"
#include "jingle-session.h"
#include <algorithm>

namespace XMPP::Jingle::RTP {
class Pad::RoutingPrivate {
public:
    struct SecurityIngress {
        QPointer<SrtpSession>                         security;
        BundleRouter                                  router;
        QMap<ContentKey, BundleRouter::Route>         routes;
        QMap<ContentKey, QPointer<Application>>       applications;
        QMetaObject::Connection                       packetConnection;
        QMetaObject::Connection                       destroyedConnection;
    };

    QHash<SrtpSession *, QSharedPointer<SecurityIngress>> ingresses;
    QHash<Application *, SrtpSession *>                   applicationSecurity;
};

static QList<BundleRouter::Route> routeList(const QMap<ContentKey, BundleRouter::Route> &routes)
{
    QList<BundleRouter::Route> result;
    result.reserve(routes.size());
    for (const auto &route : routes)
        result.append(route);
    return result;
}

Pad::Pad(Manager *manager, Session *session, std::shared_ptr<MediaProvider> provider, QStringList transports) :
    manager_(manager), session_(session), provider_(std::move(provider)), transports_(std::move(transports))
{
    directions_ = new DirectionController(this);
    routing_    = std::make_unique<RoutingPrivate>();
    if (provider_)
        media_ = provider_->createSession();
    if (media_)
        connect(media_.get(), &MediaSession::runtimeError, this, &Pad::mediaError);
}
Pad::~Pad()
{
    if (media_)
        media_->cancelAll();
}
QString             Pad::ns() const { return Description::ns(); }
Session            *Pad::session() const { return session_; }
ApplicationManager *Pad::manager() const { return manager_; }
MediaSession       *Pad::mediaSession() const { return media_.get(); }

bool Pad::bindPacketRoute(Application *application, SrtpSession *security, const Description &local,
                          const Description &remote)
{
    if (!application || !security || application->pad().data() != this || !session_)
        return false;

    const ContentKey key { application->contentName(), application->creator() };
    const bool localContent = application->creator() == session_->role();
    auto route = bundleRouteForDescriptions(key, localContent, local, remote);
    if (!route)
        return false;

    auto ingress = routing_->ingresses.value(security);
    const bool newIngress = !ingress;
    if (!ingress) {
        ingress = QSharedPointer<RoutingPrivate::SecurityIngress>::create();
        ingress->security = security;
    }

    auto candidate = ingress->routes;
    candidate.insert(key, *route);
    if (!ingress->router.configure(routeList(candidate)))
        return false;

    // Commit the target route before releasing an older security association so
    // reconfiguration cannot leave the application unrouted on validation error.
    auto oldSecurity = routing_->applicationSecurity.value(application, nullptr);
    if (oldSecurity && oldSecurity != security)
        unbindPacketRoute(application);

    ingress->routes       = std::move(candidate);
    ingress->applications.insert(key, application);
    routing_->applicationSecurity.insert(application, security);

    if (newIngress) {
        routing_->ingresses.insert(security, ingress);
        ingress->packetConnection = connect(
            security, &SrtpSession::packetReceived, this,
            [this, security](const QByteArray &bytes, SrtpContext::Packet kind, quint64 epoch) {
                auto ingress = routing_->ingresses.value(security);
                if (!ingress || ingress->security != security)
                    return;
                auto routed = ingress->router.routeIncoming(bytes, kind);
                if (!routed || !ingress->router.isCurrent(*routed))
                    return;

                if (routed->delivery == BundleRouter::Delivery::Content) {
                    auto application = ingress->applications.value(routed->content);
                    if (application)
                        application->receiveRoutedPacket(routed->data, routed->kind, epoch);
                    return;
                }

                // A compound RTCP packet spanning multiple BUNDLE contents needs
                // one group-level media ingress. The current psimedia API has no
                // such endpoint, so never duplicate it across per-content inputs.
                // Negotiated RTP BUNDLE can reach this path; fail closed until the
                // media API exposes a group-level RTCP ingress.
                qWarning("jingle-rtp: dropping shared RTCP until group media ingress is wired");
            });
        ingress->destroyedConnection = connect(security, &QObject::destroyed, this, [this, security]() {
            auto ingress = routing_->ingresses.take(security);
            if (!ingress)
                return;
            for (auto application : std::as_const(ingress->applications)) {
                if (application && routing_->applicationSecurity.value(application) == security)
                    routing_->applicationSecurity.remove(application);
            }
        });
    }
    return true;
}

void Pad::unbindPacketRoute(Application *application)
{
    if (!application)
        return;
    auto security = routing_->applicationSecurity.take(application);
    if (!security)
        return;
    auto ingress = routing_->ingresses.value(security);
    if (!ingress)
        return;

    const ContentKey key { application->contentName(), application->creator() };
    auto candidate = ingress->routes;
    candidate.remove(key);
    ingress->applications.remove(key);

    if (candidate.isEmpty()) {
        QObject::disconnect(ingress->packetConnection);
        QObject::disconnect(ingress->destroyedConnection);
        routing_->ingresses.remove(security);
        return;
    }

    if (!ingress->router.configure(routeList(candidate))) {
        // Removing a route from a previously valid table cannot introduce a
        // collision. Fail closed if that invariant is ever violated.
        ingress->router.reset();
        QObject::disconnect(ingress->packetConnection);
        QObject::disconnect(ingress->destroyedConnection);
        for (auto survivor : std::as_const(ingress->applications)) {
            if (survivor && routing_->applicationSecurity.value(survivor) == security)
                routing_->applicationSecurity.remove(survivor);
        }
        routing_->ingresses.remove(security);
        return;
    }
    ingress->routes = std::move(candidate);
}

bool Pad::registerOutgoingRtp(Application *application, const QByteArray &packet)
{
    if (!application || packet.size() < 12)
        return false;
    const auto *bytes = reinterpret_cast<const uchar *>(packet.constData());
    if ((bytes[0] >> 6) != 2)
        return false;
    const quint32 ssrc = (quint32(bytes[8]) << 24) | (quint32(bytes[9]) << 16) | (quint32(bytes[10]) << 8)
        | quint32(bytes[11]);
    if (!ssrc)
        return false;

    auto security = routing_->applicationSecurity.value(application, nullptr);
    auto ingress  = security ? routing_->ingresses.value(security) : QSharedPointer<RoutingPrivate::SecurityIngress>();
    if (!ingress)
        return false;
    return ingress->router.registerOutgoingSsrc(ContentKey { application->contentName(), application->creator() }, ssrc);
}
bool                Pad::incomingSessionInfo(const QDomElement &xml)
{
    if (!session_ || session_->state() >= State::Finishing)
        return false;
    QList<SessionInfo> infos;
    for (auto child = xml.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        auto info = SessionInfo::fromXml(child);
        if (!info)
            return false;
        if (!info->name.isEmpty()) {
            auto app = session_->content(info->name, info->creator);
            if (!app || app->pad().data() != this || app->state() >= State::Finishing)
                return false;
        }
        infos.append(*info);
    }
    if (infos.isEmpty())
        return false;
    QPointer<Pad> guard(this);
    for (const auto &info : infos) {
        if (!guard || !session_ || session_->state() >= State::Finishing)
            break;
        emit informationReceived(info);
    }
    return true;
}
QString Pad::generateContentName(Origin)
{
    QString name;
    do {
        name = QStringLiteral("rtp-%1").arg(++nextName_);
    } while (session_ && (session_->content(name, Origin::Initiator) || session_->content(name, Origin::Responder)));
    return name;
}

Application::Application(const QSharedPointer<Pad> &pad, const QString &name, Origin creator, Origin senders)
{
    _pad               = pad;
    _contentName       = name;
    _creator           = creator;
    _senders           = senders;
    _transportSelector = std::make_unique<NSTransportsList>(pad->session(), pad->transportNamespaces());
    connect(pad.data(), &Pad::mediaError, this, [this](const MediaError &error) {
        if (_state >= State::Finishing)
            return;
        remove(Reason::FailedApplication,
               error.text.isEmpty() ? QStringLiteral("Media backend failed during the call") : error.text);
    });
}
Application::~Application()
{
    stopMedia();
    if (_transport) {
        _transport->disconnect(this);
        _transport->stop();
    }
}
void Application::stopMedia()
{
    prepareOperation_.reset();
    applyOperation_.reset();
    pendingRemoteOffer_.reset();
    if (auto pad = _pad.staticCast<Pad>())
        pad->unbindPacketRoute(this);
    if (security_)
        security_->disconnect(this);
    security_.clear();
    attached_   = false;
    configured_ = false;
    negotiatedPayloads_.clear();
    if (endpoint_)
        endpoint_->stop();
}
void Application::setState(State state)
{
    if (_state == state)
        return;
    // Session rolls back accepted contents when a later content in the same
    // answer stanza is malformed. Restore the offer as well as the enum.
    if (state == State::Pending && _state == State::Accepted && beforeAnswer_) {
        negotiation_ = std::move(*beforeAnswer_);
        beforeAnswer_.reset();
    }
    _state = state;
    QPointer<Application> guard(this);
    if (state >= State::Finishing)
        stopMedia();
    if (guard)
        emit stateChanged(state);
}
Application::Update Application::evaluateOutgoingUpdate()
{
    auto result = XMPP::Jingle::Application::evaluateOutgoingUpdate();
    if (preparationFailed_ && isRemote() && result.action == Action::ContentRemove) {
        result.action = Action::ContentReject;
        _update       = result;
    }
    return result;
}
bool Application::initializeOutgoing(const QString &media)
{
    if (isRemote() || endpoint_ || _state != State::Created || (media != "audio" && media != "video"))
        return false;
    auto pad = _pad.staticCast<Pad>();
    if (!pad->mediaSession())
        return false;
    auto endpoint = pad->mediaSession()->createEndpoint(_contentName, media);
    if (!endpoint)
        return false;
    media_    = media;
    endpoint_ = std::move(endpoint);
    return true;
}
Application::SetDescError Application::setRemoteOffer(const QDomElement &xml)
{
    if (!isRemote() || endpoint_ || _state != State::Created)
        return IncompatibleParameters;
    auto offer = Description::fromXml(xml);
    if (!offer)
        return Unparsed;
    auto pad = _pad.staticCast<Pad>();
    if ((offer->media != "audio" && offer->media != "video") || !pad->mediaSession())
        return IncompatibleParameters;
    auto endpoint = pad->mediaSession()->createEndpoint(_contentName, offer->media);
    if (!endpoint)
        return IncompatibleParameters;
    if (endpoint->supportsPacketIo() && !offer->rtcpMux) {
        endpoint->stop();
        return IncompatibleParameters;
    }
    QDomDocument snapshotDoc;
    auto         snapshot = Description::fromXml(offer->toXml(snapshotDoc));
    if (!snapshot) {
        endpoint->stop();
        return Unparsed;
    }
    media_              = offer->media;
    pendingRemoteOffer_ = std::move(snapshot);
    endpoint_           = std::move(endpoint);
    return Ok;
}
Application::SetDescError Application::setRemoteAnswer(const QDomElement &xml)
{
    if (!endpoint_ || isRemote() || _state != State::Pending)
        return IncompatibleParameters;
    auto answer = Description::fromXml(xml);
    if (!answer)
        return Unparsed;
    if (endpoint_->supportsPacketIo() && !answer->rtcpMux)
        return IncompatibleParameters;
    auto candidate = negotiation_;
    auto result    = candidate.setRemoteAnswer(*answer, *endpoint_);
    if (result != Negotiation::Result::Ok)
        return result == Negotiation::Result::InvalidDescription ? Unparsed : IncompatibleParameters;
    beforeAnswer_ = negotiation_;
    negotiation_  = std::move(candidate);
    setState(State::Accepted);
    return Ok;
}
bool Application::incomingDescriptionInfo(const QDomElement &xml)
{
    if (!endpoint_ || _state >= State::Finishing)
        return false;
    auto hint  = Description::fromXml(xml, true);
    auto local = negotiation_.localDescription();
    if (!hint || !local || hint->media != local->media)
        return false;
    endpoint_->advisory(*hint);
    return true;
}
QDomElement Application::makeLocalOffer()
{
    auto description = negotiation_.localDescription();
    return description ? description->toXml(*_pad->doc()) : QDomElement();
}
QDomElement Application::makeLocalAnswer()
{
    return negotiation_.state() == Negotiation::State::Accepted ? makeLocalOffer() : QDomElement();
}
bool Application::isTransportReplaceEnabled() const
{
    // Coordinated replacement of a shared RTP connection is not implemented yet.
    return _state < State::Connecting;
}
void Application::prepare()
{
    if (!endpoint_ || prepareOperation_ || (_state != State::Created && !(isRemote() && _state == State::Pending)))
        return;
    if (!_transport && !selectNextTransport())
        return;
    auto pad   = _pad.staticCast<Pad>();
    auto media = pad->mediaSession();
    if (!media) {
        failPreparation(Reason::FailedApplication, QStringLiteral("Media session unavailable"));
        return;
    }
    QPointer<Application> guard(this);
    if (pendingRemoteOffer_) {
        prepareOperation_ = media->prepareAnswer(
            endpoint_.get(), *pendingRemoteOffer_,
            [guard](MediaOperation::Id id, std::optional<Description> result, MediaError error) mutable {
                if (guard)
                    guard->prepared(id, std::move(result), std::move(error));
            });
    } else {
        prepareOperation_ = media->prepareLocalOffer(
            endpoint_.get(),
            [guard](MediaOperation::Id id, std::optional<Description> result, MediaError error) mutable {
                if (guard)
                    guard->prepared(id, std::move(result), std::move(error));
            });
    }
    if (!prepareOperation_)
        failPreparation(Reason::FailedApplication, QStringLiteral("Media preparation could not be started"));
}
void Application::prepared(MediaOperation::Id id, std::optional<Description> description, MediaError error)
{
    if (!prepareOperation_ || prepareOperation_->id() != id || _state >= State::Finishing)
        return;
    prepareOperation_.reset();
    if (error || !description) {
        failPreparation(error.code == MediaError::Code::Unsupported ? Reason::IncompatibleParameters
                                                                    : Reason::FailedApplication,
                        error.text.isEmpty() ? QStringLiteral("Media preparation failed") : error.text);
        return;
    }
    if (description->media != media_ || (endpoint_->supportsPacketIo() && !description->rtcpMux)) {
        failPreparation(Reason::FailedApplication, QStringLiteral("Media backend returned incompatible parameters"));
        return;
    }

    Negotiation::Result result;
    if (pendingRemoteOffer_) {
        result = negotiation_.setRemoteOffer(*pendingRemoteOffer_, *description);
        pendingRemoteOffer_.reset();
    } else {
        result = negotiation_.setLocalOffer(*description);
    }
    if (result != Negotiation::Result::Ok) {
        failPreparation(Reason::FailedApplication, QStringLiteral("Media backend returned an invalid RTP description"));
        return;
    }

    QPointer<Application> guard(this);
    setState(State::ApprovedToSend);
    if (guard)
        prepareTransport();
}
void Application::failPreparation(Reason::Condition condition, const QString &text)
{
    if (_state >= State::Finishing)
        return;
    preparationFailed_ = true;
    reason_ = _terminationReason = Reason(condition, text);
    QPointer<Application> guard(this);
    stopMedia();
    if (!guard)
        return;
    auto transport = _transport;
    if (transport) {
        transport->disconnect(this);
        transport->stop();
    }
    if (!guard)
        return;
    setState(State::Finishing);
    if (guard)
        emit updated();
}
void Application::prepareTransport()
{
    if (!_transport)
        return;
    if (!endpoint_->supportsPacketIo()) {
        _transport->prepare();
        return;
    }
    if (security_)
        security_->disconnect(this);
    security_.clear();
    const auto local     = negotiation_.localDescription();
    const auto remote    = negotiation_.remoteDescription();
    auto       preparing = _transport;
    auto       packets   = dynamic_cast<PacketTransport *>(preparing.data());
    if (!local || !local->rtcpMux || (remote && !remote->rtcpMux) || !packets || !packets->enableRtpMux()) {
        remove(Reason::UnsupportedTransports, QStringLiteral("Authenticated RTP mux transport required"));
        return;
    }
    QPointer<Application> guard(this);
    preparing->prepare();
    if (!guard || _state >= State::Finishing || _transport != preparing)
        return;
    security_ = packets->rtpSession();
    if (!security_) {
        remove(Reason::SecurityError, QStringLiteral("RTP security binding unavailable"));
        return;
    }

    // A transport-replace can install the successor while the superseded
    // association is still alive (make-before-break). Its DTLS/SRTP callbacks
    // may therefore arrive before the queued prepareTransport() for the new
    // transport has disconnected security_. Bind every callback to the transport
    // incarnation that produced this security session so stale readiness or
    // teardown can never mutate the replacement application.
    const QPointer<Transport> securityTransport(preparing.data());
    connect(security_, &SrtpSession::ready, this, [this, securityTransport]() {
        if (securityTransport && _transport.data() == securityTransport)
            activateMedia();
    });
    connect(security_, &SrtpSession::invalidated, this, [this, securityTransport]() {
        if (securityTransport && _transport.data() == securityTransport)
            remove(Reason::SecurityError, QStringLiteral("RTP security association invalidated"));
    });
    connect(security_, &QObject::destroyed, this, [this, securityTransport]() {
        if (securityTransport && _transport.data() == securityTransport)
            remove(Reason::SecurityError, QStringLiteral("RTP security association destroyed"));
    });
}
void Application::start()
{
    if (!endpoint_ || !_transport || configured_ || applyOperation_ || _state >= State::Finishing
        || (_state != State::Accepted && _state != State::Connecting)
        || negotiation_.state() != Negotiation::State::Accepted)
        return;
    const auto local  = negotiation_.localDescription();
    const auto remote = negotiation_.remoteDescription();
    if (!local || !remote)
        return;
    auto pad   = _pad.staticCast<Pad>();
    auto media = pad->mediaSession();
    if (!media) {
        remove(Reason::FailedApplication, QStringLiteral("Media session unavailable"));
        return;
    }
    QPointer<Application> guard(this);
    applyOperation_ = media->applyNegotiation(endpoint_.get(), *local, *remote,
                                              [guard](MediaOperation::Id id, MediaError error) mutable {
                                                  if (guard)
                                                      guard->applied(id, std::move(error));
                                              });
    if (!applyOperation_)
        remove(Reason::FailedApplication, QStringLiteral("Media configuration could not be started"));
}
void Application::applied(MediaOperation::Id id, MediaError error)
{
    if (!applyOperation_ || applyOperation_->id() != id || _state >= State::Finishing)
        return;
    applyOperation_.reset();
    if (error) {
        remove(Reason::FailedApplication,
               error.text.isEmpty() ? QStringLiteral("Media configuration failed") : error.text);
        return;
    }
    const auto local  = negotiation_.localDescription();
    const auto remote = negotiation_.remoteDescription();
    if (!local || !remote) {
        remove(Reason::FailedApplication, QStringLiteral("Negotiated RTP description disappeared"));
        return;
    }
    configured_ = true;
    // Current negotiation retains offered PT identifiers, so the answer is the
    // accepted payload set in both directions, independent of offer preferences.
    for (const auto &payload : (isLocal() ? remote : local)->payloads)
        negotiatedPayloads_.insert(payload.id);
    if (endpoint_->supportsPacketIo()) {
        auto pad = _pad.staticCast<Pad>();
        if (!security_ || !pad || !pad->bindPacketRoute(this, security_, *local, *remote)) {
            remove(Reason::FailedApplication, QStringLiteral("Authenticated RTP route configuration failed"));
            return;
        }
    }
    beforeAnswer_.reset();
    auto                  transport = _transport;
    QPointer<Application> guard(this);
    if (_state == State::Accepted)
        setState(State::Connecting);
    if (guard && _state == State::Connecting && configured_ && _transport == transport)
        transport->start();
    if (guard)
        activateMedia();
}
bool Application::allowsRtp(bool sending) const
{
    if (sending && !_pad.staticCast<Pad>()->directionController()->allowsLocalSending(this))
        return false;
    const auto localRole = _pad->session()->role();
    const auto role = sending ? localRole : (localRole == Origin::Initiator ? Origin::Responder : Origin::Initiator);
    return _senders == Origin::Both || _senders == role;
}
void Application::activateMedia()
{
    if (!configured_ || attached_ || _state != State::Connecting || !security_ || !security_->isReady())
        return;
    const auto            epoch = security_->epoch();
    QPointer<Application> guard(this);
    const bool            ok = endpoint_->attachPacketIo([guard, epoch](QByteArray data, SrtpContext::Packet kind) {
        return guard && guard->sendPacket(std::move(data), kind, epoch);
    });
    if (!guard)
        return;
    if (!ok) {
        remove(Reason::FailedApplication, QStringLiteral("Media packet attachment failed"));
        return;
    }
    if (_state != State::Connecting || !security_ || !security_->isReady() || security_->epoch() != epoch)
        return;
    attached_ = true;
    setState(State::Active);
}
bool Application::sendPacket(QByteArray data, SrtpContext::Packet kind, quint64 epoch)
{
    if (_state != State::Active || !attached_ || !security_ || !security_->isReady() || epoch != security_->epoch())
        return false;
    if (kind == SrtpContext::Packet::Rtp) {
        if (!allowsRtp(true) || data.size() < 12 || !negotiatedPayloads_.contains(quint8(data[1]) & 0x7f))
            return false;
        auto pad = _pad.staticCast<Pad>();
        if (!pad || !pad->registerOutgoingRtp(this, data)) {
            remove(Reason::FailedApplication, QStringLiteral("Outgoing RTP source routing conflict"));
            return false;
        }
    }
    auto packets = dynamic_cast<PacketTransport *>(_transport.data());
    return packets && packets->sendRtpPacket(std::move(data), kind, epoch);
}

void Application::receiveRoutedPacket(const QByteArray &bytes, SrtpContext::Packet kind, quint64 epoch)
{
    if (_state != State::Active || !attached_ || !endpoint_ || !security_ || !security_->isReady()
        || epoch != security_->epoch())
        return;
    if (kind == SrtpContext::Packet::Rtp
        && (!allowsRtp(false) || bytes.size() < 12 || !negotiatedPayloads_.contains(quint8(bytes[1]) & 0x7f)))
        return;
    endpoint_->receivePacket(bytes, kind);
}
void Application::remove(Reason::Condition condition, const QString &text)
{
    if (_state >= State::Finishing || stopping_)
        return;
    stopping_             = true;
    const auto finalState = isLocal() && _state <= State::ApprovedToSend ? State::Finished : State::Finishing;
    reason_ = _terminationReason = Reason(condition, text);
    QPointer<Application> guard(this);
    stopMedia();
    if (!guard)
        return;
    auto transport = _transport;
    if (transport) {
        transport->disconnect(this);
        transport->stop();
    }
    if (!guard)
        return;
    stopping_ = false;
    setState(finalState);
    if (guard)
        emit updated();
}
void Application::incomingRemove(const Reason &reason)
{
    if (_state >= State::Finishing || stopping_)
        return;
    stopping_ = true;
    reason_   = reason;
    QPointer<Application> guard(this);
    stopMedia();
    if (!guard)
        return;
    auto transport = _transport;
    if (transport)
        transport->stop();
    if (!guard)
        return;
    stopping_ = false;
    setState(State::Finished);
}

Manager::Manager(QObject *parent) : ApplicationManager(parent)
{
    qRegisterMetaType<SessionInfo>();
    qRegisterMetaType<MediaError>();
}
Manager::~Manager() { closeAll(); }

QStringList Manager::discoFeatures() const
{
    if (!provider_ || !jingle_ || transports_.isEmpty() || supportedSecureRtpProfiles().isEmpty())
        return {};

    const auto packetTransports = jingle_->availableTransports(
        TransportFeature::Unreliable | TransportFeature::MessageOriented | TransportFeature::LiveOriented);
    bool hasUsableTransport = false;
    for (const auto &ns : transports_) {
        if (packetTransports.contains(ns)) {
            hasUsableTransport = true;
            break;
        }
    }
    if (!hasUsableTransport)
        return {};

    const auto  media = provider_->mediaTypes();
    QStringList features;
    if (media.contains(QStringLiteral("audio")) || media.contains(QStringLiteral("video")))
        features << Description::ns();
    if (media.contains(QStringLiteral("audio")))
        features << QStringLiteral("urn:xmpp:jingle:apps:rtp:audio");
    if (media.contains(QStringLiteral("video")))
        features << QStringLiteral("urn:xmpp:jingle:apps:rtp:video");
    return features;
}

void Manager::setJingleManager(XMPP::Jingle::Manager *manager)
{
    if (!manager)
        closeAll();
    jingle_ = manager;
}
void Manager::setMediaProvider(std::shared_ptr<MediaProvider> provider) { provider_ = std::move(provider); }
void Manager::setTransportNamespaces(const QStringList &transports) { transports_ = transports; }
ApplicationManagerPad *Manager::pad(Session *session)
{
    if (!provider_ || !session || session->manager() != jingle_)
        return nullptr;
    auto result = std::make_unique<Pad>(this, session, provider_, transports_);
    return result->mediaSession() ? result.release() : nullptr;
}
Application *Manager::startApplication(const ApplicationManagerPad::Ptr &base, const QString &name, Origin creator,
                                       Origin senders)
{
    auto pad = qSharedPointerDynamicCast<Pad>(base);
    if (!pad || pad->manager() != this || !pad->session() || !pad->mediaSession() || name.isEmpty()
        || (creator != Origin::Initiator && creator != Origin::Responder)
        || (senders != Origin::None && senders != Origin::Both && senders != Origin::Initiator
            && senders != Origin::Responder))
        return nullptr;
    auto app = new Application(pad, name, creator, senders);
    applications_.append(app);
    connect(app, &QObject::destroyed, this, [this] {
        applications_.erase(
            std::remove_if(applications_.begin(), applications_.end(), [](const auto &p) { return p.isNull(); }),
            applications_.end());
    });
    return app;
}
Application *Manager::createOutgoing(Session *session, const QString &media, Origin senders)
{
    if (!session || session->manager() != jingle_ || session->state() >= State::Finishing
        || (media != QLatin1String("audio") && media != QLatin1String("video")))
        return nullptr;

    // When XEP-0115/disco information is available, fail before constructing an
    // offer unless the peer advertises the complete secure RTP profile. Unknown
    // caps remain a compatibility case; the transport selector will still refuse
    // to send if no configured transport namespace is advertised.
    const auto peerFeatures = session->peerFeatures();
    if (!peerFeatures.isEmpty()) {
        const auto mediaFeature = QStringLiteral("urn:xmpp:jingle:apps:rtp:") + media;
        if (!peerFeatures.test(XMPP::Jingle::NS) || !peerFeatures.test(Description::ns())
            || !peerFeatures.test(mediaFeature) || !peerFeatures.test(Dtls::FingerPrint::ns()))
            return nullptr;
    }

    auto pad = session->applicationPadFactory(Description::ns());
    if (!pad)
        return nullptr;
    std::unique_ptr<Application> app(
        startApplication(pad, pad->generateContentName(senders), session->role(), senders));
    if (!app || !app->initializeOutgoing(media))
        return nullptr;
    // addContent can prepare immediately, notifying user code which may delete
    // the session. Transfer ownership before entering those callbacks.
    auto                  content = app.release();
    QPointer<Application> guard(content);
    session->addContent(content);
    return guard;
}
void Manager::closeAll(const QString &)
{
    const auto applications = applications_;
    for (const auto &app : applications)
        if (app)
            app->remove(Reason::Gone, QStringLiteral("RTP manager stopped"));
}
}
