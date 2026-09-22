// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp.h"
#include "dtls.h"
#include "jingle-nstransportslist.h"
#include "jingle-session.h"
#include <QDomDocument>
#include <QUuid>
#include <algorithm>

namespace XMPP::Jingle::RTP {
namespace {
QString mediaName(Media media)
{
    switch (media) {
    case Media::Audio:
        return QStringLiteral("audio");
    case Media::Video:
        return QStringLiteral("video");
    case Media::None:
        break;
    }
    return {};
}

Media mediaFromName(const QString &name)
{
    if (name == QLatin1String("audio"))
        return Media::Audio;
    if (name == QLatin1String("video"))
        return Media::Video;
    return Media::None;
}

QStringList commonSecureRtpProfiles(const MediaProvider *provider)
{
    if (!provider || !Dtls::isSupported())
        return {};

    auto       result       = provider->secureRtpProfiles();
    const auto dtlsProfiles = Dtls::supportedSRTPProfiles();
    for (auto it = result.begin(); it != result.end();) {
        if (!dtlsProfiles.contains(*it))
            it = result.erase(it);
        else
            ++it;
    }
    result.removeDuplicates();
    return result;
}
} // namespace
class Pad::RoutingPrivate {
public:
    struct AssociationBinding {
        QPointer<SecureRtpAssociation> association;
        quint64                        epoch = 0;
        QSet<Application *>            applications;
        QMetaObject::Connection        packetConnection;
        QMetaObject::Connection        readyConnection;
        QMetaObject::Connection        invalidatedConnection;
        QMetaObject::Connection        destroyedConnection;
    };

    QHash<QByteArray, QSharedPointer<AssociationBinding>> associations;
    QHash<Application *, SecureRtpEndpoint>               endpoints;
    QHash<Application *, QByteArray>                      applicationAssociations;
};

namespace {
QByteArray secureEndpointId(const Application *application)
{
    if (!application)
        return {};
    QByteArray result = QByteArray::number(int(application->creator()));
    result += ':';
    result += application->contentName().toUtf8();
    return result;
}

QList<SecureRtpEndpoint> secureEndpointList(const QHash<Application *, SecureRtpEndpoint> &endpoints)
{
    QList<SecureRtpEndpoint> result;
    result.reserve(endpoints.size());
    for (const auto &endpoint : endpoints)
        result.append(endpoint);
    return result;
}

void appendSecureSources(QSet<quint32> &target, const Description &description)
{
    if (description.ssrc && *description.ssrc)
        target.insert(*description.ssrc);
    for (const auto &source : description.sources)
        if (source.ssrc)
            target.insert(source.ssrc);
}

std::optional<SecureRtpEndpoint> secureEndpointForDescriptions(Application *application, Session *session,
                                                               const QByteArray &associationId,
                                                               const Description &local,
                                                               const Description &remote)
{
    if (!application || !session || associationId.isEmpty() || local.media.isEmpty() || remote.media.isEmpty()
        || local.media != remote.media || !local.rtcpMux || !remote.rtcpMux)
        return std::nullopt;

    const bool localContent = application->creator() == session->role();
    const auto &accepted    = localContent ? remote : local;
    if (accepted.payloads.isEmpty())
        return std::nullopt;

    SecureRtpEndpoint endpoint;
    endpoint.endpointId    = secureEndpointId(application);
    endpoint.associationId = associationId;
    endpoint.media         = local.media;
    for (const auto &payload : accepted.payloads)
        endpoint.incomingPayloadTypes.insert(payload.id);

    appendSecureSources(endpoint.incomingSsrcs, remote);
    appendSecureSources(endpoint.localSsrcs, local);

    constexpr auto MidUri = "urn:ietf:params:rtp-hdrext:sdes:mid";
    for (const auto &extension : accepted.headerExtensions) {
        if (extension.uri == QLatin1String(MidUri)) {
            endpoint.midExtensionId = extension.id;
            endpoint.mid            = application->contentName().toUtf8();
            break;
        }
    }

    return endpoint.isValid() ? std::optional<SecureRtpEndpoint>(std::move(endpoint)) : std::nullopt;
}
} // namespace

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
    if (media_) {
        if (securePacketIoAttached_)
            media_->detachSecureRtpPacketIo();
        media_->cancelAll();
    }
}

