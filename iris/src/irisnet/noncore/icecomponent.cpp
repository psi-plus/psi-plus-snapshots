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

#include "icecomponent.h"

#include <QUdpSocket>
#include <QtCrypto>
#include "objectsession.h"
#include "udpportreserver.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"

namespace XMPP {

static int calc_priority(int typePref, int localPref, int componentId)
{
	Q_ASSERT(typePref >= 0 && typePref <= 126);
	Q_ASSERT(localPref >= 0 && localPref <= 65535);
	Q_ASSERT(componentId >= 1 && componentId <= 256);

	int priority = (1 << 24) * typePref;
	priority += (1 << 8) * localPref;
	priority += (256 - componentId);
	return priority;
}

class IceComponent::Private : public QObject
{
	Q_OBJECT

public:
	class Config
	{
	public:
		QList<Ice176::LocalAddress> localAddrs;
		QList<Ice176::ExternalAddress> extAddrs;

		QHostAddress stunBindAddr;
		int stunBindPort;

		QHostAddress stunRelayUdpAddr;
		int stunRelayUdpPort;
		QString stunRelayUdpUser;
		QCA::SecureArray stunRelayUdpPass;

		QHostAddress stunRelayTcpAddr;
		int stunRelayTcpPort;
		QString stunRelayTcpUser;
		QCA::SecureArray stunRelayTcpPass;
	};

	class LocalTransport
	{
	public:
		QUdpSocket *qsock;
		bool borrowedSocket;
		QHostAddress addr;
		IceLocalTransport *sock;
		int network;
		bool isVpn;
		bool started;
		bool stun_started;
		bool stun_finished, turn_finished;
		QHostAddress extAddr;
		bool ext_finished;

		LocalTransport() :
			qsock(0),
			borrowedSocket(false),
			sock(0),
			network(-1),
			isVpn(false),
			started(false),
			stun_started(false),
			stun_finished(false),
			turn_finished(false),
			ext_finished(false)
		{
		}
	};

	IceComponent *q;
	ObjectSession sess;
	int id;
	QString clientSoftware;
	TurnClient::Proxy proxy;
	UdpPortReserver *portReserver;
	Config pending;
	Config config;
	bool stopping;
	QList<LocalTransport*> localLeap;
	QList<LocalTransport*> localStun;
	IceTurnTransport *tt;
	QList<Candidate> localCandidates;
	QHash<int, QSet<TransportAddress> > channelPeers;
	bool useLocal;
	bool useStunBind;
	bool useStunRelayUdp;
	bool useStunRelayTcp;
	bool local_finished;
	int debugLevel;

	Private(IceComponent *_q) :
		QObject(_q),
		q(_q),
		sess(this),
		portReserver(0),
		stopping(false),
		tt(0),
		useLocal(true),
		useStunBind(true),
		useStunRelayUdp(true),
		useStunRelayTcp(true),
		local_finished(false),
		debugLevel(DL_None)
	{
	}

	~Private()
	{
		QList<QUdpSocket*> socketsToReturn;

		for(int n = 0; n < localLeap.count(); ++n)
		{
			delete localLeap[n]->sock;

			if(localLeap[n]->borrowedSocket)
				socketsToReturn += localLeap[n]->qsock;
			else
				localLeap[n]->qsock->deleteLater();
		}

		if(!socketsToReturn.isEmpty())
			portReserver->returnSockets(socketsToReturn);

		qDeleteAll(localLeap);

		for(int n = 0; n < localStun.count(); ++n)
			delete localStun[n]->sock;

		qDeleteAll(localStun);

		delete tt;
	}

