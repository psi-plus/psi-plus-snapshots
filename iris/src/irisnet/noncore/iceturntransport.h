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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef ICETURNTRANSPORT_H
#define ICETURNTRANSPORT_H

#include <QObject>
#include <QByteArray>
#include <QHostAddress>
#include "turnclient.h"
#include "icetransport.h"

namespace XMPP {

// for the turn transport, only path 0 is used

class IceTurnTransport : public IceTransport
{
	Q_OBJECT

public:
	enum Error
	{
		ErrorTurn = ErrorCustom
	};

	IceTurnTransport(QObject *parent = 0);
	~IceTurnTransport();

	void setClientSoftwareNameAndVersion(const QString &str);

	// set these before calling start()
	void setUsername(const QString &user);
	void setPassword(const QCA::SecureArray &pass);

	void setProxy(const TurnClient::Proxy &proxy);

	void start(const QHostAddress &addr, int port, TurnClient::Mode mode = TurnClient::PlainMode);

	QHostAddress relayedAddress() const;
	int relayedPort() const;

	TurnClient::Error turnErrorCode() const;

	// reimplemented
	virtual void stop();
	virtual bool hasPendingDatagrams(int path) const;
	virtual QByteArray readDatagram(int path, QHostAddress *addr, int *port);
	virtual void writeDatagram(int path, const QByteArray &buf, const QHostAddress &addr, int port);
	virtual void addChannelPeer(const QHostAddress &addr, int port);
	virtual void setDebugLevel(DebugLevel level);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
