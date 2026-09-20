/*
 * jingle-pub.h - XEP-0358 Publishing Available Jingle Sessions
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XMPP_JINGLE_PUB_H
#define XMPP_JINGLE_PUB_H

#include <iris/iris_export.h>

#include <iris/xmpp-core/xmpp_stanza.h>
#include <iris/xmpp-im/xmpp_pubsub.h>

#include <QDomElement>
#include <QList>
#include <QObject>
#include <QSharedDataPointer>
#include <QUrl>

#include <functional>
#include <memory>

namespace XMPP::Jingle {

extern IRIS_EXPORT const QString JINGLEPUB_NS;

class JinglePub {
public:
    struct Meta {
        QString language;
        QString title;
        QString summary;
    };

    JinglePub();
    explicit JinglePub(const QDomElement &element);
    JinglePub(const JinglePub &);
    JinglePub &operator=(const JinglePub &);
    ~JinglePub();

    bool isValid() const;

    Jid  from() const;
    void setFrom(const Jid &from);

    QString id() const;
    void    setId(const QString &id);

    QUrl uri() const;
    void setUri(const QUrl &uri);

    QString     startUri() const;
    static bool parseStartUri(const QString &uri, Jid *publisher, QString *publicationId);

    QList<Meta> meta() const;
    void        setMeta(const QList<Meta> &meta);
    void        addMeta(const Meta &meta);

    // Returned DOM handles are backed by this JinglePub's shared data. They
    // remain valid while a JinglePub instance sharing that data is alive.
    QList<QDomElement> descriptions() const;
    // Inputs are deep-imported; the caller's QDomDocument may be destroyed after return.
    void               setDescriptions(const QList<QDomElement> &descriptions);
    void               addDescription(const QDomElement &description);
    void               addDescription(const QString &applicationNamespace);

    bool        fromXml(const QDomElement &element);
    QDomElement toXml(QDomDocument *doc) const;

private:
    class Private;
    Private                    *ensureD();
    QSharedDataPointer<Private> d;
};

class Manager;
class PublicationManager;
class PublishedSessionRequest;
class Session;

using PublishedSessionFactory = std::function<Session *(const Jid &requester)>;

/** A PubSub/PEP location whose direct item payloads are XEP-0358 announcements. */
struct IRIS_EXPORT PublishedSessionEndpoint {
    Jid     service;
    QString node;
    /** Add the XEP-0163 @c node+notify feature to entity capabilities. */
    bool pepNotify = true;

    bool isValid() const { return !node.isEmpty(); }
};

/**
 * Application-owned bridge between durable media state and Jingle.
 *
 * The provider is registered before the XMPP connection starts. It restores
 * locally usable session factories from its encrypted cache, while
 * PublicationManager verifies their corresponding PubSub items after resource
 * binding. A cached
 * factory is never callable until its server item has been positively
 * observed. Remote retraction only disables a factory; it never republishes an
 * item from the local cache.
 */
class IRIS_EXPORT PublishedSessionProvider : public QObject {
    Q_OBJECT
public:
    enum class State { WaitingForConnection, Synchronizing, Synchronized, Failed };
    Q_ENUM(State)

    explicit PublishedSessionProvider(QObject *parent = nullptr);
    ~PublishedSessionProvider() override;

    PublicationManager *manager() const;
    State               state() const;

signals:
    void stateChanged(XMPP::Jingle::PublishedSessionProvider::State state);

protected:
    /** Endpoints are queried before initial presence so PEP notifications can be advertised. */
    virtual QList<PublishedSessionEndpoint> publishedSessionEndpoints() const = 0;
    /** Called synchronously when the provider is attached, normally before connecting. */
    virtual void restoreCachedPublishedSessions();
    /**
     * Starts authoritative verification after Client::start().
     *
     * The default implementation performs targeted PubSub lookups for all
     * cached candidates. Subclasses may override it for an application-specific
     * authoritative store and then call finishPublishedSessionSynchronization().
     */
    virtual void synchronizePublishedSessions();

    virtual void publishedSessionObserved(const PublishedSessionEndpoint &endpoint, const QString &itemId,
                                          const JinglePub &publication);
    virtual void publishedSessionRetracted(const PublishedSessionEndpoint &endpoint, const QString &itemId);
    virtual void publishedSessionNodeInvalidated(const PublishedSessionEndpoint &endpoint, bool deleted);

    /** Cache a locally usable factory in the Unverified state. */
    JinglePub cachePublishedSession(const PublishedSessionEndpoint &endpoint, const QString &itemId,
                                    JinglePub publication, PublishedSessionFactory factory);
    /** Positive server acknowledgement or snapshot observation. */
    bool confirmPublishedSession(const QString &publicationId);
    /** Immediately stops serving while retaining the local capability. */
    bool withdrawPublishedSession(const QString &publicationId);
    /** Removes the local capability as well as its authority state. */
    void forgetPublishedSession(const QString &publicationId);
    /** Completes an overridden synchronizePublishedSessions() implementation. */
    void finishPublishedSessionSynchronization(bool success);

private:
    friend class PublicationManager;
    QList<PublishedSessionEndpoint> endpoints() const;
    void                            attachManager(PublicationManager *manager);
    void                            beginSynchronization();
    void                            setState(State state);

