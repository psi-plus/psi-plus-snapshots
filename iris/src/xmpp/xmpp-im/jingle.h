/*
 * jignle.h - General purpose Jingle
 * Copyright (C) 2019  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef JINGLE_H
#define JINGLE_H

#include "xmpp_hash.h"

#include <QSharedDataPointer>
#include <QSharedPointer>
#include <functional>

class QDomElement;
class QDomDocument;

namespace XMPP {
class Client;

namespace Jingle {

extern const QString NS;

class Manager;
class Session;

enum class Origin {
    None,
    Both,
    Initiator,
    Responder
};

class Jingle
{
public:
    enum Action {
        NoAction, // non-standard, just a default
        ContentAccept,
        ContentAdd,
        ContentModify,
        ContentReject,
        ContentRemove,
        DescriptionInfo,
        SecurityInfo,
        SessionAccept,
        SessionInfo,
        SessionInitiate,
        SessionTerminate,
        TransportAccept,
        TransportInfo,
        TransportReject,
        TransportReplace
    };

    Jingle(); // make invalid jingle element
    Jingle(Action action, const QString &sid); // start making outgoing jingle
    Jingle(const QDomElement &e); // likely incoming
    Jingle(const Jingle &);
    ~Jingle();

    QDomElement toXml(QDomDocument *doc) const;
    inline bool isValid() const { return d != nullptr; }
    Action action() const;
    const QString &sid() const;
    const Jid &initiator() const;
    void setInitiator(const Jid &jid);
    const Jid &responder() const;
    void setResponder(const Jid &jid);
private:
    class Private;
    QSharedDataPointer<Private> d;
    Jingle::Private *ensureD();
};

class Reason {
    class Private;
public:
    enum Condition
    {
        NoReason = 0, // non-standard, just a default
        AlternativeSession,
        Busy,
        Cancel,
        ConnectivityError,
        Decline,
        Expired,
        FailedApplication,
        FailedTransport,
        GeneralError,
        Gone,
        IncompatibleParameters,
        MediaError,
        SecurityError,
        Success,
        Timeout,
        UnsupportedApplications,
        UnsupportedTransports
    };

    Reason();
    ~Reason();
    Reason(Condition cond);
    Reason(const QDomElement &el);
    Reason(const Reason &other);
    inline bool isValid() const { return d != nullptr; }
    Condition condition() const;
    void setCondition(Condition cond);
    QString text() const;
    void setText(const QString &text);

    QDomElement toXml(QDomDocument *doc) const;

private:
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

class ContentBase {
public:

    inline ContentBase(){}
    ContentBase(Origin creator, const QString &name);
    ContentBase(const QDomElement &el);

    inline bool isValid() const { return creator != Origin::None && !name.isEmpty(); }

    QDomElement toXml(QDomDocument *doc, const char *tagName) const;
    static Origin creatorAttr(const QDomElement &el);
    static bool setCreatorAttr(QDomElement &el, Origin creator);

    Origin  creator = Origin::None;
    QString name;
    Origin  senders = Origin::Both;
    QString disposition; // default "session"
};

class Transport : public QObject {
    Q_OBJECT
public:
    /*
    Categorization by speed, reliability and connectivity
    - speed: realtim, fast, slow
    - reliability: reliable, not reliable (some transport can both modes)
    - connectivity: always connect, hard to connect

    Some transports may change their qualities, so we have to consider worst case.

    ICE-UDP: RealTime, Not Reliable, Hard To Connect
    S5B:     Fast,     Reliable,     Hard To Connect
    IBB:     Slow,     Reliable,     Always Connect

    Also most of transports may add extra features but it's matter of configuration.
    For example all of them can enable p2p crypto mode (<security/> should work here)
    */
    enum Feature {
        // connection establishment
        HardToConnect = 0x01,  // anything else but ibb
        AlwaysConnect = 0x02,  // ibb. basically it's always connected

        // reliability
        NotReliable   = 0x10,  // datagram-oriented
        Reliable      = 0x20,  // connection-orinted

        // speed.
        Slow          = 0x100, // only ibb is here probably
        Fast          = 0x200, // basically all tcp-based and reliable part of sctp
        RealTime      = 0x400  // it's rather about synchronization of frames with time which implies fast
    };
    Q_DECLARE_FLAGS(Features, Feature)


    using QObject::QObject;

    enum Direction { // incoming or outgoing file/data transfer.
        Outgoing,
        Incoming
    };

    virtual void start() = 0; // for local transport start searching for candidates (including probing proxy,stun etc)
                         // for remote transport try to connect to all proposed hosts in order their priority.
                         // in-band transport may just emit updated() here
    virtual bool update(const QDomElement &el) = 0; // accepts transport element on incoming transport-info
    virtual Jingle::Action outgoingUpdateType() const = 0;
    virtual QDomElement takeOutgoingUpdate() = 0;
    virtual bool isValid() const = 0;
    virtual Features features() const = 0;