	void update(QList<QUdpSocket*> *socketList)
	{
		Q_ASSERT(!stopping);

		// for now, only allow setting localAddrs once
		if(!pending.localAddrs.isEmpty() && config.localAddrs.isEmpty())
		{
			foreach(const Ice176::LocalAddress &la, pending.localAddrs)
			{
				// skip duplicate addrs
				if(findLocalAddr(la.addr) != -1)
					continue;

				if(!useLocal)
				{
					// skip out, but log the address in
					//   case we need it for stun
					config.localAddrs += la;
					continue;
				}

				QUdpSocket *qsock = 0;
				if(socketList)
					qsock = takeFromSocketList(socketList, la.addr, this);

				bool borrowedSocket;
				if(qsock)
				{
					borrowedSocket = true;
				}
				else
				{
					// otherwise, bind to random
					qsock = new QUdpSocket(this);
					if(!qsock->bind(la.addr, 0))
					{
						delete qsock;
						emit q->debugLine("Warning: unable to bind to random port.");
						continue;
					}

					borrowedSocket = false;
				}

				int port = qsock->localPort();

				config.localAddrs += la;

				LocalTransport *lt = new LocalTransport;
				lt->addr = la.addr;
				lt->qsock = qsock;
				lt->borrowedSocket = borrowedSocket;
				lt->sock = new IceLocalTransport(this);
				lt->sock->setDebugLevel((IceTransport::DebugLevel)debugLevel);
				lt->network = la.network;
				lt->isVpn = la.isVpn;
				connect(lt->sock, SIGNAL(started()), SLOT(lt_started()));
				connect(lt->sock, SIGNAL(stopped()), SLOT(lt_stopped()));
				connect(lt->sock, SIGNAL(addressesChanged()), SLOT(lt_addressesChanged()));
				connect(lt->sock, SIGNAL(error(int)), SLOT(lt_error(int)));
				connect(lt->sock, SIGNAL(debugLine(const QString &)), SLOT(lt_debugLine(const QString &)));
				localLeap += lt;

				lt->sock->start(qsock);
				emit q->debugLine(QString("starting transport ") + la.addr.toString() + ';' + QString::number(port) + " for component " + QString::number(id));
			}
		}

		// extAddrs created on demand if present, but only once
		if(!pending.extAddrs.isEmpty() && config.extAddrs.isEmpty())
		{
			config.extAddrs = pending.extAddrs;

			bool need_doExt = false;

			foreach(LocalTransport *lt, localLeap)
			{
				// already assigned an ext address?  skip
				if(!lt->extAddr.isNull())
					continue;

				QHostAddress laddr = lt->sock->localAddress();
				int lport = lt->sock->localPort();

				int at = -1;
				for(int n = 0; n < config.extAddrs.count(); ++n)
				{
					const Ice176::ExternalAddress &ea = config.extAddrs[n];
					if(laddr.protocol() != QAbstractSocket::IPv6Protocol && ea.base.addr == laddr && (ea.portBase == -1 || ea.portBase == lport))
					{
						at = n;
						break;
					}
				}

				if(at != -1)
				{
					lt->extAddr = config.extAddrs[at].addr;
					if(lt->started)
						need_doExt = true;
				}
			}

			if(need_doExt)
				sess.defer(this, "doExt");
		}

		// only allow setting stun stuff once
		if(!pending.stunBindAddr.isNull() && config.stunBindAddr.isNull())
		{
			config.stunBindAddr = pending.stunBindAddr;
			config.stunBindPort = pending.stunBindPort;
			config.stunRelayUdpAddr = pending.stunRelayUdpAddr;
			config.stunRelayUdpPort = pending.stunRelayUdpPort;
			config.stunRelayUdpUser = pending.stunRelayUdpUser;
			config.stunRelayUdpPass = pending.stunRelayUdpPass;
			config.stunRelayTcpAddr = pending.stunRelayTcpAddr;
			config.stunRelayTcpPort = pending.stunRelayTcpPort;
			config.stunRelayTcpUser = pending.stunRelayTcpUser;
			config.stunRelayTcpPass = pending.stunRelayTcpPass;
		}

		// localStun sockets created on demand if stun settings are
		//   present, but only once (cannot be changed, for now)
		if(((useStunBind && !config.stunBindAddr.isNull()) || (useStunRelayUdp && !config.stunRelayUdpAddr.isNull() && !config.stunRelayUdpUser.isEmpty())) && !config.localAddrs.isEmpty() && localStun.isEmpty())
		{
			foreach(const Ice176::LocalAddress &la, config.localAddrs)
			{
				// don't setup stun ports for ipv6
				if(la.addr.protocol() == QAbstractSocket::IPv6Protocol)
					continue;

				LocalTransport *lt = new LocalTransport;
				lt->addr = la.addr;
				lt->sock = new IceLocalTransport(this);
				lt->sock->setDebugLevel((IceTransport::DebugLevel)debugLevel);
				lt->network = la.network;
				lt->isVpn = la.isVpn;
				connect(lt->sock, SIGNAL(started()), SLOT(lt_started()));
				connect(lt->sock, SIGNAL(stopped()), SLOT(lt_stopped()));
				connect(lt->sock, SIGNAL(addressesChanged()), SLOT(lt_addressesChanged()));
				connect(lt->sock, SIGNAL(error(int)), SLOT(lt_error(int)));
				connect(lt->sock, SIGNAL(debugLine(const QString &)), SLOT(lt_debugLine(const QString &)));
				localStun += lt;

				lt->sock->setClientSoftwareNameAndVersion(clientSoftware);
				lt->sock->start(la.addr);
				emit q->debugLine(QString("starting transport ") + la.addr.toString() + ";(dyn)" + " for component " + QString::number(id));
			}
		}

		if((!config.stunBindAddr.isNull() || !config.stunRelayUdpAddr.isNull()) && !localStun.isEmpty())
		{
			for(int n = 0; n < localStun.count(); ++n)
			{
				if(localStun[n]->started && !localStun[n]->stun_started)
					tryStun(n);
			}
		}

		if(useStunRelayTcp && !config.stunRelayTcpAddr.isNull() && !config.stunRelayTcpUser.isEmpty() && !tt)
		{
			tt = new IceTurnTransport(this);
			tt->setDebugLevel((IceTransport::DebugLevel)debugLevel);
			connect(tt, SIGNAL(started()), SLOT(tt_started()));
			connect(tt, SIGNAL(stopped()), SLOT(tt_stopped()));
			connect(tt, SIGNAL(error(int)), SLOT(tt_error(int)));
			connect(tt, SIGNAL(debugLine(const QString &)), SLOT(tt_debugLine(const QString &)));
			tt->setClientSoftwareNameAndVersion(clientSoftware);
			tt->setProxy(proxy);
			tt->setUsername(config.stunRelayTcpUser);
			tt->setPassword(config.stunRelayTcpPass);
			tt->start(config.stunRelayTcpAddr, config.stunRelayTcpPort);

			emit q->debugLine(QString("starting TURN transport with server ") + config.stunRelayTcpAddr.toString() + ';' + QString::number(config.stunRelayTcpPort) + " for component " + QString::number(id));
		}

		if(localLeap.isEmpty() && localStun.isEmpty() && !local_finished)
		{
			local_finished = true;
			sess.defer(q, "localFinished");
		}
	}

