/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#ifndef ICETRANSPORT_H
#define ICETRANSPORT_H

#include <QByteArray>
#include <QObject>
#include <QWeakPointer>

class QHostAddress;

namespace XMPP {
class IceTransport : public QObject {
    Q_OBJECT

public:
    enum Error { ErrorGeneric, ErrorCustom };

    enum DebugLevel { DL_None, DL_Info, DL_Packet };

    IceTransport(QObject *parent = nullptr);
    ~IceTransport();

    virtual void stop() = 0;

    virtual bool       hasPendingDatagrams(int path) const                                                = 0;
    virtual QByteArray readDatagram(int path, QHostAddress *addr, quint16 *port)                          = 0;
    virtual void       writeDatagram(int path, const QByteArray &buf, const QHostAddress &addr, int port) = 0;
    virtual void       addChannelPeer(const QHostAddress &addr, int port)                                 = 0;

    virtual void setDebugLevel(DebugLevel level) = 0;
    virtual void changeThread(QThread *thread)   = 0;

signals:
    void started();
    void stopped(); // emitted when stop() finished cleaning up
    void error(int e);

    void readyRead(int path);
    void datagramsWritten(int path, int count, const QHostAddress &addr, int port);

    // not DOR-SS/DS safe
    void debugLine(const QString &str);
};

inline uint qHash(const QWeakPointer<IceTransport> &p) { return qHash(p.toStrongRef().data()); }

} // namespace XMPP

#endif // ICETRANSPORT_H
