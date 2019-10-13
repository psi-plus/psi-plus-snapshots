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

#ifndef XMPP_LIVEROSTER_H
#define XMPP_LIVEROSTER_H

#include "xmpp_liverosteritem.h"

#include <QList>

namespace XMPP {
class Jid;

class LiveRoster : public QList<LiveRosterItem> {
public:
    LiveRoster();
    LiveRoster(const LiveRoster &other);
    ~LiveRoster();

    LiveRoster &operator=(const LiveRoster &other);

    void                      flagAllForDelete();
    LiveRoster::Iterator      find(const Jid &, bool compareRes = true);
    LiveRoster::ConstIterator find(const Jid &, bool compareRes = true) const;

    void    setGroupsDelimiter(const QString &groupsDelimiter);
    QString groupsDelimiter() const;

private:
    class Private;
    Private *d;
};
} // namespace XMPP

#endif // XMPP_LIVEROSTER_H
