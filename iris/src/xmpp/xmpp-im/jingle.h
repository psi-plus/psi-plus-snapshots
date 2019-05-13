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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef JINGLE_H
#define JINGLE_H

#include "bytestream.h"

#include <QSharedDataPointer>
#include <QSharedPointer>
#include <functional>
#if QT_VERSION >= QT_VERSION_CHECK(5,8,0)
# include <QNetworkDatagram>
#endif

#include "xmpp_stanza.h"

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

inline uint qHash(const XMPP::Jingle::Origin &o, uint seed = 0)
{
    return ::qHash(int(o), seed);
}

/*
 Session states:
  * Created           - new session
  * PrepareLocalOffer - user accepted session but it's not yet ready for session-initiate/accept message
  * Unacked           - session-initiate/accept was sent. wait for IQ ack
  * Pending           - session-initiate was acknowledged. awaits session-accept.
  * Active            - session was accepted and now active.
  * Finihed           - session-terminate was sent/received

 Locally initiated session passes all the above and remotely initiated skips Pending.
*/
enum class State {
    Created,     // just after constructor
    PrepareLocalOffer, // content accepted by local user but we are not ready yet to send content-accept or session-accept.
                       // same for content-add/session-initiate, where user already already sent/added in ui and it's network turn.
    Unacked,     // local content offer is sent to remote but no IQ ack yet
    Pending,     // waits for session-accept or content-accept from remote
    Accepted,    // app only: local: "accept" received, waits for start(). remote: "accept" sent and acknowledged, waits for start()
    Connecting,  // app only: s5b/ice probes etc (particular application state. can be omited for other entities)
    Active,      // active transfer. transport is connected. For session it means it was accepted
    Finishing,   // app only: basically it's finished but has some pending operations. like sending content-reject/remove to remote
    Finished     // transfering is finished for whatever reason. no more signals/state changes etc. can be deleted
};

enum class Action {
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

inline uint qHash(const XMPP::Jingle::Action &o, uint seed = 0)
{
    return ::qHash(int(o), seed);
}

typedef QPair<QString,Origin> ContentKey;
typedef std::function<void ()> OutgoingUpdateCB;
typedef std::tuple<QList<QDomElement>, OutgoingUpdateCB> OutgoingUpdate; // list of elements to b inserted to <jingle> and success callback
typedef std::tuple<QDomElement, OutgoingUpdateCB> OutgoingTransportInfoUpdate; // transport element and success callback

class Jingle
{
public:
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
    Reason(Condition cond, const QString &text = QString());
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

class TransportManager;
class TransportManagerPad : public SessionManagerPad
{
    Q_OBJECT
public:
    typedef QSharedPointer<TransportManagerPad> Ptr;

    virtual TransportManager *manager() const = 0;
};

class ApplicationManager;
class ApplicationManagerPad : public SessionManagerPad
{
    Q_OBJECT
public:
    typedef QSharedPointer<ApplicationManagerPad> Ptr;

    using SessionManagerPad::SessionManagerPad;

    virtual ApplicationManager *manager() const = 0;

    /*
     * for example we transfer a file
     * then first file may generate name "file1", next "file2" etc
     * As result it will be sent as <content name="file1" ... >
     */
    virtual QString generateContentName(Origin senders) = 0;
};

#if QT_VERSION < QT_VERSION_CHECK(5,8,0)
// stub implementation
class NetworkDatagram
{
public:
    bool _valid = false;
    QByteArray _data;
    inline NetworkDatagram(const QByteArray &data, const QHostAddress &destinationAddress = QHostAddress(), quint16 port = 0) :
        _valid(true),
        _data(data)
    {
        Q_UNUSED(destinationAddress);
        Q_UNUSED(port)
    }
    inline NetworkDatagram(){}

    inline bool isValid() const { return _valid; }
    inline QByteArray data() const { return _data; }
}
#else
typedef QNetworkDatagram NetworkDatagram;
#endif

class Connection : public ByteStream
{
    Q_OBJECT
public:
    using Ptr = QSharedPointer<Connection>; // will be shared between transport and application
    virtual bool hasPendingDatagrams() const;
    virtual NetworkDatagram receiveDatagram(qint64 maxSize = -1);
};

class Application;
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

    /**
     * @brief prepare to send content-add/session-initiate
     *  When ready, the application first set update type to ContentAdd and then emit updated()
     */
    virtual void prepare() = 0;

    /**
     * @brief start really transfer data. starting with connection to remote candidates for example
     */
    virtual void start() = 0;   // for local transport start searching for candidates (including probing proxy,stun etc)
                                // for remote transport try to connect to all proposed hosts in order their priority.
                                // in-band transport may just emit updated() here
    virtual bool update(const QDomElement &el) = 0; // accepts transport element on incoming transport-info
    virtual bool hasUpdates() const = 0;
    virtual OutgoingTransportInfoUpdate takeOutgoingUpdate() = 0;
    virtual bool isValid() const = 0;
    virtual Features features() const = 0;
    virtual TransportManagerPad::Ptr pad() const = 0;
    virtual Connection::Ptr connection() const = 0; // returns established QIODevice-based connection
signals:
    void updated(); // found some candidates and they have to be sent. takeUpdate has to be called from this signal handler.
                    // if it's just always ready then signal has to be sent at least once otherwise session-initiate won't be sent.
    void connected(); // this signal is for app logic. maybe to finally start drawing some progress bar
    void failed();    // transport ailed for whatever reason. aborted for example
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