	void stop()
	{
		Q_ASSERT(!stopping);

		stopping = true;

		// nothing to stop?
		if(allStopped())
		{
			sess.defer(this, "postStop");
			return;
		}

		foreach(LocalTransport *lt, localLeap)
			lt->sock->stop();

		foreach(LocalTransport *lt, localStun)
			lt->sock->stop();

		if(tt)
			tt->stop();
	}

	int peerReflexivePriority(const IceTransport *iceTransport, int path) const
	{
		int addrAt = -1;
		const IceLocalTransport *lt = qobject_cast<const IceLocalTransport*>(iceTransport);
		if(lt)
		{
			bool isLocalLeap = false;
			addrAt = findLocalTransport(lt, &isLocalLeap);
			if(addrAt != -1 && path == 1)
			{
				// lower priority, but not as far as IceTurnTransport
				addrAt += 512;
			}
		}
		else if(qobject_cast<const IceTurnTransport*>(iceTransport) == tt)
		{
			// lower priority by making it seem like the last nic
			addrAt = 1024;
		}

		Q_ASSERT(addrAt != -1);

		return choose_default_priority(PeerReflexiveType, 65535 - addrAt, false, id);
	}

	void flagPathAsLowOverhead(int id, const QHostAddress &addr, int port)
	{
		int at = -1;
		for(int n = 0; n < localCandidates.count(); ++n)
		{
			if(localCandidates[n].id == id)
			{
				at = n;
				break;
			}
		}

		Q_ASSERT(at != -1);

		Candidate &c = localCandidates[at];

		TransportAddress ta(addr, port);
		QSet<TransportAddress> &addrs = channelPeers[c.id];
		if(!addrs.contains(ta))
		{
			addrs += ta;
			c.iceTransport->addChannelPeer(ta.addr, ta.port);
		}
	}

private:
	// localPref is the priority of the network interface being used for
	//   this candidate.  the value must be between 0-65535 and different
	//   interfaces must have different values.  if there is only one
	//   interface, the value should be 65535.
	static int choose_default_priority(CandidateType type, int localPref, bool isVpn, int componentId)
	{
		int typePref;
		if(type == HostType)
		{
			if(isVpn)
				typePref = 0;
			else
				typePref = 126;
		}
		else if(type == PeerReflexiveType)
			typePref = 110;
		else if(type == ServerReflexiveType)
			typePref = 100;
		else // RelayedType
			typePref = 0;

		return calc_priority(typePref, localPref, componentId);
	}

