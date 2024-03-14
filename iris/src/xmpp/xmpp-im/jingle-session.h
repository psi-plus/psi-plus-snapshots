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

#include "jingle-application.h"
#include "jingle-transport.h"
#include "xmpp_features.h"

namespace XMPP { namespace Jingle {

    // class Manager;
    class Application;

    class Session : public QObject {
        Q_OBJECT
    public:
        // Note incoming session are not registered in Jingle Manager until validated.
        // and then either rejected or registered in Pending state.

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

        XMPP::Stanza::Error lastError() const;

        // make new local content but do not add it to session yet
        Application *newContent(const QString &ns, Origin senders = Origin::Both);
        // get registered content if any
        Application                           *content(const QString &contentName, Origin creator);
        void                                   addContent(Application *content);
        const QMap<ContentKey, Application *> &contentList() const;
        void                                   setGrouping(const QString &groupType, const QStringList &group);

        ApplicationManagerPad::Ptr applicationPad(const QString &ns);
        TransportManagerPad::Ptr   transportPad(const QString &ns);

        QSharedPointer<Transport> newOutgoingTransport(const QString &ns);

        QString     preferredApplication() const;
        QStringList allApplicationTypes() const;

        void setLocalJid(const Jid &jid); // w/o real use case the implementation is rather stub

        void accept();
        void initiate();
        void terminate(Reason::Condition cond, const QString &comment = QString());

        // allocates or returns existing pads
        ApplicationManagerPad::Ptr applicationPadFactory(const QString &ns);
        TransportManagerPad::Ptr   transportPadFactory(const QString &ns);
    signals:
        void managerPadAdded(const QString &ns);
        void initiated();
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
}}

#endif // JINGLE_SESSION_H