    virtual ApplicationManagerPad::Ptr pad() const = 0;
    virtual State state() const = 0;
    virtual void setState(State state) = 0; // likely just remember the state and not generate any signals

    virtual Origin creator() const = 0;
    virtual Origin senders() const = 0;
    virtual QString contentName() const = 0;
    virtual SetDescError setDescription(const QDomElement &description) = 0;

    /**
     * @brief setTransport checks if transport is compatible and stores it
     * @param transport
     * @return false if not compatible
     */
    virtual bool setTransport(const QSharedPointer<Transport> &transport) = 0;
    virtual QSharedPointer<Transport> transport() const = 0;
    virtual Action outgoingUpdateType() const = 0;
    virtual OutgoingUpdate takeOutgoingUpdate() = 0; // this may return something only when outgoingUpdateType() != NoAction
    virtual bool wantBetterTransport(const QSharedPointer<Transport> &) const = 0;
    virtual bool selectNextTransport() = 0;

    /**
     * @brief prepare to send content-add/session-initiate
     *  When ready, the application first set update type to ContentAdd and then emit updated()
     */
    virtual void prepare() = 0;
    virtual bool accept(const QDomElement &el) = 0; // remote accepted our content
    virtual void start() = 0;

signals:
    void updated(); // signal for session it has to send updates to remote. so it will follow with takeOutgoingUpdate() eventually
    void stateChanged(State);
};

class Session : public QObject
{
    Q_OBJECT
public:
    // Note incoming session are not registered in Jingle Manager until validated.
    // and then either rejected or registered in Pending state.

    Session(Manager *manager, const Jid &peer);
    ~Session();

    Manager* manager() const;
    State state() const;

    Jid me() const;
    Jid peer() const;
    Jid initiator() const;
    Jid responder() const;

    Origin role() const; // my role in session: initiator or responder
    XMPP::Stanza::Error lastError() const;

    // make new local content but do not add it to session yet
    Application *newContent(const QString &ns, Origin senders = Origin::Both);
    // get registered content if any
    Application *content(const QString &contentName, Origin creator);
    void addContent(Application *content);
    const QMap<ContentKey,Application*> &contentList() const;

    ApplicationManagerPad::Ptr applicationPad(const QString &ns);
    TransportManagerPad::Ptr transportPad(const QString &ns);

    QSharedPointer<Transport> newOutgoingTransport(const QString &ns);

    QString preferredApplication() const;
    QStringList allApplicationTypes() const;

    void setLocalJid(const Jid &jid); // w/o real use case the implementation is rather stub

    void accept();
    void initiate();
    void terminate(Reason::Condition cond, const QString &comment = QString());

    // allocates or returns existing pads
    ApplicationManagerPad::Ptr applicationPadFactory(const QString &ns);
    TransportManagerPad::Ptr transportPadFactory(const QString &ns);
signals:
    void managerPadAdded(const QString &ns);
    void activated();
    void terminated();
    void newContentReceived();

private:
    friend class Manager;
    friend class JTPush;
    bool incomingInitiate(const Jingle &jingle, const QDomElement &jingleEl);
    bool updateFromXml(Action action, const QDomElement &jingleEl);

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

    // may show more features than Transport instance. For example some transports may work in both reliable and not reliable modes
    virtual Transport::Features features() const = 0;
    virtual void setJingleManager(Manager *jm) = 0;

    // FIXME rename methods
    virtual QSharedPointer<Transport> newTransport(const TransportManagerPad::Ptr &pad) = 0; // outgoing. one have to call Transport::start to collect candidates
    virtual QSharedPointer<Transport> newTransport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl) = 0; // incoming
    virtual TransportManagerPad* pad(Session *session) = 0;

    // this method is supposed to gracefully close all related sessions as a preparation for plugin unload for example
    virtual void closeAll() = 0;
signals:
    void abortAllRequested(); // mostly used by transport instances to abort immediately
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
    ApplicationManagerPad* applicationPad(Session *session, const QString &ns); // allocates new pad on application manager

    void registerTransport(const QString &ns, TransportManager *transport);
    void unregisterTransport(const QString &ns);
    bool isRegisteredTransport(const QString &ns);
    TransportManagerPad* transportPad(Session *session, const QString &ns); // allocates new pad on transport manager
    QStringList availableTransports(const Transport::Features &features = Transport::Features()) const;

    /**
     * @brief isAllowedParty checks if the remote jid allowed to initiate a session
     * @param jid - remote jid
     * @return true if allowed
     */
    bool isAllowedParty(const Jid &jid) const;
    void setRemoteJidChecker(std::function<bool(const Jid &)> checker);

    Session* session(const Jid &remoteJid, const QString &sid);
    Session* newSession(const Jid &j);
    QString generateSessionId(const Jid &peer);
    XMPP::Stanza::Error lastError() const;

signals:
    void incomingSession(Session *);

private:
    friend class JTPush;
    Session *incomingSessionInitiate(const Jid &from, const Jingle &jingle, const QDomElement &jingleEl);

    class Private;
    QScopedPointer<Private> d;
};

Origin negateOrigin(Origin o);

} // namespace Jingle
} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::Jingle::Transport::Features)

#endif // JINGLE_H