	static QUdpSocket *takeFromSocketList(QList<QUdpSocket*> *socketList, const QHostAddress &addr, QObject *parent = 0)
	{
		for(int n = 0; n < socketList->count(); ++n)
		{
			if((*socketList)[n]->localAddress() == addr)
			{
				QUdpSocket *sock = socketList->takeAt(n);
				sock->setParent(parent);
				return sock;
			}
		}

		return 0;
	}

	int getId() const
	{
		for(int n = 0;; ++n)
		{
			bool found = false;
			foreach(const Candidate &c, localCandidates)
			{
				if(c.id == n)
				{
					found = true;
					break;
				}
			}

			if(!found)
				return n;
		}
	}

	int findLocalAddr(const QHostAddress &addr)
	{
		for(int n = 0; n < config.localAddrs.count(); ++n)
		{
			if(config.localAddrs[n].addr == addr)
				return n;
		}

		return -1;
	}

	int findLocalTransport(const IceLocalTransport *sock, bool *isLocalLeap) const
	{
		for(int n = 0; n < localLeap.count(); ++n)
		{
			if(localLeap[n]->sock == sock)
			{
				*isLocalLeap = true;
				return n;
			}
		}

		for(int n = 0; n < localStun.count(); ++n)
		{
			if(localStun[n]->sock == sock)
			{
				*isLocalLeap = false;
				return n;
			}
		}

		return -1;
	}

	void tryStun(int at)
	{
		LocalTransport *lt = localStun[at];

		bool atLeastOne = false;
		if(useStunBind && !config.stunBindAddr.isNull())
		{
			atLeastOne = true;
			lt->sock->setStunBindService(config.stunBindAddr, config.stunBindPort);
		}
		if(useStunRelayUdp && !config.stunRelayUdpAddr.isNull() && !config.stunRelayUdpUser.isEmpty())
		{
			atLeastOne = true;
			lt->sock->setStunRelayService(config.stunRelayUdpAddr, config.stunRelayUdpPort, config.stunRelayUdpUser, config.stunRelayUdpPass);
		}

		Q_ASSERT(atLeastOne);

		lt->stun_started = true;
		lt->sock->stunStart();
	}

	void ensureExt(LocalTransport *lt, int addrAt)
	{
		if(!lt->extAddr.isNull() && !lt->ext_finished)
		{
			CandidateInfo ci;
			ci.addr.addr = lt->extAddr;
			ci.addr.port = lt->sock->localPort();
			ci.type = ServerReflexiveType;
			ci.componentId = id;
			ci.priority = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
			ci.base.addr = lt->sock->localAddress();
			ci.base.port = lt->sock->localPort();
			ci.network = lt->network;

			Candidate c;
			c.id = getId();
			c.info = ci;
			c.iceTransport = lt->sock;
			c.path = 0;

			localCandidates += c;
			lt->ext_finished = true;

			emit q->candidateAdded(c);
		}
	}

