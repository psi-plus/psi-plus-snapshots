/*
 * Copyright (C) 2009,2010  Barracuda Networks, Inc.
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

#ifndef ICE176_H
#define ICE176_H

#include <QObject>
#include <QString>
#include <QHostAddress>
#include "turnclient.h"

namespace QCA {
	class SecureArray;
}

namespace XMPP {

class UdpPortReserver;

class Ice176 : public QObject
{
	Q_OBJECT

public:
	enum Error
	{
		ErrorGeneric
	};

	enum Mode
	{
		Initiator,
		Responder
	};

	class LocalAddress
	{
	public:
		QHostAddress addr;
		int network; // -1 = unknown
		bool isVpn;

		LocalAddress() :
			network(-1),
			isVpn(false)
		{
		}
	};

	class ExternalAddress
	{
	public:
		LocalAddress base;
		QHostAddress addr;
		int portBase; // -1 = same as base

		ExternalAddress() :
			portBase(-1)
		{
		}
	};

	class Candidate
	{
	public:
		int component;
		QString foundation;
		int generation;
		QString id;
		QHostAddress ip;
		int network; // -1 = unknown
		int port;
		int priority;
		QString protocol;
		QHostAddress rel_addr;
		int rel_port;
		QHostAddress rem_addr;
		int rem_port;
		QString type;

		Candidate() :
			component(-1),
			generation(-1),
			network(-1),
			port(-1),
			priority(-1),
			rel_port(-1),
			rem_port(-1)
		{
		}
	};

	Ice176(QObject *parent = 0);
	~Ice176();

	void reset();

	void setProxy(const TurnClient::Proxy &proxy);

	// if set, ports will be drawn from the reserver if possible, before
	//   binding to random ports
	// note: ownership is not passed
	void setPortReserver(UdpPortReserver *portReserver);

	void setLocalAddresses(const QList<LocalAddress> &addrs);

	// one per local address.  you must set local addresses first.
	void setExternalAddresses(const QList<ExternalAddress> &addrs);

	void setStunBindService(const QHostAddress &addr, int port);
	void setStunRelayUdpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass);
	void setStunRelayTcpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass);

	// these all start out enabled, but can be disabled for diagnostic
	//   purposes
	void setUseLocal(bool enabled);
	void setUseStunBind(bool enabled);
	void setUseStunRelayUdp(bool enabled);
	void setUseStunRelayTcp(bool enabled);

	void setComponentCount(int count);
	void setLocalCandidateTrickle(bool enabled); // default false

	void start(Mode mode);
	void stop();

	QString localUfrag() const;
	QString localPassword() const;

	void setPeerUfrag(const QString &ufrag);
	void setPeerPassword(const QString &pass);

	void addRemoteCandidates(const QList<Candidate> &list);

	bool hasPendingDatagrams(int componentIndex) const;
	QByteArray readDatagram(int componentIndex);
	void writeDatagram(int componentIndex, const QByteArray &datagram);

	// this call will ensure that TURN headers are minimized on this
	//   component, with the drawback that packets might not be able to
	//   be set as non-fragmentable.  use this on components that expect
	//   to send lots of very small packets, where header overhead is the
	//   most costly but also where fragmentation is impossible anyway.
	//   in short, use this on audio, but not on video.
	void flagComponentAsLowOverhead(int componentIndex);

	// FIXME: this should probably be in netinterface.h or such
	static bool isIPv6LinkLocalAddress(const QHostAddress &addr);

signals:
	// indicates that the ice engine is started and is ready to receive
	//   peer creds and remote candidates
	void started();

	void stopped();
	void error(XMPP::Ice176::Error e);

	void localCandidatesReady(const QList<XMPP::Ice176::Candidate> &list);
	void componentReady(int index);

	void readyRead(int componentIndex);
	void datagramsWritten(int componentIndex, int count);

private:
	class Private;
	friend class Private;
	Private *d;
};

}

#endif
