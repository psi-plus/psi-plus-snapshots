/*
 * jingle-pub.cpp - XEP-0358 Publishing Available Jingle Sessions
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "jingle-pub.h"

#include "jingle-session.h"
#include "jingle.h"
#include "xmpp_client.h"
#include "xmpp_stream.h"
#include "xmpp_task.h"
#include "xmpp_xmlcommon.h"

#include <QDomDocument>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <iterator>
#include <utility>

namespace XMPP::Jingle {

const QString JINGLEPUB_NS = QStringLiteral("urn:xmpp:jinglepub:1");

class JinglePub::Private : public QSharedData {
public:
    Jid                from;
    QString            id;
    QUrl               uri;
    QList<Meta>        meta;
    QDomDocument       descriptionsDocument;
    QList<QDomElement> descriptions;
};

JinglePub::JinglePub() { }
JinglePub::JinglePub(const QDomElement &element) { fromXml(element); }
JinglePub::JinglePub(const JinglePub &)            = default;
JinglePub &JinglePub::operator=(const JinglePub &) = default;
JinglePub::~JinglePub()                            = default;

JinglePub::Private *JinglePub::ensureD()
{
    if (!d)
        d = new Private;
    return d.data();
}

bool JinglePub::isValid() const
{
    if (!d || !d->from.isValid() || d->id.isEmpty() || d->descriptions.isEmpty()
        || (!d->uri.isEmpty() && !d->uri.isValid()))
        return false;
    QSet<QString> languages;
    for (const auto &meta : d->meta) {
        if (meta.title.isEmpty() || languages.contains(meta.language))
            return false;
        languages.insert(meta.language);
    }
    for (const auto &description : d->descriptions) {
        if (description.isNull() || description.localName() != QLatin1String("description")
            || description.namespaceURI().isEmpty())
            return false;
    }
    return true;
}

Jid     JinglePub::from() const { return d ? d->from : Jid(); }
void    JinglePub::setFrom(const Jid &from) { ensureD()->from = from; }
QString JinglePub::id() const { return d ? d->id : QString(); }
void    JinglePub::setId(const QString &id) { ensureD()->id = id; }
QUrl    JinglePub::uri() const { return d ? d->uri : QUrl(); }
void    JinglePub::setUri(const QUrl &uri) { ensureD()->uri = uri; }

QString JinglePub::startUri() const
{
    if (!d || !d->from.isValid() || d->id.isEmpty())
        return {};
    const auto encodedJid = QUrl::toPercentEncoding(d->from.full(), QByteArray("@/"));
    const auto encodedId  = QUrl::toPercentEncoding(d->id);
    return QStringLiteral("xmpp:%1?jingle;id=%2").arg(QString::fromLatin1(encodedJid), QString::fromLatin1(encodedId));
}

bool JinglePub::parseStartUri(const QString &uri, Jid *publisher, QString *publicationId)
{
    if (!publisher || !publicationId || !uri.startsWith(QLatin1String("xmpp:"), Qt::CaseInsensitive))
        return false;

    const auto queryPos = uri.indexOf(QLatin1Char('?'), 5);
    if (queryPos < 0)
        return false;
    const auto parsedPublisher = Jid(QUrl::fromPercentEncoding(uri.mid(5, queryPos - 5).toUtf8()));
    if (!parsedPublisher.isValid())
        return false;

    const auto queryParts = uri.mid(queryPos + 1).split(QLatin1Char(';'));
    if (queryParts.isEmpty() || queryParts.first() != QLatin1String("jingle"))
        return false;
    QString parsedId;
    for (auto it = std::next(queryParts.cbegin()); it != queryParts.cend(); ++it) {
        if (it->startsWith(QLatin1String("id="))) {
            parsedId = QUrl::fromPercentEncoding(it->mid(3).toUtf8());
            break;
        }
    }
    if (parsedId.isEmpty())
        return false;

    *publisher     = parsedPublisher;
    *publicationId = parsedId;
    return true;
}

QList<JinglePub::Meta> JinglePub::meta() const { return d ? d->meta : QList<Meta>(); }
void                   JinglePub::setMeta(const QList<Meta> &meta) { ensureD()->meta = meta; }
void                   JinglePub::addMeta(const Meta &meta) { ensureD()->meta.append(meta); }
QList<QDomElement>     JinglePub::descriptions() const { return d ? d->descriptions : QList<QDomElement>(); }

void JinglePub::setDescriptions(const QList<QDomElement> &descriptions)
{
    auto data = ensureD();
    data->descriptions.clear();
    data->descriptionsDocument = QDomDocument();
    for (const auto &description : descriptions) {
        const auto imported = data->descriptionsDocument.importNode(description, true).toElement();
        if (!imported.isNull())
            data->descriptions.append(imported);
    }
}

void JinglePub::addDescription(const QDomElement &description)
{
    if (description.isNull())
        return;
    auto       data     = ensureD();
    const auto imported = data->descriptionsDocument.importNode(description, true).toElement();
    if (!imported.isNull())
        data->descriptions.append(imported);
}

void JinglePub::addDescription(const QString &applicationNamespace)
{
    if (applicationNamespace.isEmpty())
        return;
    auto data    = ensureD();
    auto element = data->descriptionsDocument.createElementNS(applicationNamespace, QStringLiteral("description"));
    data->descriptions.append(element);
}

bool JinglePub::fromXml(const QDomElement &element)
{
    if (element.isNull() || element.localName() != QLatin1String("jinglepub") || element.namespaceURI() != JINGLEPUB_NS)
        return false;

    Private parsed;
    parsed.from = Jid(element.attribute(QStringLiteral("from")));
    parsed.id   = element.attribute(QStringLiteral("id"));
    if (!parsed.from.isValid() || parsed.id.isEmpty())
        return false;

    QSet<QString> metaLanguages;
    bool          uriSeen = false;
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI() == JINGLEPUB_NS && child.localName() == QLatin1String("uri")) {
            if (uriSeen)
                return false;
            uriSeen    = true;
            parsed.uri = QUrl(child.text(), QUrl::StrictMode);
            if (!parsed.uri.isValid())
                return false;
        } else if (child.namespaceURI() == JINGLEPUB_NS && child.localName() == QLatin1String("meta")) {
            Meta meta;
            meta.language
                = child.attributeNS(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("lang"));
            meta.title   = child.attribute(QStringLiteral("title"));
            meta.summary = child.attribute(QStringLiteral("summary"));
            if (meta.title.isEmpty() || metaLanguages.contains(meta.language))
                return false;
            metaLanguages.insert(meta.language);
            parsed.meta.append(meta);
        } else if (child.localName() == QLatin1String("description") && child.namespaceURI() != JINGLEPUB_NS
                   && !child.namespaceURI().isEmpty()) {
            const auto imported = parsed.descriptionsDocument.importNode(child, true).toElement();
            if (!imported.isNull())
                parsed.descriptions.append(imported);
        }
    }
    if (parsed.descriptions.isEmpty())
        return false;

    d = new Private(parsed);
    return true;
}

QDomElement JinglePub::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};

    auto root = doc->createElementNS(JINGLEPUB_NS, QStringLiteral("jinglepub"));
    root.setAttribute(QStringLiteral("from"), d->from.full());
    root.setAttribute(QStringLiteral("id"), d->id);

    if (d->uri.isValid() && !d->uri.isEmpty()) {
        auto uri = doc->createElementNS(JINGLEPUB_NS, QStringLiteral("uri"));
        uri.appendChild(doc->createTextNode(d->uri.toString()));
        root.appendChild(uri);
    }
    for (const auto &meta : d->meta) {
        if (meta.title.isEmpty())
            continue;
        auto element = doc->createElementNS(JINGLEPUB_NS, QStringLiteral("meta"));
        element.setAttribute(QStringLiteral("title"), meta.title);
        if (!meta.summary.isEmpty())
            element.setAttribute(QStringLiteral("summary"), meta.summary);
        if (!meta.language.isEmpty())
            element.setAttributeNS(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("xml:lang"),
                                   meta.language);
        root.appendChild(element);
    }
    for (const auto &description : d->descriptions)
        root.appendChild(doc->importNode(description, true));
    return root;
}

class PublishedSessionProvider::Private {
public:
    QPointer<PublicationManager> manager;
    State                        state = State::WaitingForConnection;
};

PublishedSessionProvider::PublishedSessionProvider(QObject *parent) :
    QObject(parent), d(std::make_unique<Private>()) { }

PublishedSessionProvider::~PublishedSessionProvider()
{
    if (d->manager)
        d->manager->unregisterProvider(this);
}

PublicationManager             *PublishedSessionProvider::manager() const { return d->manager; }
PublishedSessionProvider::State PublishedSessionProvider::state() const { return d->state; }

void PublishedSessionProvider::restoreCachedPublishedSessions() { }

void PublishedSessionProvider::synchronizePublishedSessions()
{
    if (d->manager)
        d->manager->synchronizeProvider(this);
    else
        finishPublishedSessionSynchronization(false);
}

void PublishedSessionProvider::publishedSessionObserved(const PublishedSessionEndpoint &, const QString &,
                                                        const JinglePub &)
{
}

void PublishedSessionProvider::publishedSessionRetracted(const PublishedSessionEndpoint &, const QString &) { }

void PublishedSessionProvider::publishedSessionNodeInvalidated(const PublishedSessionEndpoint &, bool) { }

JinglePub PublishedSessionProvider::cachePublishedSession(const PublishedSessionEndpoint &endpoint,
                                                          const QString &itemId, JinglePub publication,
                                                          PublishedSessionFactory factory)
{
    return d->manager
        ? d->manager->cachePublishedSession(this, endpoint, itemId, std::move(publication), std::move(factory))
        : JinglePub();
}

bool PublishedSessionProvider::confirmPublishedSession(const QString &publicationId)
{
    return d->manager && d->manager->setPublishedSessionActive(this, publicationId, true);
}

bool PublishedSessionProvider::withdrawPublishedSession(const QString &publicationId)
{
    return d->manager && d->manager->setPublishedSessionActive(this, publicationId, false);
}

void PublishedSessionProvider::forgetPublishedSession(const QString &publicationId)
{
    if (d->manager)
        d->manager->forgetPublishedSession(this, publicationId);
}

void PublishedSessionProvider::finishPublishedSessionSynchronization(bool success)
{
    if (d->manager)
        d->manager->providerSynchronizationFinished(this, success);
}

QList<PublishedSessionEndpoint> PublishedSessionProvider::endpoints() const { return publishedSessionEndpoints(); }

void PublishedSessionProvider::attachManager(PublicationManager *manager) { d->manager = manager; }

void PublishedSessionProvider::beginSynchronization()
{
    setState(State::Synchronizing);
    synchronizePublishedSessions();
}

void PublishedSessionProvider::setState(State state)
{
    if (d->state == state)
        return;
    d->state = state;
    emit stateChanged(state);
}

namespace {

    PublishedSessionEndpoint resolvedEndpoint(const PublishedSessionEndpoint &source, const Client *client)
    {
        auto endpoint = source;
        if (!endpoint.service.isValid() && client && client->jid().isValid())
            endpoint.service = Jid(client->jid().bare());
        return endpoint;
    }

    bool endpointMatches(const PublishedSessionEndpoint &source, const Jid &service, const QString &node,
                         const Client *client)
    {
        const auto endpoint = resolvedEndpoint(source, client);
        if (!endpoint.isValid() || !endpoint.service.isValid() || endpoint.node != node || !service.isValid())
            return false;
        return endpoint.service.resource().isEmpty() ? endpoint.service.compare(service, false)
                                                     : endpoint.service.compare(service);
    }

    QString endpointKey(const PublishedSessionEndpoint &source, const Client *client)
    {
        const auto endpoint = resolvedEndpoint(source, client);
        return endpoint.service.full() + QLatin1Char('\n') + endpoint.node;
    }

    QString authorityKey(const PublishedSessionEndpoint &source, const QString &itemId, const Client *client)
    {
        return endpointKey(source, client) + QLatin1Char('\n') + itemId;
    }

    QByteArray publicationFingerprint(const JinglePub &publication)
    {
        QDomDocument document;
        const auto   element = publication.toXml(&document);
        if (element.isNull())
            return {};
        document.appendChild(element);
        return document.toByteArray(-1);
    }

    bool samePublication(const JinglePub &left, const JinglePub &right)
    {
        return left.isValid() && right.isValid() && publicationFingerprint(left) == publicationFingerprint(right);
    }

} // namespace

class JinglePubPushTask : public Task {
public:
    explicit JinglePubPushTask(PublicationManager *manager, Task *parent) : Task(parent), manager_(manager) { }

    ~JinglePubPushTask() override { qDeleteAll(pending_); }

    void retryPending()
    {
        const auto pending = pending_;
        for (auto item : pending) {
            if (!pending_.contains(item))
                continue;
            if (process(item->document.documentElement()) == Result::Handled)
                removePending(item);
        }
    }

    void clearPending()
    {
        qDeleteAll(pending_);
        pending_.clear();
        pendingKeys_.clear();
    }

protected:
    bool take(const QDomElement &iq) override
    {
        if (iq.tagName() != QLatin1String("iq") || iq.attribute(QStringLiteral("type")) != QLatin1String("get"))
            return false;
        const auto start = childElementsByTagNameNS(iq, JINGLEPUB_NS, QStringLiteral("start")).item(0).toElement();
        if (start.isNull())
            return false;
        if (process(iq) == Result::AwaitingAuthority)
            queue(iq);
        return true;
    }

private:
    enum class Result { Handled, AwaitingAuthority };
    struct Pending {
        QDomDocument document;
        QString      key;
    };

    static constexpr int MaxPending = 64;

    Result process(const QDomElement &iq)
    {
        const auto start = childElementsByTagNameNS(iq, JINGLEPUB_NS, QStringLiteral("start")).item(0).toElement();
        const Jid  requester(iq.attribute(QStringLiteral("from")));
        const auto publicationId = start.attribute(QStringLiteral("id"));
        if (publicationId.isEmpty()) {
            respondError(iq, Stanza::Error::ErrorType::Modify, Stanza::Error::ErrorCond::NotAcceptable);
            return Result::Handled;
        }

        // Do this before looking at the registry, so synchronization does not
        // leak whether an id is present in the local durable cache.
        if (!requester.isValid() || !manager_->jingleManager()->isAllowedParty(requester)) {
            respondError(iq, Stanza::Error::ErrorType::Auth, Stanza::Error::ErrorCond::Forbidden);
            return Result::Handled;
        }

        switch (manager_->startReadiness(publicationId)) {
        case PublicationManager::StartReadiness::AwaitingAuthority:
            return Result::AwaitingAuthority;
        case PublicationManager::StartReadiness::AuthorityUnavailable:
            respondError(iq, Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::ServiceUnavailable);
            return Result::Handled;
        case PublicationManager::StartReadiness::Unknown:
            respondError(iq, Stanza::Error::ErrorType::Modify, Stanza::Error::ErrorCond::NotAcceptable);
            return Result::Handled;
        case PublicationManager::StartReadiness::Active:
            break;
        }

        auto session = manager_->startSession(requester, publicationId);
        if (!session || session->sid().isEmpty()) {
            respondError(iq, Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::ServiceUnavailable);
            return Result::Handled;
        }
        auto response
            = createIQ(client()->doc(), QStringLiteral("result"), requester.full(), iq.attribute(QStringLiteral("id")));
        auto starting = client()->doc()->createElementNS(JINGLEPUB_NS, QStringLiteral("starting"));
        starting.setAttribute(QStringLiteral("sid"), session->sid());
        response.appendChild(starting);
        client()->send(response);
        QTimer::singleShot(0, session, [session]() { session->initiate(); });
        return Result::Handled;
    }

    void queue(const QDomElement &iq)
    {
        const auto key = iq.attribute(QStringLiteral("from")) + QLatin1Char('\n') + iq.attribute(QStringLiteral("id"));
        if (pendingKeys_.contains(key))
            return;
        if (pending_.size() >= MaxPending) {
            respondError(iq, Stanza::Error::ErrorType::Wait, Stanza::Error::ErrorCond::ResourceConstraint);
            return;
        }
        auto pending = new Pending;
        pending->document.appendChild(pending->document.importNode(iq, true));
        pending->key = key;
        pending_.append(pending);
        pendingKeys_.insert(key);
    }

    void removePending(Pending *pending)
    {
        if (!pending_.removeOne(pending))
            return;
        pendingKeys_.remove(pending->key);
        delete pending;
    }

    void respondError(const QDomElement &iq, Stanza::Error::ErrorType type, Stanza::Error::ErrorCond condition)
    {
        auto response = createIQ(client()->doc(), QStringLiteral("error"), iq.attribute(QStringLiteral("from")),
                                 iq.attribute(QStringLiteral("id")));
        const Stanza::Error error(type, condition);
        response.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
        client()->send(response);
    }

    PublicationManager *manager_ = nullptr;
    QList<Pending *>    pending_;
    QSet<QString>       pendingKeys_;
};

class PublicationManager::Private {
public:
    struct Published {
        JinglePub                          publication;
        PublishedSessionFactory            factory;
        QPointer<PublishedSessionProvider> provider;
        PublishedSessionEndpoint           endpoint;
        QString                            itemId;
        bool                               active = true;
    };

    struct AuthorityEvent {
        enum class Type { Published, Retracted, Invalidated };
        Type                     type = Type::Published;
        PublishedSessionEndpoint endpoint;
        QString                  itemId;
        JinglePub                publication;
        bool                     deleted = false;
    };

    struct ProviderSync {
        PublishedSessionProvider::State state = PublishedSessionProvider::State::WaitingForConnection;
        QList<PublishedSessionEndpoint> endpoints;
        quint64                         generation   = 0;
        int                             pendingTasks = 0;
        bool                            failed       = false;
        QHash<QString, AuthorityEvent>  discoveryItems;
        QHash<QString, AuthorityEvent>  targetedResults;
        QList<AuthorityEvent>           bufferedEvents;
    };

    Manager                                        *jingleManager = nullptr;
    std::unique_ptr<JinglePubPushTask>              pushTask;
    QHash<QString, Published>                       publishedSessions;
    QHash<PublishedSessionProvider *, ProviderSync> providers;
    quint64                                         generation        = 0;
    bool                                            presenceAvailable = false;
};

PublicationManager::PublicationManager(Manager *jingleManager) : QObject(jingleManager), d(std::make_unique<Private>())
{
    d->jingleManager = jingleManager;
    d->pushTask      = std::make_unique<JinglePubPushTask>(this, client()->rootTask());

    connect(client(), &Client::disconnected, this, &PublicationManager::clientDisconnected);
    connect(client()->pubSubManager(), &PubSubManager::itemPublished, this, &PublicationManager::handleItemPublished);
    connect(client()->pubSubManager(), &PubSubManager::itemRetracted, this, &PublicationManager::handleItemRetracted);
    connect(client()->pubSubManager(), &PubSubManager::nodePurged, this,
            [this](const Jid &service, const QString &node) { handleNodeInvalidated(service, node, false); });
    connect(client()->pubSubManager(), &PubSubManager::nodeDeleted, this,
            [this](const Jid &service, const QString &node) { handleNodeInvalidated(service, node, true); });
}

PublicationManager::~PublicationManager()
{
    const auto providers = d->providers.keys();
    for (auto provider : providers) {
        if (provider)
            provider->attachManager(nullptr);
    }
}

Manager *PublicationManager::jingleManager() const { return d->jingleManager; }
Client  *PublicationManager::client() const { return d->jingleManager->client(); }

QStringList PublicationManager::discoFeatures() const
{
    QStringList features { JINGLEPUB_NS };
    for (auto it = d->providers.cbegin(); it != d->providers.cend(); ++it) {
        if (!it.key())
            continue;
        for (const auto &endpoint : it->endpoints) {
            if (endpoint.pepNotify && !endpoint.node.isEmpty())
                features.append(endpoint.node + QStringLiteral("+notify"));
        }
    }
    features.removeDuplicates();
    return features;
}

void PublicationManager::registerProvider(PublishedSessionProvider *provider)
{
    if (!provider || d->providers.contains(provider) || (provider->manager() && provider->manager() != this))
        return;

    Private::ProviderSync sync;
    sync.endpoints = provider->endpoints();
    sync.endpoints.erase(std::remove_if(sync.endpoints.begin(), sync.endpoints.end(),
                                        [](const auto &endpoint) { return !endpoint.isValid(); }),
                         sync.endpoints.end());
    d->providers.insert(provider, std::move(sync));
    provider->attachManager(this);
    provider->setState(PublishedSessionProvider::State::WaitingForConnection);
    provider->restoreCachedPublishedSessions();

    if (!provider || provider->manager() != this || !d->presenceAvailable)
        return;
    auto &context      = d->providers[provider];
    context.state      = PublishedSessionProvider::State::Synchronizing;
    context.generation = ++d->generation;
    provider->beginSynchronization();
}

void PublicationManager::unregisterProvider(PublishedSessionProvider *provider)
{
    if (!provider || !d->providers.remove(provider))
        return;

    QStringList removed;
    for (auto it = d->publishedSessions.begin(); it != d->publishedSessions.end();) {
        if (it->provider == provider) {
            removed.append(it.key());
            it = d->publishedSessions.erase(it);
        } else {
            ++it;
        }
    }
    if (provider->manager() == this)
        provider->attachManager(nullptr);
    for (const auto &id : std::as_const(removed))
        emit publishedSessionStateChanged(id, SessionState::Missing);
    retryPendingStarts();
}

JinglePub PublicationManager::registerPublishedSession(JinglePub publication, PublishedSessionFactory factory)
{
    if (!factory)
        return {};
    if (!publication.from().isValid())
        publication.setFrom(client()->jid());
    if (publication.id().isEmpty()) {
        QString id;
        do {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        } while (d->publishedSessions.contains(id));
        publication.setId(id);
    }
    if (!publication.isValid())
        return {};
    const auto localJid = client()->jid();
    if (!publication.from().compare(localJid, false)
        || (!publication.from().resource().isEmpty() && !publication.from().compare(localJid))) {
        return {};
    }
    if (d->publishedSessions.contains(publication.id()))
        return {};

    Private::Published entry;
    entry.publication = publication;
    entry.factory     = std::move(factory);
    entry.active      = true;
    d->publishedSessions.insert(publication.id(), std::move(entry));
    emit publishedSessionStateChanged(publication.id(), SessionState::Active);
    retryPendingStarts();
    return publication;
}

void PublicationManager::unregisterPublishedSession(const QString &id)
{
    if (!d->publishedSessions.remove(id))
        return;
    emit publishedSessionStateChanged(id, SessionState::Missing);
    retryPendingStarts();
}

JinglePub PublicationManager::publishedSession(const QString &id) const
{
    const auto it = d->publishedSessions.constFind(id);
    return it == d->publishedSessions.cend() ? JinglePub() : it->publication;
}

PublicationManager::SessionState PublicationManager::publishedSessionState(const QString &id) const
{
    const auto it = d->publishedSessions.constFind(id);
    if (it == d->publishedSessions.cend())
        return SessionState::Missing;
    return it->active ? SessionState::Active : SessionState::Unverified;
}

JinglePub PublicationManager::cachePublishedSession(PublishedSessionProvider       *provider,
                                                    const PublishedSessionEndpoint &endpoint,
                                                    const QString &sourceItemId, JinglePub publication,
                                                    PublishedSessionFactory factory)
{
    if (!provider || !d->providers.contains(provider) || !endpoint.isValid() || !factory)
        return {};
    if (publication.id().isEmpty()) {
        QString id;
        do {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        } while (d->publishedSessions.contains(id));
        publication.setId(id);
    }
    if (!publication.isValid())
        return {};
    if (client()->isActive() && !publication.from().compare(client()->jid()))
        return {};

    const auto itemId  = sourceItemId.isEmpty() ? publication.id() : sourceItemId;
    const auto current = d->publishedSessions.constFind(publication.id());
    const bool existed = current != d->publishedSessions.cend();
    if (existed && current->provider != provider)
        return {};
    const bool wasActive = existed && current->active;

    Private::Published entry;
    entry.publication = publication;
    entry.factory     = std::move(factory);
    entry.provider    = provider;
    entry.endpoint    = endpoint;
    entry.itemId      = itemId;
    entry.active      = false;
    d->publishedSessions.insert(publication.id(), std::move(entry));
    if (!existed || wasActive)
        emit publishedSessionStateChanged(publication.id(), SessionState::Unverified);
    retryPendingStarts();
    return publication;
}

bool PublicationManager::setPublishedSessionActive(PublishedSessionProvider *provider, const QString &id, bool active)
{
    auto it = d->publishedSessions.find(id);
    if (it == d->publishedSessions.end() || it->provider != provider)
        return false;
    if (active && (!client()->isActive() || !it->publication.from().compare(client()->jid())))
        return false;
    if (it->active == active)
        return true;
    it->active = active;
    emit publishedSessionStateChanged(id, active ? SessionState::Active : SessionState::Unverified);
    retryPendingStarts();
    return true;
}

void PublicationManager::forgetPublishedSession(PublishedSessionProvider *provider, const QString &id)
{
    auto it = d->publishedSessions.find(id);
    if (it == d->publishedSessions.end() || it->provider != provider)
        return;
    d->publishedSessions.erase(it);
    emit publishedSessionStateChanged(id, SessionState::Missing);
    retryPendingStarts();
}

PubSubPublishTask *PublicationManager::publishSession(const QString &publicationId, const PubSubOptions &publishOptions)
{
    const auto it = d->publishedSessions.constFind(publicationId);
    if (it == d->publishedSessions.cend() || !it->provider)
        return nullptr;
    const auto endpoint = resolvedEndpoint(it->endpoint, client());
    auto task = publishSessionAnnouncement(endpoint.service, endpoint.node, publicationId, it->itemId, publishOptions);
    if (!task)
        return nullptr;

    const QPointer<PublishedSessionProvider> provider       = it->provider;
    const auto                               expectedItemId = it->itemId;
    connect(task, &Task::finished, this, [this, task, provider, publicationId, expectedItemId]() {
        if (!provider || !task->success())
            return;
        const auto publishedId = task->publishedId();
        if (!publishedId.isEmpty() && publishedId != expectedItemId)
            return;
        setPublishedSessionActive(provider, publicationId, true);
    });
    return task;
}

PubSubRetractTask *PublicationManager::retractSession(const QString &publicationId, bool notify)
{
    const auto it = d->publishedSessions.constFind(publicationId);
    if (it == d->publishedSessions.cend() || !it->provider)
        return nullptr;
    const auto provider = it->provider;
    const auto endpoint = resolvedEndpoint(it->endpoint, client());
    const auto itemId   = it->itemId;
    setPublishedSessionActive(provider, publicationId, false);
    return retractSessionAnnouncement(endpoint.service, endpoint.node, itemId, notify);
}

PubSubPublishTask *PublicationManager::publishSessionAnnouncement(const Jid &service, const QString &node,
                                                                  const QString       &publicationId,
                                                                  const QString       &sourceItemId,
                                                                  const PubSubOptions &publishOptions)
{
    if (!service.isValid() || node.isEmpty())
        return nullptr;
    const auto publication = publishedSession(publicationId);
    if (!publication.isValid())
        return nullptr;
    const auto payload = publication.toXml(client()->doc());
    if (payload.isNull())
        return nullptr;
    const auto itemId = sourceItemId.isEmpty() ? publication.id() : sourceItemId;
    return client()->pubSubManager()->publish(service, node, PubSubItem(itemId, payload), publishOptions);
}

PubSubRetractTask *PublicationManager::retractSessionAnnouncement(const Jid &service, const QString &node,
                                                                  const QString &itemId, bool notify)
{
    if (!service.isValid() || node.isEmpty() || itemId.isEmpty())
        return nullptr;
    return client()->pubSubManager()->retract(service, node, itemId, notify);
}

PublishedSessionRequest *PublicationManager::requestPublishedSession(const Jid &publisher, const QString &id,
                                                                     QObject *parent)
{
    return new PublishedSessionRequest(this, publisher, id, parent ? parent : this);
}

void PublicationManager::clientPresenceAvailable()
{
    if (d->presenceAvailable)
        return;
    d->presenceAvailable = true;
    const auto providers = d->providers.keys();
    for (auto provider : providers) {
        if (!provider || !d->providers.contains(provider))
            continue;
        auto &context        = d->providers[provider];
        context.state        = PublishedSessionProvider::State::Synchronizing;
        context.generation   = ++d->generation;
        context.pendingTasks = 0;
        context.failed       = false;
        context.discoveryItems.clear();
        context.targetedResults.clear();
        context.bufferedEvents.clear();
        for (auto it = d->publishedSessions.begin(); it != d->publishedSessions.end(); ++it) {
            if (it->provider != provider || !it->active)
                continue;
            it->active = false;
            emit publishedSessionStateChanged(it.key(), SessionState::Unverified);
        }
        provider->beginSynchronization();
    }
    retryPendingStarts();
}

void PublicationManager::clientDisconnected()
{
    d->presenceAvailable = false;
    ++d->generation;
    for (auto it = d->publishedSessions.begin(); it != d->publishedSessions.end(); ++it) {
        if (!it->provider || !it->active)
            continue;
        it->active = false;
        emit publishedSessionStateChanged(it.key(), SessionState::Unverified);
    }
    for (auto it = d->providers.begin(); it != d->providers.end(); ++it) {
        it->state        = PublishedSessionProvider::State::WaitingForConnection;
        it->generation   = d->generation;
        it->pendingTasks = 0;
        it->failed       = false;
        it->discoveryItems.clear();
        it->targetedResults.clear();
        it->bufferedEvents.clear();
        if (it.key())
            it.key()->setState(PublishedSessionProvider::State::WaitingForConnection);
    }
    d->pushTask->clearPending();
}

void PublicationManager::synchronizeProvider(PublishedSessionProvider *provider)
{
    auto contextIt = d->providers.find(provider);
    if (contextIt == d->providers.end() || contextIt->state != PublishedSessionProvider::State::Synchronizing
        || !client()->isActive()) {
        providerSynchronizationFinished(provider, false);
        return;
    }

    struct Query {
        PublishedSessionEndpoint endpoint;
        QStringList              itemIds;
        bool                     discovery = false;
    };
    QList<Query> queries;

    QHash<QString, PublishedSessionEndpoint> uniqueEndpoints;
    for (const auto &configured : contextIt->endpoints) {
        const auto endpoint = resolvedEndpoint(configured, client());
        if (endpoint.isValid() && endpoint.service.isValid())
            uniqueEndpoints.insert(endpointKey(endpoint, client()), endpoint);
    }
    for (const auto &endpoint : uniqueEndpoints)
        queries.append(Query { endpoint, {}, true });

    static constexpr int BatchSize = 50;
    for (const auto &endpoint : uniqueEndpoints) {
        QStringList itemIds;
        for (auto it = d->publishedSessions.cbegin(); it != d->publishedSessions.cend(); ++it) {
            if (it->provider == provider && endpointMatches(it->endpoint, endpoint.service, endpoint.node, client()))
                itemIds.append(it->itemId);
        }
        itemIds.removeDuplicates();
        for (int offset = 0; offset < itemIds.size(); offset += BatchSize)
            queries.append(Query { endpoint, itemIds.mid(offset, BatchSize), false });
    }

    if (queries.isEmpty()) {
        providerSynchronizationFinished(provider, true);
        return;
    }

    const auto generation = contextIt->generation;
    contextIt->pendingTasks += queries.size();
    for (const auto &query : queries) {
        auto task = client()->pubSubManager()->items(query.endpoint.service, query.endpoint.node, query.itemIds);
        const QPointer<PublishedSessionProvider> guard(provider);
        connect(task, &Task::finished, this, [this, task, guard, query, generation]() {
            if (!guard)
                return;
            auto context = d->providers.find(guard);
            if (context == d->providers.end() || context->generation != generation
                || context->state != PublishedSessionProvider::State::Synchronizing) {
                return;
            }

            const bool absent  = !task->success() && task->error().condition == Stanza::Error::ErrorCond::ItemNotFound;
            const bool success = task->success() || absent;
            if (!success)
                context->failed = true;

            if (success) {
                if (!query.discovery) {
                    for (const auto &itemId : query.itemIds) {
                        Private::AuthorityEvent result;
                        result.type     = Private::AuthorityEvent::Type::Retracted;
                        result.endpoint = query.endpoint;
                        result.itemId   = itemId;
                        context->targetedResults.insert(authorityKey(query.endpoint, itemId, client()), result);
                    }
                }
                for (const auto &item : task->items()) {
                    if (item.id().isEmpty() || (!query.discovery && !query.itemIds.contains(item.id())))
                        continue;
                    Private::AuthorityEvent result;
                    result.type        = Private::AuthorityEvent::Type::Published;
                    result.endpoint    = query.endpoint;
                    result.itemId      = item.id();
                    result.publication = JinglePub(item.payload());
                    const auto key     = authorityKey(query.endpoint, item.id(), client());
                    if (query.discovery)
                        context->discoveryItems.insert(key, result);
                    else
                        context->targetedResults.insert(key, result);
                }
            }

            if (--context->pendingTasks == 0)
                providerSynchronizationFinished(guard, !context->failed);
        });
        task->go(true);
    }
}

void PublicationManager::providerSynchronizationFinished(PublishedSessionProvider *provider, bool success)
{
    auto context = d->providers.find(provider);
    if (context == d->providers.end() || context->state != PublishedSessionProvider::State::Synchronizing)
        return;

    const auto discovery = context->discoveryItems.values();
    const auto targeted  = context->targetedResults.values();
    const auto events    = context->bufferedEvents;
    context->discoveryItems.clear();
    context->targetedResults.clear();
    context->bufferedEvents.clear();
    context->pendingTasks = 0;

    for (const auto &event : discovery)
        applyPublishedItem(provider, event.endpoint, event.itemId, event.publication);
    // Targeted results are authoritative for known ids and deliberately
    // override a possibly truncated/unordered discovery snapshot.
    for (const auto &event : targeted) {
        if (event.type == Private::AuthorityEvent::Type::Published)
            applyPublishedItem(provider, event.endpoint, event.itemId, event.publication);
        else
            applyRetractedItem(provider, event.endpoint, event.itemId);
    }
    // PubSub notifications observed while the IQs were in flight are replayed
    // last so a stale snapshot cannot resurrect a retracted item.
    for (const auto &event : events) {
        switch (event.type) {
        case Private::AuthorityEvent::Type::Published:
            applyPublishedItem(provider, event.endpoint, event.itemId, event.publication);
            break;
        case Private::AuthorityEvent::Type::Retracted:
            applyRetractedItem(provider, event.endpoint, event.itemId);
            break;
        case Private::AuthorityEvent::Type::Invalidated:
            applyInvalidatedNode(provider, event.endpoint, event.deleted);
            break;
        }
    }

    if (!d->providers.contains(provider))
        return;
    const auto state
        = success ? PublishedSessionProvider::State::Synchronized : PublishedSessionProvider::State::Failed;
    d->providers[provider].state = state;
    provider->setState(state);
    retryPendingStarts();
}

void PublicationManager::handleItemPublished(const Jid &service, const QString &node, const PubSubItem &item)
{
    const auto providers = d->providers.keys();
    for (auto provider : providers) {
        if (!provider || !d->providers.contains(provider))
            continue;
        auto &context = d->providers[provider];
        for (const auto &configured : context.endpoints) {
            if (!endpointMatches(configured, service, node, client()))
                continue;
            Private::AuthorityEvent event;
            event.type        = Private::AuthorityEvent::Type::Published;
            event.endpoint    = resolvedEndpoint(configured, client());
            event.itemId      = item.id();
            event.publication = JinglePub(item.payload());
            if (context.state == PublishedSessionProvider::State::Synchronizing)
                context.bufferedEvents.append(event);
            else if (context.state != PublishedSessionProvider::State::WaitingForConnection)
                applyPublishedItem(provider, event.endpoint, event.itemId, event.publication);
            break;
        }
    }
}

void PublicationManager::handleItemRetracted(const Jid &service, const QString &node, const QString &itemId)
{
    const auto providers = d->providers.keys();
    for (auto provider : providers) {
        if (!provider || !d->providers.contains(provider))
            continue;
        auto &context = d->providers[provider];
        for (const auto &configured : context.endpoints) {
            if (!endpointMatches(configured, service, node, client()))
                continue;
            Private::AuthorityEvent event;
            event.type     = Private::AuthorityEvent::Type::Retracted;
            event.endpoint = resolvedEndpoint(configured, client());
            event.itemId   = itemId;
            if (context.state == PublishedSessionProvider::State::Synchronizing)
                context.bufferedEvents.append(event);
            else if (context.state != PublishedSessionProvider::State::WaitingForConnection)
                applyRetractedItem(provider, event.endpoint, event.itemId);
            break;
        }
    }
}

void PublicationManager::handleNodeInvalidated(const Jid &service, const QString &node, bool deleted)
{
    const auto providers = d->providers.keys();
    for (auto provider : providers) {
        if (!provider || !d->providers.contains(provider))
            continue;
        auto &context = d->providers[provider];
        for (const auto &configured : context.endpoints) {
            if (!endpointMatches(configured, service, node, client()))
                continue;
            Private::AuthorityEvent event;
            event.type     = Private::AuthorityEvent::Type::Invalidated;
            event.endpoint = resolvedEndpoint(configured, client());
            event.deleted  = deleted;
            if (context.state == PublishedSessionProvider::State::Synchronizing)
                context.bufferedEvents.append(event);
            else if (context.state != PublishedSessionProvider::State::WaitingForConnection)
                applyInvalidatedNode(provider, event.endpoint, deleted);
            break;
        }
    }
}

void PublicationManager::applyPublishedItem(PublishedSessionProvider       *provider,
                                            const PublishedSessionEndpoint &endpoint, const QString &itemId,
                                            const JinglePub &publication)
{
    if (!provider || !d->providers.contains(provider))
        return;

    auto updateMatches = [this, provider, &endpoint, &itemId, &publication]() {
        const auto ids = d->publishedSessions.keys();
        for (const auto &id : ids) {
            auto it = d->publishedSessions.find(id);
            if (it == d->publishedSessions.end() || it->provider != provider || it->itemId != itemId
                || !endpointMatches(it->endpoint, endpoint.service, endpoint.node, client())) {
                continue;
            }
            const bool active = client()->isActive() && publication.isValid()
                && publication.from().compare(client()->jid()) && samePublication(it->publication, publication);
            setPublishedSessionActive(provider, id, active);
        }
    };

    updateMatches();
    const QPointer<PublishedSessionProvider> guard(provider);
    if (publication.isValid())
        provider->publishedSessionObserved(endpoint, itemId, publication);
    if (guard && d->providers.contains(guard))
        updateMatches(); // the callback may have materialized a factory from its durable store
}

void PublicationManager::applyRetractedItem(PublishedSessionProvider       *provider,
                                            const PublishedSessionEndpoint &endpoint, const QString &itemId)
{
    if (!provider || !d->providers.contains(provider))
        return;
    const auto ids = d->publishedSessions.keys();
    for (const auto &id : ids) {
        auto it = d->publishedSessions.constFind(id);
        if (it != d->publishedSessions.cend() && it->provider == provider && it->itemId == itemId
            && endpointMatches(it->endpoint, endpoint.service, endpoint.node, client())) {
            setPublishedSessionActive(provider, id, false);
        }
    }
    provider->publishedSessionRetracted(endpoint, itemId);
}

void PublicationManager::applyInvalidatedNode(PublishedSessionProvider       *provider,
                                              const PublishedSessionEndpoint &endpoint, bool deleted)
{
    if (!provider || !d->providers.contains(provider))
        return;
    const auto ids = d->publishedSessions.keys();
    for (const auto &id : ids) {
        auto it = d->publishedSessions.constFind(id);
        if (it != d->publishedSessions.cend() && it->provider == provider
            && endpointMatches(it->endpoint, endpoint.service, endpoint.node, client())) {
            setPublishedSessionActive(provider, id, false);
        }
    }
    provider->publishedSessionNodeInvalidated(endpoint, deleted);
}

PublicationManager::StartReadiness PublicationManager::startReadiness(const QString &id) const
{
    const auto session = d->publishedSessions.constFind(id);
    if (session != d->publishedSessions.cend()) {
        if (!session->provider)
            return session->active ? StartReadiness::Active : StartReadiness::Unknown;
        const auto provider = d->providers.constFind(session->provider);
        if (provider == d->providers.cend())
            return StartReadiness::Unknown;
        if (provider->state == PublishedSessionProvider::State::WaitingForConnection
            || provider->state == PublishedSessionProvider::State::Synchronizing) {
            return StartReadiness::AwaitingAuthority;
        }
        if (session->active)
            return StartReadiness::Active;
        return provider->state == PublishedSessionProvider::State::Failed ? StartReadiness::AuthorityUnavailable
                                                                          : StartReadiness::Unknown;
    }

    bool unavailable = false;
    for (auto it = d->providers.cbegin(); it != d->providers.cend(); ++it) {
        if (it->state == PublishedSessionProvider::State::WaitingForConnection
            || it->state == PublishedSessionProvider::State::Synchronizing) {
            return StartReadiness::AwaitingAuthority;
        }
        unavailable = unavailable || it->state == PublishedSessionProvider::State::Failed;
    }
    return unavailable ? StartReadiness::AuthorityUnavailable : StartReadiness::Unknown;
}

Session *PublicationManager::startSession(const Jid &requester, const QString &id)
{
    auto it = d->publishedSessions.find(id);
    if (it == d->publishedSessions.end() || startReadiness(id) != StartReadiness::Active
        || !d->jingleManager->isAllowedParty(requester)) {
        return nullptr;
    }
    Session *session = it->factory(requester);
    if (!session || session->manager() != d->jingleManager || session->role() != Origin::Initiator
        || !session->peer().compare(requester)) {
        if (session && session->manager() == d->jingleManager)
            session->deleteLater();
        return nullptr;
    }
    if (session->reserveSid().isEmpty()) {
        session->deleteLater();
        return nullptr;
    }
    return session;
}

void PublicationManager::retryPendingStarts() { d->pushTask->retryPending(); }

class JinglePubStartTask : public Task {
public:
    JinglePubStartTask(Task *parent, const Jid &publisher, const QString &publicationId) : Task(parent), to(publisher)
    {
        iq         = createIQ(doc(), "get", publisher.full(), id());
        auto start = doc()->createElementNS(JINGLEPUB_NS, QStringLiteral("start"));
        start.setAttribute(QStringLiteral("id"), publicationId);
        iq.appendChild(start);
    }

    QString sid;

protected:
    void onGo() override { send(iq); }

    bool take(const QDomElement &element) override
    {
        if (element.localName() != QLatin1String("iq") || element.attribute(QStringLiteral("id")) != id())
            return false;
        const Jid  from(element.attribute(QStringLiteral("from")));
        const bool senderMatches = to.resource().isEmpty() ? from.compare(to, false) : from.compare(to);
        if (!from.isValid() || !senderMatches)
            return false;
        const auto type = element.attribute(QStringLiteral("type"));
        if (type != QLatin1String("result") && type != QLatin1String("error"))
            return false;
        if (type == QLatin1String("error")) {
            setError(element);
            return true;
        }
        auto starting = childElementsByTagNameNS(element, JINGLEPUB_NS, QStringLiteral("starting")).item(0).toElement();
        if (starting.isNull() || (sid = starting.attribute(QStringLiteral("sid"))).isEmpty()) {
            setError(0, QStringLiteral("Invalid XEP-0358 <starting/> response"));
            return true;
        }
        setSuccess();
        return true;
    }

private:
    Jid         to;
    QDomElement iq;
};

class PublishedSessionRequest::Private {
public:
    PublicationManager          *manager = nullptr;
    Jid                          publisher;
    QString                      publicationId;
    QString                      sid;
    State                        state = State::Idle;
    Stanza::Error                error;
    QPointer<JinglePubStartTask> task;
};

PublishedSessionRequest::PublishedSessionRequest(PublicationManager *manager, const Jid &publisher,
                                                 const QString &publicationId, QObject *parent) :
    QObject(parent), d(new Private)
{
    d->manager       = manager;
    d->publisher     = publisher;
    d->publicationId = publicationId;
}

PublishedSessionRequest::~PublishedSessionRequest()
{
    if (d->task)
        d->task->deleteLater();
}
PublishedSessionRequest::State PublishedSessionRequest::state() const { return d->state; }
Jid                            PublishedSessionRequest::publisher() const { return d->publisher; }
QString                        PublishedSessionRequest::publicationId() const { return d->publicationId; }
QString                        PublishedSessionRequest::sid() const { return d->sid; }
Stanza::Error                  PublishedSessionRequest::error() const { return d->error; }

void PublishedSessionRequest::start()
{
    if (d->state != State::Idle || !d->manager || !d->publisher.isValid() || d->publicationId.isEmpty())
        return;
    d->state  = State::Pending;
    auto task = new JinglePubStartTask(d->manager->client()->rootTask(), d->publisher, d->publicationId);
    d->task   = task;
    connect(task, &Task::finished, this, [this, task]() {
        if (task->success()) {
            d->sid   = task->sid;
            d->state = State::Succeeded;
        } else {
            d->error = task->error();
            d->state = State::Failed;
        }
        emit finished();
    });
    task->go(true);
}

} // namespace XMPP::Jingle
