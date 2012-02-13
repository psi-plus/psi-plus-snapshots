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

#ifndef TURNCLIENT_H
#define TURNCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QHostAddress>

namespace QCA {
	class SecureArray;
}

namespace XMPP {

class StunTransactionPool;
class StunAllocate;

class TurnClient : public QObject
{
	Q_OBJECT

public:
	enum Error
	{
		ErrorGeneric,
		ErrorHostNotFound,
		ErrorConnect,

		// stream error or stream unexpectedly disconnected by peer
		ErrorStream,

		ErrorProxyConnect,
		ErrorProxyNeg,
		ErrorProxyAuth,
		ErrorTls,
		ErrorAuth,
		ErrorRejected,
		ErrorProtocol,
		ErrorCapacity,

		// according to the TURN spec, a client should try three times
		//   to correct a mismatch error before giving up.  this class
		//   will perform the retries internally, and ErrorMismatch is
		//   only emitted when it has given up.  note that if this
		//   happens, the TURN spec says you should not connect to the
		//   TURN server again for at least 2 minutes.
		// note: in UDP mode, this class does not perform retries and
		//   will emit this error immediately.
		ErrorMismatch
	};

	enum Mode
	{
		PlainMode,
		TlsMode
	};

	enum DebugLevel
	{
		DL_None,
		DL_Info,
		DL_Packet
	};

	// adapted from XMPP::AdvancedConnector
	class Proxy
	{
	public:
		enum
		{
			None,
			HttpConnect,
			Socks
		};

		Proxy();
		~Proxy();

		int type() const;
		QString host() const;
		quint16 port() const;
		QString user() const;
		QString pass() const;

		void setHttpConnect(const QString &host, quint16 port);
		void setSocks(const QString &host, quint16 port);
		void setUserPass(const QString &user, const QString &pass);

	private:
		int t;
		QString v_host;
		quint16 v_port;
		QString v_user, v_pass;
	};

	TurnClient(QObject *parent = 0);
	~TurnClient();

	void setProxy(const Proxy &proxy);
	void setClientSoftwareNameAndVersion(const QString &str);

	// for UDP.  does not take ownership of the pool.  stun transaction
	//   I/O occurs through the pool.  transfer of data packets occurs
	//   via processIncomingDatagram(), outgoingDatagram(), and
	//   outgoingDatagramsWritten().  authentication happens through the
	//   pool and not through this class.  the turn addr/port is optional,
	//   and used only for addr association with the pool
	void connectToHost(StunTransactionPool *pool, const QHostAddress &addr = QHostAddress(), int port = -1);

	// for TCP and TCP-TLS
	void connectToHost(const QHostAddress &addr, int port, Mode mode = PlainMode);

	// for UDP, use this function to process incoming packets instead of
	//   read().
	QByteArray processIncomingDatagram(const QByteArray &buf, bool notStun, QHostAddress *addr, int *port);

	// call after writing datagrams from outgoingDatagram.  not DOR-DS safe
	void outgoingDatagramsWritten(int count);

	QString realm() const;
	void setUsername(const QString &username);
	void setPassword(const QCA::SecureArray &password);
	void setRealm(const QString &realm);
	void continueAfterParams();

	void close();

	StunAllocate *stunAllocate();

	void addChannelPeer(const QHostAddress &addr, int port);

	int packetsToRead() const;
	int packetsToWrite() const;

	// TCP mode only
	QByteArray read(QHostAddress *addr, int *port);

	// for UDP, this call may emit outgoingDatagram() immediately (not
	//   DOR-DS safe)
	void write(const QByteArray &buf, const QHostAddress &addr, int port);

	QString errorString() const;

	void setDebugLevel(DebugLevel level); // default DL_None

signals:
	void connected(); // tcp connected
	void tlsHandshaken();
	void closed();
	void needAuthParams();
	void retrying(); // mismatch error received, starting all over
	void activated(); // ready for read/write

	// TCP mode only
	void readyRead();

	void packetsWritten(int count, const QHostAddress &addr, int port);
	void error(XMPP::TurnClient::Error e);

	// data packets to be sent to the TURN server, UDP mode only
	void outgoingDatagram(const QByteArray &buf);

	// not DOR-SS/DS safe
	void debugLine(const QString &line);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
