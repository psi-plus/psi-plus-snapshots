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

#include "xmpp_stanza.h"

#include <QSharedDataPointer>
#include <QSharedPointer>
#include <functional>

class QDomDocument;
class QDomElement;

namespace XMPP {
class Client;

namespace Jingle {
    extern const QString NS;

    class Manager;
    class Session;

    enum class Origin { None, Both, Initiator, Responder };

    inline uint qHash(const XMPP::Jingle::Origin &o, uint seed = 0) { return ::qHash(int(o), seed); }

    /*
     Session states:
      * Created           - new session
      * ApprovedToSend    - user accepted session but it's not yet ready for session-initiate/accept message
      * Unacked           - session-initiate/accept was sent. wait for IQ ack
      * Pending           - local only: session-initiate was acknowledged. awaits session-accept.
      * Active            - session was accepted and now active.
      * Finihed           - session-terminate was sent/received

     Local app states:
      * Created           - after constructor till local user initiates the sending
      * ApprovedToSend    - user already clicked "send" but our offer is not ready yet (collecting candidates)
      * Unacked           - initial offer is sent but no iq result yet
      * Pending           - got iq result but no accept (answer) request yet
      * Accepted          - remote accepted the app. waiting for start() (for example when all session is accepted)
      * Connecting        - session was accepted (or content-accepted for early-session). negotiating connection
      * Active            - connection was established. now real data passes.
      * Finishing         - need to send some final statuses over signalling
      * Finished          - app was removed from session

     Remote app states:
      * Created           - after constructor till local user accepts the app
      * ApprovedToSend    - user already accepted but our answer is not ready yet (collecting candidates)
      * Unacked           - the answer is sent but no iq result yet
      * Accepted          - waiting for start() (for example when all session is accepted)
      * Connecting        - session was accepted (or content-accepted for early-session). negotiating connection
      * Active            - connection was established. now real data passes.
      * Finishing         - need to send some final statuses over signalling
      * Finished          - app was removed from session

     Local transport states (our initial offer or our outgoing transport-replace):
      * Created           - initial: just after constructor but before "send" button was pushed.
      *                     replace: if previous was > Created then replace will start right from ApprovedToSend
      * ApprovedToSend    - initial: we are preparing initial offer ("send" was pushed already)
      *                     replace: we are going to replace previously sent trasport offer. preparing new one
      * Unacked           - no iq "result" yet
      * Pending           - got iq result but no any kind of transport accept
      * Accepted          - session/content/transport-replace accepted. app should start negotiation explicitly
      * Connecting        - connetion negotiation
      * Active            - traferring data
      * Finished          - In failure case: Needs to report transport failure / replace / reject

     Remote transport states (remote initial offer or remote transport-replace):
      * Created           - initial: local user hasn't accepted yet the offer
      *                     replace: remote changes its own offer before local accepted anything
      * ApprovedToSend    - initial/replace: user accepted the offer. we are preparing our response
      * Unacked           - no iq "result" yet
      * Accepted          - session/content/transport-replace accepted. app should start negotiation explicitly
      * Connecting        - connetion negotiation
      * Active            - traferring data
      * Finished          - In failure case: Needs to report transport failure / replace / reject

     Locally initiated session passes all the above and remotely initiated skips Pending.
    */
    enum class State { Created, ApprovedToSend, Unacked, Pending, Accepted, Connecting, Active, Finishing, Finished };

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

    inline uint qHash(const XMPP::Jingle::Action &o, uint seed = 0) { return ::qHash(int(o), seed); }

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
    enum class TransportFeature {
        // connection establishment
        HardToConnect = 0x01, // anything else but ibb
        AlwaysConnect = 0x02, // ibb. basically it's always connected

        // reliability
        NotReliable = 0x10, // datagram-oriented
        Reliable    = 0x20, // connection-orinted

        // speed.
        Slow     = 0x100, // only ibb is here probably
        Fast     = 0x200, // basically all tcp-based and reliable part of sctp
        RealTime = 0x400  // it's rather about synchronization of frames with time which implies fast
    };
    Q_DECLARE_FLAGS(TransportFeatures, TransportFeature)

    typedef QPair<QString, Origin>            ContentKey;
    typedef std::function<void(bool success)> OutgoingUpdateCB;
    typedef std::tuple<QList<QDomElement>, OutgoingUpdateCB>
        OutgoingUpdate; // list of elements to b inserted to <jingle> and success callback
    typedef std::tuple<QDomElement, OutgoingUpdateCB>
        OutgoingTransportInfoUpdate; // transport element and success callback

    class ErrorUtil {
    public:
        enum {
            UnknownError, // unparsed/unknown error
            OutOfOrder,
            TieBreak,
            UnknownSession,
            UnsupportedInfo,
            Last
        };

