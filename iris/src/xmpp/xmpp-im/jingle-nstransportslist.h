/*
 * jignle-nstransportslist.h - Simple transport selector based on sorted namespaces list
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

#ifndef JINGLENSTRANSPORTSLIST_H
#define JINGLENSTRANSPORTSLIST_H

#include "jingle-transport.h"

namespace XMPP { namespace Jingle {

    class Session;
    class NSTransportsList : public TransportSelector {
    public:
        inline NSTransportsList(Session *session, const QStringList &transports) :
            _session(session), _transports(transports)
        {
        }

        QSharedPointer<Transport> getNextTransport() override;
        QSharedPointer<Transport> getAlikeTransport(QSharedPointer<Transport> alike) override;

        QSharedPointer<Transport> getNextNSTransport(const QString &preferredNS = QString());

        void backupTransport(QSharedPointer<Transport>) override;
        bool hasMoreTransports() const override;
        bool hasTransport(QSharedPointer<Transport>) const override;
        int  compare(QSharedPointer<Transport> a, QSharedPointer<Transport> b) const override;
        bool replace(QSharedPointer<Transport> old, QSharedPointer<Transport> newer) override;

    private:
        Session *   _session = nullptr;
        QStringList _transports;
    };

}}
#endif
