/*
 * jignle-nstransportslist.cpp - Simple transport selector based on sorted namespaces list
 * Copyright (C) 2020  Sergey Ilinykh
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

#include "jingle-nstransportslist.h"
#include "jingle-session.h"

namespace XMPP { namespace Jingle {

    QSharedPointer<Transport> NSTransportsList::getNextTransport(QSharedPointer<Transport> alike)
    {
        return getNextTransport(alike->pad()->ns());
    }

    QSharedPointer<Transport> NSTransportsList::getNextTransport(const QString &preferredNS)
    {
        if (_transports.isEmpty()) {
            return QSharedPointer<Transport>();
        }

        int idx = _transports.indexOf(preferredNS);
        if (idx == -1)
            idx = _transports.size() - 1;

        do {
            auto t = _session->newOutgoingTransport(_transports[idx]);
            if (t) {
                return t;
            }
            _transports.removeAt(idx);
            idx = _transports.size() - 1;
        } while (idx != -1);

        return QSharedPointer<Transport>();
    }

    void NSTransportsList::backupTransport(QSharedPointer<Transport> tr) { _transports += tr->pad()->ns(); }

    bool NSTransportsList::hasMoreTransports() const { return !_transports.isEmpty(); }

    bool NSTransportsList::hasTransport(QSharedPointer<Transport>) const { return false; }

    int NSTransportsList::compare(QSharedPointer<Transport> a, QSharedPointer<Transport> b) const
    {
        auto idxA = _transports.indexOf(a->pad()->ns());
        auto idxB = _transports.indexOf(b->pad()->ns());
        return idxA < idxB ? -1 : idxA > idxB ? 1 : 0;
    }

}}
