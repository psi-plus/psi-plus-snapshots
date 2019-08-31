/*
 * Copyright (C) 2003  Justin Karneges
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

#ifndef XMPP_ROSTERITEM_H
#define XMPP_ROSTERITEM_H

#include "xmpp/jid/jid.h"

#include <QString>
#include <QStringList>

namespace XMPP {
    class Subscription
    {
    public:
        enum SubType { None, To, From, Both, Remove };

        Subscription(SubType type=None);

        int type() const;

        QString toString() const;
        bool fromString(const QString &);

    private:
        SubType value;
    };

    class RosterItem
    {
    public:
        RosterItem(const Jid &jid="");
        RosterItem(const RosterItem &item);
        virtual ~RosterItem();
        RosterItem& operator=(const RosterItem& other) = default;

        const Jid & jid() const;
        const QString & name() const;
        const QStringList & groups() const;
        const Subscription & subscription() const;
        const QString & ask() const;
        bool isPush() const;
        bool inGroup(const QString &) const;

        virtual void setJid(const Jid &);
        void setName(const QString &);
        void setGroups(const QStringList &);
        void setSubscription(const Subscription &);
        void setAsk(const QString &);
        void setIsPush(bool);
        bool addGroup(const QString &);
        bool removeGroup(const QString &);

        QDomElement toXml(QDomDocument *) const;
        bool fromXml(const QDomElement &);

    private:
        Jid v_jid;
        QString v_name;
        QStringList v_groups;
        Subscription v_subscription;
        QString v_ask;
        bool v_push;
    };
} // namespace XMPP

#endif // XMPP_ROSTERITEM_H