signals:
    void updated(); // found some candidates and they have to be sent. takeUpdate has to be called from this signal handler.
                    // if it's just always ready then signal has to be sent at least once otherwise session-initiate won't be sent.
    void connected(); // this signal is for app logic. maybe to finally start drawing some progress bar
};

class Application : public QObject
{
    Q_OBJECT
public:

    enum SetDescError {
        Ok,
        Unparsed,
        IncompatibleParameters // this one is for <reason>
    };

    /**
     * @brief setTransport checks if transport is compatible and stores it
     * @param transport
     * @return false if not compatible
     */
    virtual QString contentName() const = 0;
    virtual SetDescError setDescription(const QDomElement &description) = 0;
    virtual bool setTransport(const QSharedPointer<Transport> &transport) = 0;
    virtual QSharedPointer<Transport> transport() const = 0;
    virtual Jingle::Action outgoingUpdateType() const = 0;
    virtual bool isReadyForSessionAccept() const = 0; // has connected transport for example
    virtual QDomElement takeOutgoingUpdate() = 0; // this may return something only when outgoingUpdateType() != NoAction
    virtual QDomElement sessionAcceptContent() const = 0; // for example has filtered ice candidates (only connected)
    virtual bool wantBetterTransport(const QSharedPointer<Transport> &) const = 0;
};

class Security
{

};

/**
 * @brief The SessionManagerPad class - TransportManager/AppManager PAD
 *
 * The class is intended to be used to monitor global session events
 * as well as send them in context of specific application type.
 *
 * For example a session has 3 content elements (voice, video and whiteboard).
 * voice and video are related to RTP application while whiteaboard (Jingle SXE)
 * is a different application. Therefore the session will have 2 pads:
 * rtp pad and whitebaord pad.
 * The pads are connected to both session and transport/application manager
 * and their main task to handle Jingle session-info events.
 *
 * SessionManagerPad is a base class for all kinds of pads.
 * UI can connect to its signals.
 */
class SessionManagerPad : public QObject
{
    Q_OBJECT
public:
    virtual QDomElement takeOutgoingSessionInfoUpdate();
    virtual QString ns() const = 0;
    virtual Session *session() const = 0;
};

class TransportManagerPad : public SessionManagerPad
{
    Q_OBJECT
public:
    typedef QSharedPointer<TransportManagerPad> Ptr;
};

class ApplicationManagerPad : public SessionManagerPad
{
    Q_OBJECT
public:
    typedef QSharedPointer<ApplicationManagerPad> Ptr;

    using SessionManagerPad::SessionManagerPad;

    //const QString &
    /*
     * for example we transfer a file
     * then first file may generate name "file1", next "file2" etc
     * As result it will be sent as <content name="file1" ... >
     */
    //QString newContentName();
};

class Session : public QObject
{
    Q_OBJECT
public:
    enum State {
        Starting,
        Unacked,
        Pending,
        Active,
        Ended
    };