	void removeLocalCandidates(const IceTransport *sock)
	{
		ObjectSessionWatcher watch(&sess);

		for(int n = 0; n < localCandidates.count(); ++n)
		{
			Candidate &c = localCandidates[n];

			if(c.iceTransport == sock)
			{
				Candidate tmp = localCandidates.takeAt(n);
				--n; // adjust position

				channelPeers.remove(tmp.id);

				emit q->candidateRemoved(tmp);
				if(!watch.isValid())
					return;
			}
		}
	}

	bool allStopped() const
	{
		if(localLeap.isEmpty() && localStun.isEmpty() && !tt)
			return true;
		else
			return false;
	}

	void tryStopped()
	{
		if(allStopped())
			postStop();
	}

private slots:
	void doExt()
	{
		if(stopping)
			return;

		ObjectSessionWatcher watch(&sess);

		foreach(LocalTransport *lt, localLeap)
		{
			if(lt->started)
			{
				int addrAt = findLocalAddr(lt->addr);
				Q_ASSERT(addrAt != -1);

				ensureExt(lt, addrAt);
				if(!watch.isValid())
					return;
			}
		}
	}

	void postStop()
	{
		stopping = false;

		emit q->stopped();
	}

	void lt_started()
	{
		IceLocalTransport *sock = (IceLocalTransport *)sender();
		bool isLocalLeap = false;
		int at = findLocalTransport(sock, &isLocalLeap);
		Q_ASSERT(at != -1);

		LocalTransport *lt;
		if(isLocalLeap)
			lt = localLeap[at];
		else
			lt = localStun[at];

		lt->started = true;

		int addrAt = findLocalAddr(lt->addr);
		Q_ASSERT(addrAt != -1);

		ObjectSessionWatcher watch(&sess);

		if(useLocal && isLocalLeap)
		{
			CandidateInfo ci;
			ci.addr.addr = lt->sock->localAddress();
			ci.addr.port = lt->sock->localPort();
			ci.type = HostType;
			ci.componentId = id;
			ci.priority = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
			ci.base = ci.addr;
			ci.network = lt->network;

			Candidate c;
			c.id = getId();
			c.info = ci;
			c.iceTransport = sock;
			c.path = 0;

			localCandidates += c;

			emit q->candidateAdded(c);
			if(!watch.isValid())
				return;

			ensureExt(lt, addrAt);
			if(!watch.isValid())
				return;
		}

		if(!isLocalLeap && !lt->stun_started)
			tryStun(at);

		bool allFinished = true;
		foreach(const LocalTransport *lt, localLeap)
		{
			if(!lt->started)
			{
				allFinished = false;
				break;
			}
		}
		if(allFinished)
		{
			foreach(const LocalTransport *lt, localStun)
			{
				if(!lt->started)
				{
					allFinished = false;
					break;
				}
			}
		}

		if(allFinished && !local_finished)
		{
			local_finished = true;
			emit q->localFinished();
		}
	}

	void lt_stopped()
	{
		IceLocalTransport *sock = (IceLocalTransport *)sender();
		bool isLocalLeap = false;
		int at = findLocalTransport(sock, &isLocalLeap);
		Q_ASSERT(at != -1);

		LocalTransport *lt;
		if(isLocalLeap)
			lt = localLeap[at];
		else
			lt = localStun[at];

		ObjectSessionWatcher watch(&sess);

		removeLocalCandidates(lt->sock);
		if(!watch.isValid())
			return;

		delete lt->sock;
		lt->sock = 0;

		if(isLocalLeap)
		{
			if(lt->borrowedSocket)
				portReserver->returnSockets(QList<QUdpSocket*>() << lt->qsock);
			else
				lt->qsock->deleteLater();

			delete lt;
			localLeap.removeAt(at);
		}
		else
		{
			delete lt;
			localStun.removeAt(at);
		}

		tryStopped();
	}

