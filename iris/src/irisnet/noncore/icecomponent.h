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

#ifndef ICECOMPONENT_H
#define ICECOMPONENT_H

#include <QList>
#include "turnclient.h"
#include "icetransport.h"
#include "ice176.h"

class QUdpSocket;

namespace XMPP {

class UdpPortReserver;

class IceComponent : public QObject
{
	Q_OBJECT

public:
	enum CandidateType
	{
		HostType,
		PeerReflexiveType,
		ServerReflexiveType,
		RelayedType
	};

	class TransportAddress
	{
	public:
		QHostAddress addr;
		int port;

		TransportAddress() :
			port(-1)
		{
		}

		TransportAddress(const QHostAddress &_addr, int _port) :
			addr(_addr),
			port(_port)
		{
		}

		bool operator==(const TransportAddress &other) const
		{
			if(addr == other.addr && port == other.port)
				return true;
			else
				return false;
		}

		inline bool operator!=(const TransportAddress &other) const
		{
			return !operator==(other);
		}
	};

	class CandidateInfo
	{
	public:
		TransportAddress addr;
		CandidateType type;
		int priority;
		QString foundation;
		int componentId;
		TransportAddress base;
		TransportAddress related;
		QString id;
		int network;
	};

	class Candidate
	{
	public:
		// unique across all candidates within this component
		int id;

		// info.id is unset, since it must be unique across all
		//   components and this class is only aware of itself.  it
		//   is up to the user to create the candidate id.
		// info.foundation is also unset, since awareness of all
		//   components and candidates is needed to calculate it.
		CandidateInfo info;

		// note that these may be the same for multiple candidates
		IceTransport *iceTransport;
		int path;
	};

	enum DebugLevel
	{
		DL_None,
		DL_Info,
		DL_Packet
	};

	IceComponent(int id, QObject *parent = 0);
	~IceComponent();

	int id() const;

	void setClientSoftwareNameAndVersion(const QString &str);
	void setProxy(const TurnClient::Proxy &proxy);

	void setPortReserver(UdpPortReserver *portReserver);

	// can be set once, but later changes are ignored
	void setLocalAddresses(const QList<Ice176::LocalAddress> &addrs);

	// can be set once, but later changes are ignored.  local addresses
	//   must have been set for this to work
	void setExternalAddresses(const QList<Ice176::ExternalAddress> &addrs);

	// can be set at any time, but only once.  later changes are ignored
	void setStunBindService(const QHostAddress &addr, int port);
	void setStunRelayUdpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass);
	void setStunRelayTcpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass);

	// these all start out enabled, but can be disabled for diagnostic
	//   purposes
	void setUseLocal(bool enabled);
	void setUseStunBind(bool enabled);
	void setUseStunRelayUdp(bool enabled);
	void setUseStunRelayTcp(bool enabled);

	// if socketList is not null then port reserver must be set
	void update(QList<QUdpSocket*> *socketList = 0);
	void stop();

	// prflx priority to use when replying from this transport/path
	int peerReflexivePriority(const IceTransport *iceTransport, int path) const;

	void flagPathAsLowOverhead(int id, const QHostAddress &addr, int port);

	void setDebugLevel(DebugLevel level);

signals:
	// this is emitted in the same pass of the eventloop that a
	//   transport/path becomes ready
	void candidateAdded(const XMPP::IceComponent::Candidate &c);

	// this is emitted just before a transport/path will be deleted
	void candidateRemoved(const XMPP::IceComponent::Candidate &c);

	// indicates all the initial HostType candidates have been pushed.
	//   note that it is possible there are no HostType candidates.
	void localFinished();

	void stopped();

	// reports debug of iceTransports as well.  not DOR-SS/DS safe
	void debugLine(const QString &line);

private:
	class Private;
	friend class Private;
	Private *d;
};

inline uint qHash(const XMPP::IceComponent::TransportAddress &key)
{
	return ::qHash(key.addr) ^ ::qHash(key.port);
}

}

#endif
