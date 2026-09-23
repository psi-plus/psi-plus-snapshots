/*
 * jignle-session.h - Jingle Session
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

#ifndef JINGLE_SESSION_H
#define JINGLE_SESSION_H

#include <iris/iris_export.h>

#include <iris/xmpp-im/jingle-application.h>
#include <iris/xmpp-im/jingle-tiebreaker.h>
#include <iris/xmpp-im/jingle-transport.h>
#include <iris/xmpp-im/xmpp_features.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <QSet>

namespace XMPP { namespace Jingle {

    // class Manager;
    class Application;

    // Ordered XEP-0338 group. Multiple groups may have the same semantics.
    struct ContentGroup {
        QString     semantics;
        QStringList contents;
    };

    class IRIS_EXPORT Session : public QObject {
        Q_OBJECT
    public:
        // Incoming sessions are not registered in Jingle Manager until their initial contents are validated. A valid
        // incoming session remains in Created state while it waits for local accept() or terminate().

        Session(Manager *manager, const Jid &peer, Origin role = Origin::Initiator);
        ~Session();

        Manager *manager() const;
        State    state() const;

        Jid     me() const;
        Jid     peer() const;
        Jid     initiator() const;
        Jid     responder() const;
        QString sid() const;

        Origin   role() const; // my role in session: initiator or responder
        Origin   peerRole() const;
        bool     checkPeerCaps(const QString &ns) const;
        Features peerFeatures() const;

        bool isGroupingAllowed() const;
        // Capability-driven initial grouping is enabled by default. Explicit
        // setGroupings()/setGrouping() calls take ownership of local grouping
        // policy for this session and suppress further automatic changes.
        void setAutomaticGroupingEnabled(bool enabled);
        bool automaticGroupingEnabled() const;

        std::optional<Stanza::Error> lastError() const;

        TieBreaker       *tieBreaker();
        const TieBreaker *tieBreaker() const;

        /**
         * @brief Create a local application for a new Jingle content.
         * @param ns application description namespace
         * @param senders value represented by the Jingle content `senders` attribute
         * @return a new application, or nullptr if the namespace is not registered
         *
         * The returned application is not added to the session yet. Configure it first, then pass it to
         * addContent().
         */
        Application *newContent(const QString &ns, Origin senders = Origin::Both);
        // get registered content if any
        Application *content(const QString &contentName, Origin creator);

        /**
         * @brief Add a previously created local application to the session.
         *
         * The session takes responsibility for the application lifetime. If negotiation has already started, the
         * application is prepared immediately so it can be sent as content-add.
         */
        void                                   addContent(Application *content);
        const QMap<ContentKey, Application *> &contentList() const;
        void                                   setGrouping(const QString &groupType, const QStringList &group);
        // Local signaling proposal, not proof of an established shared transport.
        bool                setGroupings(const QList<ContentGroup> &groups);
        QList<ContentGroup> groupings() const;
        // Last successfully parsed initial peer offer/answer. Automatic local
        // grouping may accept a compatible subset while preserving this peer snapshot.
        QList<ContentGroup> remoteGroupings() const;

        ApplicationManagerPad::Ptr applicationPad(const QString &ns);
        TransportManagerPad::Ptr   transportPad(const QString &ns);

        QSharedPointer<Transport> newOutgoingTransport(const QString &ns);

        QString     preferredApplication() const;
        QStringList allApplicationTypes() const;

        void setLocalJid(const Jid &jid); // w/o real use case the implementation is rather stub

        /// Accept a validated incoming session and start preparing its initial contents for session-accept.
        void accept();

        /// Start preparing an outgoing session. session-initiate is sent when all initial contents are ready.
        void initiate();
        void terminate(Reason::Condition cond, const QString &comment = QString());

        // allocates or returns existing pads
        ApplicationManagerPad::Ptr applicationPadFactory(const QString &ns);
        TransportManagerPad::Ptr   transportPadFactory(const QString &ns);
    signals:
        void managerPadAdded(const QString &ns);
        void initiated();

        /**
         * Emitted when the initial Jingle negotiation has been accepted and applications are allowed to start their
         * transports. It does not mean that every application Connection is already active.
         */
        void activated();
        void terminated();
        void newContentReceived();

    private:
        friend class Application;
        friend class Manager;
        friend class PublicationManager;
        friend class JTPush;

        QString reserveSid(const QString &requestedSid = QString());
        bool    incomingInitiate(const Jingle &jingle, const QDomElement &jingleEl);
        // Dispatcher must invoke afterReply only after sending the incoming IQ reply.
        bool updateFromXml(Action action, const QDomElement &jingleEl, std::function<void()> *afterReply = nullptr);
        static std::optional<QList<ContentGroup>> parseGroupings(const QDomElement &jingleEl);
        static bool validBundleAnswer(const QList<ContentGroup> &offer, const QList<ContentGroup> &answer);
        bool        validLocalGroupings() const;
        void        refreshAutomaticGroupings(const QSet<Application *> &excluded = {});

        TieBreaker tieBreaker_;

        class Private;
        std::unique_ptr<Private> d;
    };
}}

#endif // JINGLE_SESSION_H