	void lt_addressesChanged()
	{
		IceLocalTransport *sock = (IceLocalTransport *)sender();
		bool isLocalLeap = false;
		int at = findLocalTransport(sock, &isLocalLeap);
		Q_ASSERT(at != -1);

		// leap does not use stun, so we should not get this signal
		Q_ASSERT(!isLocalLeap);

		LocalTransport *lt = localStun[at];

		int addrAt = findLocalAddr(lt->addr);
		Q_ASSERT(addrAt != -1);

		ObjectSessionWatcher watch(&sess);

		if(useStunBind && !lt->sock->serverReflexiveAddress().isNull() && !lt->stun_finished)
		{
			// automatically assign ext to related leaps, if possible
			foreach(LocalTransport *i, localLeap)
			{
				if(i->extAddr.isNull() && i->sock->localAddress() == lt->sock->localAddress())
				{
					i->extAddr = lt->sock->serverReflexiveAddress();
					if(i->started)
					{
						ensureExt(i, addrAt);
						if(!watch.isValid())
							return;
					}
				}
			}

			CandidateInfo ci;
			ci.addr.addr = lt->sock->serverReflexiveAddress();
			ci.addr.port = lt->sock->serverReflexivePort();
			ci.type = ServerReflexiveType;
			ci.componentId = id;
			ci.priority = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
			// stun is only used on non-leap sockets, but we don't
			//   announce non-leap local candidates, so make the
			//   base the same as the srflx
			//ci.base.addr = lt->sock->localAddress();
			//ci.base.port = lt->sock->localPort();
			ci.base = ci.addr;
			ci.network = lt->network;

			Candidate c;
			c.id = getId();
			c.info = ci;
			c.iceTransport = sock;
			c.path = 0;

			localCandidates += c;
			lt->stun_finished = true;

			emit q->candidateAdded(c);
			if(!watch.isValid())
				return;
		}

		if(!lt->sock->relayedAddress().isNull() && !lt->turn_finished)
		{
			CandidateInfo ci;
			ci.addr.addr = lt->sock->relayedAddress();
			ci.addr.port = lt->sock->relayedPort();
			ci.type = RelayedType;
			ci.componentId = id;
			ci.priority = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
			ci.base.addr = lt->sock->serverReflexiveAddress();
			ci.base.port = lt->sock->serverReflexivePort();
			ci.network = lt->network;

			Candidate c;
			c.id = getId();
			c.info = ci;
			c.iceTransport = sock;
			c.path = 1;

			localCandidates += c;
			lt->turn_finished = true;

			emit q->candidateAdded(c);
		}
	}

	void lt_error(int e)
	{
		Q_UNUSED(e);

		IceLocalTransport *sock = (IceLocalTransport *)sender();
		bool isLocalLeap = false;
		int at = findLocalTransport(sock, &isLocalLeap);
		Q_ASSERT(at != -1);

		LocalTransport *lt;
		if(isLocalLeap)
			lt = localLeap[at];
		else
			lt = localStun[at];

		ObjectSessionWatcher watch(&sess);

		removeLocalCandidates(lt->sock);
		if(!watch.isValid())
			return;

		delete lt->sock;
		lt->sock = 0;

		if(isLocalLeap)
		{
			if(lt->borrowedSocket)
				portReserver->returnSockets(QList<QUdpSocket*>() << lt->qsock);
			else
				lt->qsock->deleteLater();

			delete lt;
			localLeap.removeAt(at);
		}
		else
		{
			delete lt;
			localStun.removeAt(at);
		}
	}

	void lt_debugLine(const QString &line)
	{
		emit q->debugLine(line);
	}

