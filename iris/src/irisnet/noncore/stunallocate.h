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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef STUNALLOCATE_H
#define STUNALLOCATE_H

#include <QObject>
#include <QList>
#include <QHostAddress>

class QByteArray;

namespace XMPP {

class StunMessage;
class StunTransactionPool;

class StunAllocate : public QObject
{
	Q_OBJECT

public:
	enum Error
	{
		ErrorGeneric,
		ErrorTimeout,
		ErrorAuth,
		ErrorRejected,
		ErrorProtocol,
		ErrorCapacity,
		ErrorMismatch
	};

	class Channel
	{
	public:
		QHostAddress address;
		int port;

		Channel(const QHostAddress &_address, int _port) :
			address(_address),
			port(_port)
		{
		}

		inline bool operator==(const Channel &other)
		{
			if(address == other.address && port == other.port)
				return true;
			else
				return false;
		}

		inline bool operator!=(const Channel &other)
		{
			return !operator==(other);
		}
	};

	StunAllocate(StunTransactionPool *pool);
	~StunAllocate();

	void setClientSoftwareNameAndVersion(const QString &str);

	void start();
	void start(const QHostAddress &addr, int port); // use addr association
	void stop();

	QString serverSoftwareNameAndVersion() const;

	QHostAddress reflexiveAddress() const;
	int reflexivePort() const;

	QHostAddress relayedAddress() const;
	int relayedPort() const;

	QList<QHostAddress> permissions() const;
	void setPermissions(const QList<QHostAddress> &perms);

	QList<Channel> channels() const;
	void setChannels(const QList<Channel> &channels);

	int packetHeaderOverhead(const QHostAddress &addr, int port) const;

	QByteArray encode(const QByteArray &datagram, const QHostAddress &addr, int port);
	QByteArray decode(const QByteArray &encoded, QHostAddress *addr = 0, int *port = 0);
	QByteArray decode(const StunMessage &encoded, QHostAddress *addr = 0, int *port = 0);

	QString errorString() const;

	static bool containsChannelData(const quint8 *data, int size);
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

}

#endif
