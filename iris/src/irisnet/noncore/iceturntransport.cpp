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

#include "iceturntransport.h"

#include <QtCrypto>
#include "stunallocate.h"

namespace XMPP {

class IceTurnTransport::Private : public QObject
{
	Q_OBJECT

public:
	IceTurnTransport *q;
	int mode;
	QHostAddress serverAddr;
	int serverPort;
	QString relayUser;
	QCA::SecureArray relayPass;
	QHostAddress relayAddr;
	int relayPort;
	TurnClient turn;
	int turnErrorCode;
	int debugLevel;

	Private(IceTurnTransport *_q) :
		QObject(_q),
		q(_q),
		turn(this),
		debugLevel(IceTransport::DL_None)
	{
		connect(&turn, SIGNAL(connected()), SLOT(turn_connected()));
		connect(&turn, SIGNAL(tlsHandshaken()), SLOT(turn_tlsHandshaken()));
		connect(&turn, SIGNAL(closed()), SLOT(turn_closed()));
		connect(&turn, SIGNAL(needAuthParams()), SLOT(turn_needAuthParams()));
		connect(&turn, SIGNAL(retrying()), SLOT(turn_retrying()));
		connect(&turn, SIGNAL(activated()), SLOT(turn_activated()));
		connect(&turn, SIGNAL(readyRead()), SLOT(turn_readyRead()));
		connect(&turn, SIGNAL(packetsWritten(int, const QHostAddress &, int)), SLOT(turn_packetsWritten(int, const QHostAddress &, int)));
		connect(&turn, SIGNAL(error(XMPP::TurnClient::Error)), SLOT(turn_error(XMPP::TurnClient::Error)));
		connect(&turn, SIGNAL(debugLine(const QString &)), SLOT(turn_debugLine(const QString &)));
	}

	void start()
	{
		turn.setUsername(relayUser);
		turn.setPassword(relayPass);
		turn.connectToHost(serverAddr, serverPort, (TurnClient::Mode)mode);
	}

	void stop()
	{
		turn.close();
	}

private slots:
	void turn_connected()
	{
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine("turn_connected");
	}

	void turn_tlsHandshaken()
	{
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine("turn_tlsHandshaken");
	}

	void turn_closed()
	{
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine("turn_closed");

		emit q->stopped();
	}

	void turn_needAuthParams()
	{
		// we can get this signal if the user did not provide
		//   creds to us.  however, since this class doesn't support
		//   prompting just continue on as if we had a blank
		//   user/pass
		turn.continueAfterParams();
	}

	void turn_retrying()
	{
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine("turn_retrying");
	}

	void turn_activated()
	{
		StunAllocate *allocate = turn.stunAllocate();

		QHostAddress saddr = allocate->reflexiveAddress();
		quint16 sport = allocate->reflexivePort();
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine(QString("Server says we are ") + saddr.toString() + ';' + QString::number(sport));
		saddr = allocate->relayedAddress();
		sport = allocate->relayedPort();
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine(QString("Server relays via ") + saddr.toString() + ';' + QString::number(sport));

		relayAddr = saddr;
		relayPort = sport;

		emit q->started();
	}

	void turn_readyRead()
	{
		emit q->readyRead(0);
	}

	void turn_packetsWritten(int count, const QHostAddress &addr, int port)
	{
		emit q->datagramsWritten(0, count, addr, port);
	}

	void turn_error(XMPP::TurnClient::Error e)
	{
		if(debugLevel >= IceTransport::DL_Info)
			emit q->debugLine(QString("turn_error: ") + turn.errorString());

		turnErrorCode = e;
		emit q->error(IceTurnTransport::ErrorTurn);
	}

	void turn_debugLine(const QString &line)
	{
		emit q->debugLine(line);
	}
};

IceTurnTransport::IceTurnTransport(QObject *parent) :
	IceTransport(parent)
{
	d = new Private(this);
}

IceTurnTransport::~IceTurnTransport()
{
	delete d;
}

void IceTurnTransport::setClientSoftwareNameAndVersion(const QString &str)
{
	d->turn.setClientSoftwareNameAndVersion(str);
}

void IceTurnTransport::setUsername(const QString &user)
{
	d->relayUser = user;
}

void IceTurnTransport::setPassword(const QCA::SecureArray &pass)
{
	d->relayPass = pass;
}

void IceTurnTransport::setProxy(const TurnClient::Proxy &proxy)
{
	d->turn.setProxy(proxy);
}

void IceTurnTransport::start(const QHostAddress &addr, int port, TurnClient::Mode mode)
{
	d->serverAddr = addr;
	d->serverPort = port;
	d->mode = mode;
	d->start();
}

QHostAddress IceTurnTransport::relayedAddress() const
{
	return d->relayAddr;
}

int IceTurnTransport::relayedPort() const
{
	return d->relayPort;
}

void IceTurnTransport::addChannelPeer(const QHostAddress &addr, int port)
{
	d->turn.addChannelPeer(addr, port);
}

TurnClient::Error IceTurnTransport::turnErrorCode() const
{
	return (TurnClient::Error)d->turnErrorCode;
}

void IceTurnTransport::stop()
{
	d->stop();
}

bool IceTurnTransport::hasPendingDatagrams(int path) const
{
	Q_ASSERT(path == 0);
	Q_UNUSED(path);

	return (d->turn.packetsToRead() > 0 ? true : false);
}

QByteArray IceTurnTransport::readDatagram(int path, QHostAddress *addr, int *port)
{
	Q_ASSERT(path == 0);
	Q_UNUSED(path);

	return d->turn.read(addr, port);
}

void IceTurnTransport::writeDatagram(int path, const QByteArray &buf, const QHostAddress &addr, int port)
{
	Q_ASSERT(path == 0);
	Q_UNUSED(path);

	d->turn.write(buf, addr, port);
}

void IceTurnTransport::setDebugLevel(DebugLevel level)
{
	d->debugLevel = level;
	d->turn.setDebugLevel((TurnClient::DebugLevel)level);
}

}

#include "iceturntransport.moc"
