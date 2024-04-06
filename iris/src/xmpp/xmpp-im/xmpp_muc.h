/*
 * xmpp_muc.h
 * Copyright (C) 2006  Remko Troncon
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

#ifndef XMPP_MUC_H
#define XMPP_MUC_H

#include "xmpp/jid/jid.h"

#include <QDomElement>
#include <QString>

namespace XMPP {
class MUCItem {
public:
    enum Affiliation { UnknownAffiliation, Outcast, NoAffiliation, Member, Admin, Owner };
    enum Role { UnknownRole, NoRole, Visitor, Participant, Moderator };

    struct Actor {
        Jid     jid;
        QString nick;

        bool operator==(const Actor &o) const;
    };

    MUCItem(Role = UnknownRole, Affiliation = UnknownAffiliation);
    MUCItem(const QDomElement &);

    void setNick(const QString &);
    void setJid(const Jid &);
    void setAffiliation(Affiliation);
    void setRole(Role);
    void setActor(const Actor &);
    void setReason(const QString &);

    const QString &nick() const;
    const Jid     &jid() const; // real jid of muc participant
    Affiliation    affiliation() const;
    Role           role() const;
    const Actor   &actor() const;
    const QString &reason() const;

    void        fromXml(const QDomElement &);
    QDomElement toXml(QDomDocument &);

    bool operator==(const MUCItem &o) const;

private:
    QString     nick_;
    Jid         jid_;
    Actor       actor_;
    Affiliation affiliation_;
    Role        role_;
    QString     reason_;
};

class MUCInvite {
public:
    MUCInvite();
    MUCInvite(const QDomElement &);
    MUCInvite(const Jid &to, const QString &reason = QString());

    const Jid     &to() const;
    void           setTo(const Jid &);
    const Jid     &from() const;
    void           setFrom(const Jid &);
    const QString &reason() const;
    void           setReason(const QString &);
    bool           cont() const;
    void           setCont(bool);

    void        fromXml(const QDomElement &);
    QDomElement toXml(QDomDocument &) const;
    bool        isNull() const;

private:
    Jid     to_, from_;
    QString reason_, password_;
    bool    cont_;
};

class MUCDecline {
public:
    MUCDecline();
    MUCDecline(const Jid &to, const QString &reason);
    MUCDecline(const QDomElement &);

    const Jid     &to() const;
    void           setTo(const Jid &);
    const Jid     &from() const;
    void           setFrom(const Jid &);
    const QString &reason() const;
    void           setReason(const QString &);

    void        fromXml(const QDomElement &);
    QDomElement toXml(QDomDocument &) const;
    bool        isNull() const;

private:
    Jid     to_, from_;
    QString reason_;
};

class MUCDestroy {
public:
    MUCDestroy();
    MUCDestroy(const QDomElement &);

    const Jid     &jid() const;
    void           setJid(const Jid &);
    const QString &reason() const;
    void           setReason(const QString &);

    void        fromXml(const QDomElement &);
    QDomElement toXml(QDomDocument &) const;

private:
    Jid     jid_;
    QString reason_;
};
} // namespace XMPP

#endif // XMPP_MUC_H