	void tt_started()
	{
		// lower priority by making it seem like the last nic
		int addrAt = 1024;

		CandidateInfo ci;
		ci.addr.addr = tt->relayedAddress();
		ci.addr.port = tt->relayedPort();
		ci.type = RelayedType;
		ci.componentId = id;
		ci.priority = choose_default_priority(ci.type, 65535 - addrAt, false, ci.componentId);
		ci.base = ci.addr;
		ci.network = 0; // not relevant

		Candidate c;
		c.id = getId();
		c.info = ci;
		c.iceTransport = tt;
		c.path = 0;

		localCandidates += c;

		emit q->candidateAdded(c);
	}

	void tt_stopped()
	{
		ObjectSessionWatcher watch(&sess);

		removeLocalCandidates(tt);
		if(!watch.isValid())
			return;

		delete tt;
		tt = 0;

		tryStopped();
	}

	void tt_error(int e)
	{
		Q_UNUSED(e);

		ObjectSessionWatcher watch(&sess);

		removeLocalCandidates(tt);
		if(!watch.isValid())
			return;

		delete tt;
		tt = 0;
	}

	void tt_debugLine(const QString &line)
	{
		emit q->debugLine(line);
	}
};

IceComponent::IceComponent(int id, QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
	d->id = id;
}

IceComponent::~IceComponent()
{
	delete d;
}

int IceComponent::id() const
{
	return d->id;
}

void IceComponent::setClientSoftwareNameAndVersion(const QString &str)
{
	d->clientSoftware = str;
}

void IceComponent::setProxy(const TurnClient::Proxy &proxy)
{
	d->proxy = proxy;
}

void IceComponent::setPortReserver(UdpPortReserver *portReserver)
{
	d->portReserver = portReserver;
}

void IceComponent::setLocalAddresses(const QList<Ice176::LocalAddress> &addrs)
{
	d->pending.localAddrs = addrs;
}

void IceComponent::setExternalAddresses(const QList<Ice176::ExternalAddress> &addrs)
{
	d->pending.extAddrs = addrs;
}

void IceComponent::setStunBindService(const QHostAddress &addr, int port)
{
	d->pending.stunBindAddr = addr;
	d->pending.stunBindPort = port;
}

void IceComponent::setStunRelayUdpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass)
{
	d->pending.stunRelayUdpAddr = addr;
	d->pending.stunRelayUdpPort = port;
	d->pending.stunRelayUdpUser = user;
	d->pending.stunRelayUdpPass = pass;
}

void IceComponent::setStunRelayTcpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass)
{
	d->pending.stunRelayTcpAddr = addr;
	d->pending.stunRelayTcpPort = port;
	d->pending.stunRelayTcpUser = user;
	d->pending.stunRelayTcpPass = pass;
}

void IceComponent::setUseLocal(bool enabled)
{
	d->useLocal = enabled;
}

void IceComponent::setUseStunBind(bool enabled)
{
	d->useStunBind = enabled;
}

void IceComponent::setUseStunRelayUdp(bool enabled)
{
	d->useStunRelayUdp = enabled;
}

void IceComponent::setUseStunRelayTcp(bool enabled)
{
	d->useStunRelayTcp = enabled;
}

void IceComponent::update(QList<QUdpSocket*> *socketList)
{
	d->update(socketList);
}

void IceComponent::stop()
{
	d->stop();
}

int IceComponent::peerReflexivePriority(const IceTransport *iceTransport, int path) const
{
	return d->peerReflexivePriority(iceTransport, path);
}

void IceComponent::flagPathAsLowOverhead(int id, const QHostAddress &addr, int port)
{
	return d->flagPathAsLowOverhead(id, addr, port);
}

void IceComponent::setDebugLevel(DebugLevel level)
{
	d->debugLevel = level;
	foreach(const Private::LocalTransport *lt, d->localLeap)
		lt->sock->setDebugLevel((IceTransport::DebugLevel)level);
	foreach(const Private::LocalTransport *lt, d->localStun)
		lt->sock->setDebugLevel((IceTransport::DebugLevel)level);
	if(d->tt)
		d->tt->setDebugLevel((IceTransport::DebugLevel)level);
}

}

#include "icecomponent.moc"
