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

#ifndef XMPP_ROSTER_H
#define XMPP_ROSTER_H

#include "xmpp_rosteritem.h"

#include <QList>

class QDomDocument;
class QDomElement;

namespace XMPP {
class Jid;

class Roster : public QList<RosterItem> {
public:
    Roster();
    Roster(const Roster &other);
    ~Roster();

    Roster &operator=(const Roster &other);

    Roster::Iterator      find(const Jid &);
    Roster::ConstIterator find(const Jid &) const;

    void    setGroupsDelimiter(const QString &groupsDelimiter);
    QString groupsDelimiter() const;

private:
    class Private;
    Private *d = nullptr;
};
} // namespace XMPP

#endif // XMPP_ROSTER_H