QString             Pad::ns() const { return Description::ns(); }
Session            *Pad::session() const { return session_; }
ApplicationManager *Pad::manager() const { return manager_; }
MediaSession       *Pad::mediaSession() const { return media_.get(); }

bool Pad::configureSecureAssociation(SecureRtpAssociation *association)
{
    if (!media_ || !association)
        return false;
    if (!association->isReady())
        return true;

    const auto &material = association->keyingMaterial();
    if (!material.isValid())
        return false;

    SecureRtpParameters parameters;
    parameters.associationId   = association->associationId();
    parameters.epoch           = association->epoch();
    parameters.profile         = material.profile;
    parameters.localMasterKey  = material.localMasterKey;
    parameters.localMasterSalt = material.localMasterSalt;
    parameters.remoteMasterKey = material.remoteMasterKey;
    parameters.remoteMasterSalt = material.remoteMasterSalt;
    return parameters.isValid() && media_->configureSecureRtpAssociation(parameters);
}

bool Pad::ensureSecurePacketIo()
{
    if (!media_)
        return false;
    if (securePacketIoAttached_)
        return true;
    securePacketIoAttached_ = media_->attachSecureRtpPacketIo([this](const SecureRtpPacket &packet) {
        return sendProtectedPacket(packet);
    });
    return securePacketIoAttached_;
}

QStringList Pad::secureRtpProfiles() const
{
    return commonSecureRtpProfiles(provider_.get());
}

bool Pad::bindSecureTransport(Application *application, SecureRtpAssociation *association,
                              const Description &local, const Description &remote)
{
    if (!application || !association || !media_ || application->pad().data() != this || !session_
        || !ensureSecurePacketIo())
        return false;

    auto endpoint = secureEndpointForDescriptions(application, session_, association->associationId(), local, remote);
    if (!endpoint)
        return false;

    const auto newId = association->associationId();
    const auto associationEpoch = association->epoch();
    const auto existingBinding = routing_->associations.value(newId);
    if (existingBinding && existingBinding->association != association)
        return false;
    const bool hadBinding = bool(existingBinding);
    if (!configureSecureAssociation(association))
        return false;

    auto candidate = routing_->endpoints;
    candidate.insert(application, *endpoint);
    if (!media_->configureSecureRtpEndpoints(secureEndpointList(candidate))) {
        // Key export and route-table update form one logical transaction for a
        // newly introduced association. Do not leave unreferenced key material
        // staged in the backend if the route half is rejected. An already bound
        // BUNDLE association belongs to surviving contents and must stay alive.
        if (!hadBinding)
            media_->invalidateSecureRtpAssociation(newId, associationEpoch);
        return false;
    }

    const auto oldId = routing_->applicationAssociations.value(application);
    if (!oldId.isEmpty() && oldId != newId) {
        auto oldBinding = routing_->associations.value(oldId);
        if (oldBinding) {
            oldBinding->applications.remove(application);
            if (oldBinding->applications.isEmpty()) {
                QObject::disconnect(oldBinding->packetConnection);
                QObject::disconnect(oldBinding->readyConnection);
                QObject::disconnect(oldBinding->invalidatedConnection);
                QObject::disconnect(oldBinding->destroyedConnection);
                if (oldBinding->epoch)
                    media_->invalidateSecureRtpAssociation(oldId, oldBinding->epoch);
                routing_->associations.remove(oldId);
            }
        }
    }

    routing_->endpoints               = std::move(candidate);
    routing_->applicationAssociations.insert(application, newId);

    auto binding = routing_->associations.value(newId);
    if (!binding) {
        binding = QSharedPointer<RoutingPrivate::AssociationBinding>::create();
        binding->association = association;
        binding->epoch       = associationEpoch;
        routing_->associations.insert(newId, binding);

        binding->packetConnection = connect(
            association, &SecureRtpAssociation::protectedPacketReceived, this,
            [this, association](const QByteArray &data, PacketKind kind, quint64 epoch) {
                if (!media_ || !association || !association->isReady() || association->epoch() != epoch)
                    return;
                SecureRtpPacket packet;
                packet.associationId = association->associationId();
                packet.epoch         = epoch;
                packet.data          = data;
                packet.kind          = kind;
                media_->receiveProtectedRtpPacket(packet);
            });

        binding->readyConnection = connect(
            association, &SecureRtpAssociation::ready, this,
            [this, association](quint64 epoch) {
                if (!association || association->epoch() != epoch || !configureSecureAssociation(association)) {
                    emit mediaError({ MediaError::Code::Backend,
                                      QStringLiteral("Secure RTP association activation failed") });
                    return;
                }
                const auto binding = routing_->associations.value(association->associationId());
                if (!binding)
                    return;
                binding->epoch = epoch;
                const auto applications = binding->applications.values();
                for (auto application : applications)
                    if (application)
                        application->activateMedia();
            });

        binding->invalidatedConnection = connect(
            association, &SecureRtpAssociation::invalidated, this,
            [this, association](quint64 epoch) {
                const auto id = association ? association->associationId() : QByteArray();
                if (media_ && !id.isEmpty())
                    media_->invalidateSecureRtpAssociation(id, epoch);
                const auto binding = routing_->associations.value(id);
                if (!binding)
                    return;
                const auto applications = binding->applications.values();
                for (auto application : applications) {
                    if (application && application->state() < State::Finishing)
                        application->remove(Reason::SecurityError,
                                            QStringLiteral("RTP security association invalidated"));
                }
            });

        binding->destroyedConnection = connect(association, &QObject::destroyed, this, [this, newId]() {
            const auto binding = routing_->associations.take(newId);
            if (!binding)
                return;
            // QObject::destroyed may arrive without SecureRtpAssociation::invalidated.
            // The association object can no longer tell us its epoch here, so keep
            // the last successfully exported epoch in the binding and explicitly
            // wipe the media backend before removing the contents.
            if (media_ && binding->epoch)
                media_->invalidateSecureRtpAssociation(newId, binding->epoch);
            const auto applications = binding->applications.values();
            for (auto application : applications) {
                if (application && application->state() < State::Finishing)
                    application->remove(Reason::SecurityError,
                                        QStringLiteral("RTP security association destroyed"));
            }
        });
    } else if (binding->association != association) {
        return false;
    }

    binding->applications.insert(application);
    return true;
}