    Session(Manager *manager);
    ~Session();

    State state() const;
    Jid peer() const;
    Origin role() const; // my role in session: initiator or responder
    XMPP::Stanza::Error lastError() const;

    ApplicationManagerPad::Ptr applicationPad(const QString &ns);
    TransportManagerPad::Ptr transportPad(const QString &ns);

    QString preferredApplication() const;
    QStringList allApplicationTypes() const;

    void initiate();
    void reject();

    // allocates or returns existing pads
    ApplicationManagerPad::Ptr applicationPadFactory(const QString &ns);
    TransportManagerPad::Ptr transportPadFactory(const QString &ns);
signals:
    void managerPadAdded(const QString &ns);

private:
    friend class Manager;
    friend class JTPush;
    bool incomingInitiate(const Jid &from, const Jingle &jingle, const QDomElement &jingleEl);
    bool updateFromXml(Jingle::Action action, const QDomElement &jingleEl);

    class Private;
    QScopedPointer<Private> d;
};

class ApplicationManager : public QObject
{
    Q_OBJECT
public:
    ApplicationManager(QObject *parent = nullptr);

    virtual void setJingleManager(Manager *jm) = 0;
    virtual Application* startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName, Origin creator, Origin senders) = 0;
    virtual ApplicationManagerPad *pad(Session *session) = 0;

    // this method is supposed to gracefully close all related sessions as a preparation for plugin unload for example
    virtual void closeAll() = 0;
};

class TransportManager : public QObject
{
    Q_OBJECT
public:

    TransportManager(QObject *parent = nullptr);

    virtual void setJingleManager(Manager *jm) = 0;
    virtual QSharedPointer<Transport> sessionInitiate(const TransportManagerPad::Ptr &pad, const Jid &to) = 0; // outgoing. one have to call Transport::start to collect candidates
    virtual QSharedPointer<Transport> sessionInitiate(const TransportManagerPad::Ptr &pad, const Jid &from, const QDomElement &transportEl) = 0; // incoming
    virtual TransportManagerPad* pad(Session *session) = 0;

    // this method is supposed to gracefully close all related sessions as a preparation for plugin unload for example
    virtual void closeAll() = 0;
};

class Manager : public QObject
{
    Q_OBJECT

public:
    explicit Manager(XMPP::Client *client = 0);
    ~Manager();

    XMPP::Client* client() const;

    void setRedirection(const Jid &to);
    const Jid &redirectionJid() const;

    void registerApp(const QString &ns, ApplicationManager *app);
    void unregisterApp(const QString &ns);
    bool isRegisteredApplication(const QString &ns);
    Application* startApplication(ApplicationManagerPad::Ptr pad, const QString &contentName, Origin creator, Origin senders);
    ApplicationManagerPad* applicationPad(Session *session, const QString &ns); // allocates new pad on application manager

    void registerTransport(const QString &ns, TransportManager *transport);
    void unregisterTransport(const QString &ns);
    QSharedPointer<Transport> initTransport(const TransportManagerPad::Ptr &pad, const Jid &jid, const QDomElement &el);
    TransportManagerPad* transportPad(Session *session, const QString &ns); // allocates new pad on transport manager

    /**
     * @brief isAllowedParty checks if the remote jid allowed to initiate a session
     * @param jid - remote jid
     * @return true if allowed
     */
    bool isAllowedParty(const Jid &jid) const;
    void setRemoteJidChecked(std::function<bool(const Jid &)> checker);


    Session* session(const Jid &remoteJid, const QString &sid);
    Session* newSession(const Jid &j);
    XMPP::Stanza::Error lastError() const;

signals:
    void incomingSession(Session *);

private:
    friend class JTPush;
    Session *incomingSessionInitiate(const Jid &initiator, const Jingle &jingle, const QDomElement &jingleEl);

    class Private;
    QScopedPointer<Private> d;
};

} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_H
