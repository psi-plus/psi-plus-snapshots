/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
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

#ifndef ICELOCALTRANSPORT_H
#define ICELOCALTRANSPORT_H

#include "icetransport.h"

#include <QByteArray>
#include <QEnableSharedFromThis>
#include <QObject>

class QHostAddress;
class QUdpSocket;

namespace QCA {
class SecureArray;
}

namespace XMPP {
// this class manages a single port on a single interface, including the
//   relationship with an associated STUN/TURN server.  if TURN is used, this
//   class offers two paths (0=direct and 1=relayed), otherwise it offers
//   just one path (0=direct)
class IceLocalTransport : public IceTransport, public QEnableSharedFromThis<IceLocalTransport> {
    Q_OBJECT

public:
    enum Error { ErrorBind = ErrorCustom };

    IceLocalTransport(QObject *parent = nullptr);
    ~IceLocalTransport();

    void setClientSoftwareNameAndVersion(const QString &str);

    // passed socket must already be bind()'ed, don't support
    //   ErrorMismatch retries
    void start(QUdpSocket *sock);

    // bind to this address on a random port, do support ErrorMismatch
    //   retries
    void start(const QHostAddress &addr);

    void setStunBindService(const QHostAddress &addr, int port);
    void setStunRelayService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass);

    QHostAddress stunBindServiceAddress() const;
    QHostAddress stunRelayServiceAddress() const;

    // obtain relay / reflexive
    void stunStart();

    QHostAddress localAddress() const;
    int          localPort() const;

    QHostAddress serverReflexiveAddress() const;
    int          serverReflexivePort() const;
    QHostAddress reflexiveAddressSource() const; // address of stun/turn server provided the srflx

    QHostAddress relayedAddress() const;
    int          relayedPort() const;

    // reimplemented
    void       stop() override;
    bool       hasPendingDatagrams(int path) const override;
    QByteArray readDatagram(int path, QHostAddress *addr, int *port) override;
    void       writeDatagram(int path, const QByteArray &buf, const QHostAddress &addr, int port) override;
    void       addChannelPeer(const QHostAddress &addr, int port) override;
    void       setDebugLevel(DebugLevel level) override;

signals:
    // may be emitted multiple times.
    // if handling internal ErrorMismatch, then local address may change
    //   and server reflexive address may disappear.
    // if start(QUdpSocket*) was used, then ErrorMismatch is not handled,
    //   and this signal will only be emitted to add addresses
    void addressesChanged();

private:
    class Private;
    friend class Private;
    Private *d;
};
} // namespace XMPP

#endif // ICELOCALTRANSPORT_H