void Pad::unbindSecureTransport(Application *application)
{
    if (!application || !routing_->endpoints.contains(application))
        return;

    auto candidate = routing_->endpoints;
    candidate.remove(application);
    if (media_ && !media_->configureSecureRtpEndpoints(secureEndpointList(candidate))) {
        emit mediaError({ MediaError::Code::Backend,
                          QStringLiteral("Secure RTP route teardown failed") });
    }
    routing_->endpoints = std::move(candidate);

    const auto id = routing_->applicationAssociations.take(application);
    auto binding = routing_->associations.value(id);
    if (!binding)
        return;

    binding->applications.remove(application);
    if (!binding->applications.isEmpty())
        return;

    QObject::disconnect(binding->packetConnection);
    QObject::disconnect(binding->readyConnection);
    QObject::disconnect(binding->invalidatedConnection);
    QObject::disconnect(binding->destroyedConnection);
    if (media_ && binding->epoch)
        media_->invalidateSecureRtpAssociation(id, binding->epoch);
    routing_->associations.remove(id);
}

bool Pad::sendProtectedPacket(const SecureRtpPacket &packet)
{
    const auto binding = routing_->associations.value(packet.associationId);
    if (!binding || !binding->association || !binding->association->isReady()
        || binding->association->epoch() != packet.epoch)
        return false;

    const auto applications = binding->applications.values();
    for (auto application : applications) {
        if (!application || application->state() < State::Connecting || application->state() >= State::Finishing)
            continue;
        auto packets = dynamic_cast<PacketTransport *>(application->_transport.data());
        if (!packets || packets->rtpAssociation() != binding->association)
            continue;
        if (packets->sendProtectedRtpPacket(packet.data, packet.kind, packet.epoch))
            return true;
    }
    return false;
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
        pad->unbindSecureTransport(this);
    if (association_)
        association_->disconnect(this);
    association_.clear();
    secureBound_ = false;
    configured_  = false;
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
    if (!offer->rtcpMux) {
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
    if (!answer->rtcpMux)
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
    if (_state < State::Connecting)
        return true;
    if (_state != State::Connecting)
        return false;

    // Connecting is not an established call: ICE/DTLS may still fail before
    // the secure association ever becomes ready. Allow ordinary per-content
    // fallback in that state. A negotiated multi-content BUNDLE still needs a
    // coordinated full-group replacement, so keep that case behind the
    // existing active/shared-migration gate.
    auto pad = _pad.staticCast<Pad>();
    auto session = pad ? pad->session() : nullptr;
    if (!session)
        return false;
    const auto groups = session->role() == Origin::Initiator ? session->remoteGroupings() : session->groupings();
    for (const auto &group : groups) {
        if (group.semantics == QLatin1String("BUNDLE") && group.contents.size() > 1
            && group.contents.contains(_contentName))
            return false;
    }
    return true;
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
    if (description->media != media_ || !description->rtcpMux) {
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

    if (association_)
        association_->disconnect(this);
    association_.clear();

    const auto local  = negotiation_.localDescription();
    const auto remote = negotiation_.remoteDescription();
    auto       pad       = _pad.staticCast<Pad>();
    auto       preparing = _transport;
    auto       packets   = dynamic_cast<PacketTransport *>(preparing.data());
    const auto profiles  = pad ? pad->secureRtpProfiles() : QStringList {};

    if (!local || !local->rtcpMux || (remote && !remote->rtcpMux) || profiles.isEmpty() || !packets
        || !packets->enableRtpMux(profiles)) {
        remove(Reason::UnsupportedTransports, QStringLiteral("Authenticated RTP mux transport required"));
        return;
    }

    QPointer<Application> guard(this);
    preparing->prepare();
    if (!guard || _state >= State::Finishing || _transport != preparing)
        return;

    association_ = packets->rtpAssociation();
    if (!association_) {
        remove(Reason::SecurityError, QStringLiteral("RTP security association unavailable"));
        return;
    }

    // On a pre-connected transport fallback the RTP codec/media negotiation is
    // already configured. Move its secure endpoint binding to the replacement
    // association now; no codec renegotiation is needed. bindSecureTransport()
    // atomically retires the old per-content association binding.
    if (configured_) {
        if (!remote || !pad || !pad->bindSecureTransport(this, association_, *local, *remote)) {
            remove(Reason::FailedApplication, QStringLiteral("Secure RTP replacement binding failed"));
            return;
        }
        secureBound_ = true;
    }

    const QPointer<Transport> securityTransport(preparing.data());
    connect(association_, &SecureRtpAssociation::ready, this,
            [this, securityTransport](quint64) {
                if (securityTransport && _transport.data() == securityTransport)
                    activateMedia();
            });
    connect(association_, &SecureRtpAssociation::invalidated, this,
            [this, securityTransport](quint64) {
                if (securityTransport && _transport.data() == securityTransport)
                    remove(Reason::SecurityError, QStringLiteral("RTP security association invalidated"));
            });
    connect(association_, &QObject::destroyed, this, [this, securityTransport]() {
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
    auto       pad    = _pad.staticCast<Pad>();
    if (!local || !remote || !pad || !association_) {
        remove(Reason::FailedApplication, QStringLiteral("Negotiated secure RTP state disappeared"));
        return;
    }

    configured_ = true;
    if (!pad->bindSecureTransport(this, association_, *local, *remote)) {
        remove(Reason::FailedApplication, QStringLiteral("Secure RTP route configuration failed"));
        return;
    }
    secureBound_ = true;

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
    auto pad = _pad.staticCast<Pad>();
    if (!pad || !pad->session())
        return false;
    if (sending && !pad->directionController()->allowsLocalSending(this))
        return false;
    const auto localRole = pad->session()->role();
    const auto role = sending ? localRole
                              : (localRole == Origin::Initiator ? Origin::Responder : Origin::Initiator);
    return _senders == Origin::Both || _senders == role;
}

void Application::activateMedia()
{
    if (!configured_ || !secureBound_ || _state != State::Connecting || !association_ || !association_->isReady())
        return;
    setState(State::Active);
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

std::optional<std::any> Manager::parseProposal(const QDomElement &element) const
{
    const auto name = element.localName().isEmpty()
        ? element.tagName().section(QLatin1Char(':'), -1)
        : element.localName();
    if (name != QLatin1String("description") || element.namespaceURI() != Description::ns())
        return std::nullopt;

    Proposal proposal { mediaFromName(element.attribute(QStringLiteral("media"))) };
    if (!proposal.isValid())
        return std::nullopt;
    return std::any(std::move(proposal));
}

QDomElement Manager::serializeProposal(const std::any &data, QDomDocument *document) const
{
    if (!document || data.type() != typeid(Proposal))
        return {};

    const auto &proposal = std::any_cast<const Proposal &>(data);
    if (!proposal.isValid())
        return {};

    const auto media = mediaName(proposal.media);
    if (media.isEmpty())
        return {};

    auto element = document->createElementNS(Description::ns(), QStringLiteral("description"));
    element.setAttribute(QStringLiteral("media"), media);
    return element;
}

QStringList Manager::secureRtpProfiles() const
{
    return commonSecureRtpProfiles(provider_.get());
}

QStringList Manager::discoFeatures() const
{
    if (!provider_ || !jingle_ || transports_.isEmpty() || secureRtpProfiles().isEmpty())
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
    if (media.contains(QStringLiteral("audio")) || media.contains(QStringLiteral("video"))) {
        features << Description::ns();
        // XEP-0293 requires this disco feature when the RTP implementation can
        // negotiate AVPF feedback. Individual endpoints still select only the
        // feedback types they actually support.
        features << QStringLiteral("urn:xmpp:jingle:apps:rtp:rtcp-fb:0");
    }
    if (media.contains(QStringLiteral("audio")))
        features << QStringLiteral("urn:xmpp:jingle:apps:rtp:audio");
    if (media.contains(QStringLiteral("video")))
        features << QStringLiteral("urn:xmpp:jingle:apps:rtp:video");
    return features;
}

void Manager::setJingleManager(XMPP::Jingle::Manager *manager)
{
    if (jingle_ == manager)
        return;

    if (jmiConnection_)
        disconnect(jmiConnection_);
    jmiConnection_ = {};

    if (!manager)
        closeAll();
    jingle_ = manager;

    if (!jingle_)
        return;

    jmiConnection_ = connect(
        jingle_, &XMPP::Jingle::Manager::incomingMessageInitiation, this,
        [this](const XMPP::Message &message, const MessageInitiation &initiation) {
            if (initiation.action() != MessageInitiation::Action::Propose)
                return;

            MediaSet media;
            for (const auto &description : initiation.descriptions()) {
                if (description.applicationNamespace != Description::ns() || !description.isSupported()
                    || description.data.type() != typeid(Proposal))
                    return;

                const auto &proposal = std::any_cast<const Proposal &>(description.data);
                if (!proposal.isValid() || media.testFlag(proposal.media))
                    return;
                media |= proposal.media;
            }

            if (media != MediaSet())
                emit incomingProposal(message, initiation.id(), media);
        });
}
void Manager::setMediaProvider(std::shared_ptr<MediaProvider> provider) { provider_ = std::move(provider); }
void Manager::setTransportNamespaces(const QStringList &transports) { transports_ = transports; }

QString Manager::propose(const Jid &peer, MediaSet media)
{
    if (!jingle_ || !jingle_->messageInitiationEnabled() || !peer.isValid() || media == MediaSet())
        return {};

    const auto features = discoFeatures();
    if (media.testFlag(Media::Audio)
        && !features.contains(QStringLiteral("urn:xmpp:jingle:apps:rtp:audio")))
        return {};
    if (media.testFlag(Media::Video)
        && !features.contains(QStringLiteral("urn:xmpp:jingle:apps:rtp:video")))
        return {};

    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    MessageInitiation initiation(MessageInitiation::Action::Propose, id);
    if (media.testFlag(Media::Audio))
        initiation.addDescription(Description::ns(), Proposal { Media::Audio });
    if (media.testFlag(Media::Video))
        initiation.addDescription(Description::ns(), Proposal { Media::Video });

    if (!jingle_->sendMessageInitiation(Jid(peer.bare()), initiation))
        return {};
    return id;
}

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
Application *Manager::createOutgoing(Session *session, Media media, Origin senders)
{
    const auto name = mediaName(media);
    return name.isEmpty() ? nullptr : createOutgoing(session, name, senders);
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
