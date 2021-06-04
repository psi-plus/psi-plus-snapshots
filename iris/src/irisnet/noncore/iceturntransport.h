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

#ifndef ICETURNTRANSPORT_H
#define ICETURNTRANSPORT_H

#include "icetransport.h"
#include "turnclient.h"

#include <QByteArray>
#include <QEnableSharedFromThis>
#include <QHostAddress>
#include <QObject>

namespace XMPP {
// for the turn transport, only path 0 is used
class IceTurnTransport : public IceTransport, public QEnableSharedFromThis<IceTurnTransport> {
    Q_OBJECT

public:
    enum Error { ErrorTurn = ErrorCustom };

    IceTurnTransport(QObject *parent = nullptr);
    ~IceTurnTransport();

    void setClientSoftwareNameAndVersion(const QString &str);

    // set these before calling start()
    void setUsername(const QString &user);
    void setPassword(const QCA::SecureArray &pass);

    void setProxy(const TurnClient::Proxy &proxy);

    void start(const TransportAddress &addr, TurnClient::Mode mode = TurnClient::PlainMode);

    const TransportAddress &relayedAddress() const;
    const TransportAddress &reflexiveAddress() const;
    bool                    isStarted() const;

    TurnClient::Error turnErrorCode() const;

    // reimplemented
    virtual void       stop() override;
    virtual bool       hasPendingDatagrams(int path) const override;
    virtual QByteArray readDatagram(int path, TransportAddress &addr) override;
    virtual void       writeDatagram(int path, const QByteArray &buf, const TransportAddress &addr) override;
    virtual void       addChannelPeer(const TransportAddress &addr) override;
    virtual void       setDebugLevel(DebugLevel level) override;
    virtual void       changeThread(QThread *thread) override;

private:
    class Private;
    friend class Private;
    Private *d;
};
} // namespace XMPP

#endif // ICETURNTRANSPORT_H