        static const char *names[Last];

        static XMPP::Stanza::Error make(QDomDocument &doc, int jingleCond, int type = XMPP::Stanza::Error::Cancel,
                                        int            condition = XMPP::Stanza::Error::UndefinedCondition,
                                        const QString &text      = QString());

        static XMPP::Stanza::Error makeTieBreak(QDomDocument &doc);
        static XMPP::Stanza::Error makeOutOfOrder(QDomDocument &doc);

        static void fill(QDomDocument doc, XMPP::Stanza::Error &error, int jingleCond);
        static int  jingleCondition(const XMPP::Stanza::Error &error);
    };

    class Jingle {
    public:
        Jingle();                                  // make invalid jingle element
        Jingle(Action action, const QString &sid); // start making outgoing jingle
        Jingle(const QDomElement &e);              // likely incoming
        Jingle(const Jingle &);
        ~Jingle();

        QDomElement    toXml(QDomDocument *doc) const;
        inline bool    isValid() const { return d != nullptr; }
        Action         action() const;
        const QString &sid() const;
        const Jid &    initiator() const;
        void           setInitiator(const Jid &jid);
        const Jid &    responder() const;
        void           setResponder(const Jid &jid);

    private:
        class Private;
        QSharedDataPointer<Private> d;
        Jingle::Private *           ensureD();
    };

    class Reason {
        class Private;

    public:
        enum Condition {
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
        Reason &    operator=(const Reason &);
        inline bool isValid() const { return d != nullptr; }
        Condition   condition() const;
        void        setCondition(Condition cond);
        QString     text() const;
        void        setText(const QString &text);

        QDomElement toXml(QDomDocument *doc) const;

    private:
        Private *ensureD();

        QSharedDataPointer<Private> d;
    };

    class ContentBase {
    public:
        inline ContentBase() {}
        ContentBase(Origin creator, const QString &name);
        ContentBase(const QDomElement &el);

        inline bool isValid() const { return creator != Origin::None && !name.isEmpty(); }

        QDomElement   toXml(QDomDocument *doc, const char *tagName, const QString &ns = QString()) const;
        static Origin creatorAttr(const QDomElement &el);
        static bool   setCreatorAttr(QDomElement &el, Origin creator);

        Origin  creator = Origin::None;
        QString name;
        Origin  senders = Origin::Both;
        QString disposition; // default "session"
    };

    class Security {
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
    class SessionManagerPad : public QObject {
        Q_OBJECT
    public:
        virtual QDomElement takeOutgoingSessionInfoUpdate();
        virtual QString     ns() const      = 0;
        virtual Session *   session() const = 0;
        QDomDocument *      doc() const;
    };

    class ApplicationManager;
    class ApplicationManagerPad;
    class TransportManager;
    class TransportManagerPad;
    class Manager : public QObject {
        Q_OBJECT

    public:
        explicit Manager(XMPP::Client *client = nullptr);
        ~Manager();

        XMPP::Client *client() const;

        // if we have another jingle manager we can add its contents' namespaces here.
        void addExternalManager(const QString &ns);
        // on outgoing session destroy an external manager should call this function.
        void registerExternalSession(const QString &sid);
        void forgetExternalSession(const QString &sid);

        void       setRedirection(const Jid &to);
        const Jid &redirectionJid() const;

        void                   registerApp(const QString &ns, ApplicationManager *app);
        void                   unregisterApp(const QString &ns);
        bool                   isRegisteredApplication(const QString &ns);
        ApplicationManagerPad *applicationPad(Session *      session,
                                              const QString &ns); // allocates new pad on application manager

        void                 registerTransport(const QString &ns, TransportManager *transport);
        void                 unregisterTransport(const QString &ns);
        bool                 isRegisteredTransport(const QString &ns);
        TransportManagerPad *transportPad(Session *      session,
                                          const QString &ns); // allocates new pad on transport manager
        QStringList          availableTransports(const TransportFeatures &features = TransportFeatures()) const;

        /**
         * @brief isAllowedParty checks if the remote jid allowed to initiate a session
         * @param jid - remote jid
         * @return true if allowed
         */
        bool isAllowedParty(const Jid &jid) const;
        void setRemoteJidChecker(std::function<bool(const Jid &)> checker);

        Session *           session(const Jid &remoteJid, const QString &sid);
        Session *           newSession(const Jid &j);
        QString             registerSession(Session *session);
        XMPP::Stanza::Error lastError() const;

        void detachSession(Session *s); // disconnect the session from manager
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

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::Jingle::TransportFeatures)

#endif // JINGLE_H