    class Private;
    std::unique_ptr<Private> d;
};

/**
 * Runtime and PubSub authority for XEP-0358 publications.
 *
 * The Jingle manager owns one instance, exposed as
 * Manager::publicationManager(). Providers can be attached before connecting;
 * their cached factories remain unavailable until the corresponding PubSub
 * items have been verified after resource binding.
 */
class IRIS_EXPORT PublicationManager : public QObject {
    Q_OBJECT
public:
    enum class SessionState { Missing, Unverified, Active };
    Q_ENUM(SessionState)

    explicit PublicationManager(Manager *jingleManager);
    ~PublicationManager() override;

    Manager      *jingleManager() const;
    XMPP::Client *client() const;

    QStringList discoFeatures() const;

    void registerProvider(PublishedSessionProvider *provider);
    void unregisterProvider(PublishedSessionProvider *provider);

    // Compatibility/local mode: entries registered here are immediately
    // active and do not participate in PubSub authority reconciliation.
    JinglePub    registerPublishedSession(JinglePub publication, PublishedSessionFactory factory);
    void         unregisterPublishedSession(const QString &id);
    JinglePub    publishedSession(const QString &id) const;
    SessionState publishedSessionState(const QString &id) const;

    /** Publish/retract the provider-owned PubSub item associated with a session. */
    PubSubPublishTask *publishSession(const QString &publicationId, const PubSubOptions &publishOptions = {});
    PubSubRetractTask *retractSession(const QString &publicationId, bool notify = true);

    /** Low-level helpers for direct XEP-0358 PubSub payloads. */
    PubSubPublishTask *publishSessionAnnouncement(const Jid &service, const QString &node, const QString &publicationId,
                                                  const QString &itemId = {}, const PubSubOptions &publishOptions = {});
    PubSubRetractTask *retractSessionAnnouncement(const Jid &service, const QString &node, const QString &itemId,
                                                  bool notify = true);

    PublishedSessionRequest *requestPublishedSession(const Jid &publisher, const QString &id,
                                                     QObject *parent = nullptr);

signals:
    void publishedSessionStateChanged(const QString &id, XMPP::Jingle::PublicationManager::SessionState state);

private:
    friend class Manager;
    friend class PublishedSessionProvider;
    friend class JinglePubPushTask;

    enum class StartReadiness { Unknown, AwaitingAuthority, AuthorityUnavailable, Active };

    JinglePub cachePublishedSession(PublishedSessionProvider *provider, const PublishedSessionEndpoint &endpoint,
                                    const QString &itemId, JinglePub publication, PublishedSessionFactory factory);
    bool      setPublishedSessionActive(PublishedSessionProvider *provider, const QString &id, bool active);
    void      forgetPublishedSession(PublishedSessionProvider *provider, const QString &id);
    void      providerSynchronizationFinished(PublishedSessionProvider *provider, bool success);
    void      synchronizeProvider(PublishedSessionProvider *provider);

    void           clientPresenceAvailable();
    void           clientDisconnected();
    void           handleItemPublished(const Jid &service, const QString &node, const PubSubItem &item);
    void           handleItemRetracted(const Jid &service, const QString &node, const QString &itemId);
    void           handleNodeInvalidated(const Jid &service, const QString &node, bool deleted);
    void           applyPublishedItem(PublishedSessionProvider *provider, const PublishedSessionEndpoint &endpoint,
                                      const QString &itemId, const JinglePub &publication);
    void           applyRetractedItem(PublishedSessionProvider *provider, const PublishedSessionEndpoint &endpoint,
                                      const QString &itemId);
    void           applyInvalidatedNode(PublishedSessionProvider *provider, const PublishedSessionEndpoint &endpoint,
                                        bool deleted);
    void           retryPendingStarts();
    StartReadiness startReadiness(const QString &id) const;
    Session       *startSession(const Jid &requester, const QString &id);

    class Private;
    std::unique_ptr<Private> d;
};

class IRIS_EXPORT PublishedSessionRequest : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Pending, Succeeded, Failed };

    ~PublishedSessionRequest() override;

    State         state() const;
    Jid           publisher() const;
    QString       publicationId() const;
    QString       sid() const;
    Stanza::Error error() const;

    void start();

signals:
    void finished();

private:
    friend class PublicationManager;
    PublishedSessionRequest(PublicationManager *manager, const Jid &publisher, const QString &publicationId,
                            QObject *parent = nullptr);

    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP::Jingle

#endif // XMPP_JINGLE_PUB_H
