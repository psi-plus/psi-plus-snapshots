/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef STUNALLOCATE_H
#define STUNALLOCATE_H

#include <QHostAddress>
#include <QList>
#include <QObject>

#include "transportaddress.h"

class QByteArray;

namespace XMPP {
class StunMessage;
class StunTransactionPool;
class TransportAddress;

class StunAllocate : public QObject {
    Q_OBJECT

public:
    enum Error { ErrorGeneric, ErrorTimeout, ErrorAuth, ErrorRejected, ErrorProtocol, ErrorCapacity, ErrorMismatch };

    class Channel {
    public:
        TransportAddress address;

        Channel(const TransportAddress &_address) : address(_address) { }

        inline bool operator==(const Channel &other) const { return address == other.address; }
        inline bool operator!=(const Channel &other) const { return !operator==(other); }
    };

    StunAllocate(XMPP::StunTransactionPool *pool);
    ~StunAllocate();

    void setClientSoftwareNameAndVersion(const QString &str);

    void start();
    void start(const TransportAddress &addr); // use addr association
    void stop();

    QString serverSoftwareNameAndVersion() const;

    const TransportAddress &reflexiveAddress() const;
    const TransportAddress &relayedAddress() const;

    QList<QHostAddress> permissions() const;
    void                setPermissions(const QList<QHostAddress> &perms);

    QList<Channel> channels() const;
    void           setChannels(const QList<Channel> &channels);

    int packetHeaderOverhead(const TransportAddress &addr) const;

    QByteArray encode(const QByteArray &datagram, const TransportAddress &addr);
    QByteArray decode(const QByteArray &encoded, TransportAddress &addr);
    QByteArray decode(const StunMessage &encoded, TransportAddress &addr);

    QString errorString() const;

    static bool       containsChannelData(const quint8 *data, int size);
    static QByteArray readChannelData(const quint8 *data, int size);

signals:
    void started();
    void stopped();
    void error(XMPP::StunAllocate::Error e);

    // emitted after calling setPermissions()
    void permissionsChanged();

    // emitted after calling setChannels()
    void channelsChanged();

    // not DOR-SS/DS safe
    void debugLine(const QString &line);

private:
    Q_DISABLE_COPY(StunAllocate)

    class Private;
    friend class Private;
    Private *d;
};
} // namespace XMPP

#endif // STUNALLOCATE_H
