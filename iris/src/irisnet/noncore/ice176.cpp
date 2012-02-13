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

#include "ice176.h"

#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QtCrypto>
#include "stuntransaction.h"
#include "stunbinding.h"
#include "stunmessage.h"
#include "udpportreserver.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"
#include "icecomponent.h"

namespace XMPP {

enum
{
	Direct,
	Relayed
};

static QChar randomPrintableChar()
{
	// 0-25 = a-z
	// 26-51 = A-Z
	// 52-61 = 0-9

	uchar c = QCA::Random::randomChar() % 62;
	if(c <= 25)
		return 'a' + c;
	else if(c <= 51)
		return 'A' + (c - 26);
	else
		return '0' + (c - 52);
}

static QString randomCredential(int len)
{
	QString out;
	for(int n = 0; n < len; ++n)
		out += randomPrintableChar();
	return out;
}

static qint64 calc_pair_priority(int a, int b)
{
	qint64 priority = ((qint64)1 << 32) * qMin(a, b);
	priority += (qint64)2 * qMax(a, b);
	if(a > b)
		++priority;
	return priority;
}

// see if candidates are considered the same for pruning purposes
static bool compare_candidates(const IceComponent::CandidateInfo &a, const IceComponent::CandidateInfo &b)
{
	if(a.addr == b.addr && a.componentId == b.componentId)
		return true;
	else
		return false;
}

// scope values: 0 = local, 1 = link-local, 2 = private, 3 = public
// FIXME: dry (this is in psi avcall also)
static int getAddressScope(const QHostAddress &a)
{
	if(a.protocol() == QAbstractSocket::IPv6Protocol)
	{
		if(a == QHostAddress(QHostAddress::LocalHostIPv6))
			return 0;
		else if(XMPP::Ice176::isIPv6LinkLocalAddress(a))
			return 1;
	}
	else if(a.protocol() == QAbstractSocket::IPv4Protocol)
	{
		quint32 v4 = a.toIPv4Address();
		quint8 a0 = v4 >> 24;
		quint8 a1 = (v4 >> 16) & 0xff;
		if(a0 == 127)
			return 0;
		else if(a0 == 169 && a1 == 254)
			return 1;
		else if(a0 == 10)
			return 2;
		else if(a0 == 172 && a1 >= 16 && a1 <= 31)
			return 2;
		else if(a0 == 192 && a1 == 168)
			return 2;
	}

	return 3;
}

class Ice176::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		Stopped,
		Starting,
		Started,
		Stopping
	};

	enum CandidatePairState
	{
		PWaiting,
		PInProgress,
		PSucceeded,
		PFailed,
		PFrozen
	};

	enum CheckListState
	{
		LRunning,
		LCompleted,
		LFailed
	};

	class CandidatePair
	{
	public:
		IceComponent::CandidateInfo local, remote;
		bool isDefault;
		bool isValid;
		bool isNominated;
		CandidatePairState state;

		qint64 priority;
		QString foundation;

		StunBinding *binding;

		// FIXME: this is wrong i think, it should be in LocalTransport
		//   or such, to multiplex ids
		StunTransactionPool *pool;

		CandidatePair() :
			binding(0),
			pool(0)
		{
		}
	};

	class CheckList
	{
	public:
		QList<CandidatePair> pairs;
		CheckListState state;
	};

	class Component
	{
	public:
		int id;
		IceComponent *ic;
		bool localFinished;
		bool stopped;
		bool lowOverhead;

		Component() :
			localFinished(false),
			stopped(false),
			lowOverhead(false)
		{
		}
	};

	Ice176 *q;
	Ice176::Mode mode;
	State state;
	TurnClient::Proxy proxy;
	UdpPortReserver *portReserver;
	int componentCount;
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
	QString localUser, localPass;
	QString peerUser, peerPass;
	QList<Component> components;
	QList<IceComponent::Candidate> localCandidates;
	QSet<IceTransport*> iceTransports;
	CheckList checkList;
	QList< QList<QByteArray> > in;
	bool useLocal;
	bool useStunBind;
	bool useStunRelayUdp;
	bool useStunRelayTcp;
	bool useTrickle;
	QTimer *collectTimer;

	Private(Ice176 *_q) :
		QObject(_q),
		q(_q),
		state(Stopped),
		portReserver(0),
		componentCount(0),
		useLocal(true),
		useStunBind(true),
		useStunRelayUdp(true),
		useStunRelayTcp(true),
		useTrickle(false),
		collectTimer(0)
	{
	}

	~Private()
	{
		if(collectTimer)
		{
			collectTimer->disconnect(this);
			collectTimer->deleteLater();
		}

		foreach(const Component &c, components)
			delete c.ic;

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			StunBinding *binding = checkList.pairs[n].binding;
			StunTransactionPool *pool = checkList.pairs[n].pool;

			delete binding;

			if(pool)
			{
				pool->disconnect(this);
				pool->setParent(0);
				pool->deleteLater();
			}
		}
	}

	void reset()
	{
		// TODO
	}

	int findLocalAddress(const QHostAddress &addr)
	{
		for(int n = 0; n < localAddrs.count(); ++n)
		{
			if(localAddrs[n].addr == addr)
				return n;
		}

		return -1;
	}

	void updateLocalAddresses(const QList<LocalAddress> &addrs)
	{
		// for now, ignore address changes during operation
		if(state != Stopped)
			return;

		localAddrs.clear();
		foreach(const LocalAddress &la, addrs)
		{
			int at = findLocalAddress(la.addr);
			if(at == -1)
				localAddrs += la;
		}
	}

	void updateExternalAddresses(const QList<ExternalAddress> &addrs)
	{
		// for now, ignore address changes during operation
		if(state != Stopped)
			return;

		extAddrs.clear();
		foreach(const ExternalAddress &ea, addrs)
		{
			int at = findLocalAddress(ea.base.addr);
			if(at != -1)
				extAddrs += ea;
		}
	}

	void start()
	{
		Q_ASSERT(state == Stopped);

		state = Starting;

		localUser = randomCredential(4);
		localPass = randomCredential(22);

		QList<QUdpSocket*> socketList;
		if(portReserver)
			socketList = portReserver->borrowSockets(componentCount, this);

		for(int n = 0; n < componentCount; ++n)
		{
			Component c;
			c.id = n + 1;
			c.ic = new IceComponent(c.id, this);
			c.ic->setDebugLevel(IceComponent::DL_Info);
			connect(c.ic, SIGNAL(candidateAdded(const XMPP::IceComponent::Candidate &)), SLOT(ic_candidateAdded(const XMPP::IceComponent::Candidate &)));
			connect(c.ic, SIGNAL(candidateRemoved(const XMPP::IceComponent::Candidate &)), SLOT(ic_candidateRemoved(const XMPP::IceComponent::Candidate &)));
			connect(c.ic, SIGNAL(localFinished()), SLOT(ic_localFinished()));
			connect(c.ic, SIGNAL(stopped()), SLOT(ic_stopped()));
			connect(c.ic, SIGNAL(debugLine(const QString &)), SLOT(ic_debugLine(const QString &)));

			c.ic->setClientSoftwareNameAndVersion("Iris");
			c.ic->setProxy(proxy);
			if(portReserver)
				c.ic->setPortReserver(portReserver);
			c.ic->setLocalAddresses(localAddrs);
			c.ic->setExternalAddresses(extAddrs);
			if(!stunBindAddr.isNull())
				c.ic->setStunBindService(stunBindAddr, stunBindPort);
			if(!stunRelayUdpAddr.isNull())
				c.ic->setStunRelayUdpService(stunRelayUdpAddr, stunRelayUdpPort, stunRelayUdpUser, stunRelayUdpPass);
			if(!stunRelayTcpAddr.isNull())
				c.ic->setStunRelayTcpService(stunRelayTcpAddr, stunRelayTcpPort, stunRelayTcpUser, stunRelayTcpPass);

			c.ic->setUseLocal(useLocal);
			c.ic->setUseStunBind(useStunBind);
			c.ic->setUseStunRelayUdp(useStunRelayUdp);
			c.ic->setUseStunRelayTcp(useStunRelayTcp);

			// create an inbound queue for this component
			in += QList<QByteArray>();

			components += c;

			c.ic->update(&socketList);
		}

		// socketList should always empty here, but might not be if
		//   the app provided a different address list to
		//   UdpPortReserver and Ice176.  and that would really be
		//   a dumb thing to do but I'm not going to Q_ASSERT it
		if(!socketList.isEmpty())
			portReserver->returnSockets(socketList);
	}

	void stop()
	{
		Q_ASSERT(state == Starting || state == Started);

		state = Stopping;

		if(!components.isEmpty())
		{
			for(int n = 0; n < components.count(); ++n)
				components[n].ic->stop();
		}
		else
		{
			// TODO: hmm, is it possible to have no components?
			QMetaObject::invokeMethod(this, "postStop", Qt::QueuedConnection);
		}
	}

	void addRemoteCandidates(const QList<Candidate> &list)
	{
		QList<IceComponent::CandidateInfo> remoteCandidates;
		foreach(const Candidate &c, list)
		{
			IceComponent::CandidateInfo ci;
			ci.addr.addr = c.ip;
			ci.addr.addr.setScopeId(QString());
			ci.addr.port = c.port;
			ci.type = (IceComponent::CandidateType)string_to_candidateType(c.type); // TODO: handle error
			ci.componentId = c.component;
			ci.priority = c.priority;
			ci.foundation = c.foundation;
			if(!c.rel_addr.isNull())
			{
				ci.base.addr = c.rel_addr;
				ci.base.addr.setScopeId(QString());
				ci.base.port = c.rel_port;
			}
			ci.network = c.network;
			ci.id = c.id;
			remoteCandidates += ci;
		}

		printf("adding %d remote candidates\n", remoteCandidates.count());

		QList<CandidatePair> pairs;
		foreach(const IceComponent::Candidate &cc, localCandidates)
		{
			const IceComponent::CandidateInfo &lc = cc.info;

			foreach(const IceComponent::CandidateInfo &rc, remoteCandidates)
			{
				if(lc.componentId != rc.componentId)
					continue;

				// don't pair ipv4 with ipv6.  FIXME: is this right?
				if(lc.addr.addr.protocol() != rc.addr.addr.protocol())
					continue;

				// don't relay to localhost.  turnserver
				//   doesn't like it.  i don't know if this
				//   should qualify as a HACK or not.
				//   trying to relay to localhost is pretty
				//   stupid anyway
				if(lc.type == IceComponent::RelayedType && getAddressScope(rc.addr.addr) == 0)
					continue;

				CandidatePair pair;
				pair.state = PFrozen; // FIXME: setting state here may be wrong
				pair.local = lc;
				pair.remote = rc;
				if(pair.local.addr.addr.protocol() == QAbstractSocket::IPv6Protocol && isIPv6LinkLocalAddress(pair.local.addr.addr))
					pair.remote.addr.addr.setScopeId(pair.local.addr.addr.scopeId());
				pair.isDefault = false;
				pair.isValid = false;
				pair.isNominated = false;
				if(mode == Ice176::Initiator)
					pair.priority = calc_pair_priority(lc.priority, rc.priority);
				else
					pair.priority = calc_pair_priority(rc.priority, lc.priority);
				pairs += pair;
			}
		}

		printf("%d pairs\n", pairs.count());

		// combine pairs with existing, and sort
		pairs = checkList.pairs + pairs;
		checkList.pairs.clear();
		foreach(const CandidatePair &pair, pairs)
		{
			int at;
			for(at = 0; at < checkList.pairs.count(); ++at)
			{
				if(compare_pair(pair, checkList.pairs[at]) < 0)
					break;
			}

			checkList.pairs.insert(at, pair);
		}

		// pruning

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			CandidatePair &pair = checkList.pairs[n];
			if(pair.local.type == IceComponent::ServerReflexiveType)
				pair.local.addr = pair.local.base;
		}

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			CandidatePair &pair = checkList.pairs[n];
			printf("%d, %s:%d -> %s:%d\n", pair.local.componentId, qPrintable(pair.local.addr.addr.toString()), pair.local.addr.port, qPrintable(pair.remote.addr.addr.toString()), pair.remote.addr.port);

			bool found = false;
			for(int i = n - 1; i >= 0; --i)
			{
				if(compare_candidates(pair.local, checkList.pairs[i].local) && compare_candidates(pair.remote, checkList.pairs[i].remote))
				{
					found = true;
					break;
				}
			}

			if(found )
			{
				checkList.pairs.removeAt(n);
				--n; // adjust position
			}
		}

		// max pairs is 100 * number of components
		int max_pairs = 100 * components.count();
		while(checkList.pairs.count() > max_pairs)
			checkList.pairs.removeLast();

		printf("%d after pruning\n", checkList.pairs.count());

		// set state
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			CandidatePair &pair = checkList.pairs[n];

			// only initialize the new pairs
			if(pair.state != PFrozen)
				continue;

			pair.foundation = pair.local.foundation + pair.remote.foundation;

			// FIXME: for now we just do checks to everything immediately
			pair.state = PInProgress;

			int at = findLocalCandidate(pair.local.addr.addr, pair.local.addr.port);
			Q_ASSERT(at != -1);

			IceComponent::Candidate &lc = localCandidates[at];

			Component &c = components[findComponent(lc.info.componentId)];

			pair.pool = new StunTransactionPool(StunTransaction::Udp, this);
			connect(pair.pool, SIGNAL(outgoingMessage(const QByteArray &, const QHostAddress &, int)), SLOT(pool_outgoingMessage(const QByteArray &, const QHostAddress &, int)));
			//pair.pool->setUsername(peerUser + ':' + localUser);
			//pair.pool->setPassword(peerPass.toUtf8());

			pair.binding = new StunBinding(pair.pool);
			connect(pair.binding, SIGNAL(success()), SLOT(binding_success()));

			int prflx_priority = c.ic->peerReflexivePriority(lc.iceTransport, lc.path);
			pair.binding->setPriority(prflx_priority);

			if(mode == Ice176::Initiator)
			{
				pair.binding->setIceControlling(0);
				pair.binding->setUseCandidate(true);
			}
			else
				pair.binding->setIceControlled(0);

			pair.binding->setShortTermUsername(peerUser + ':' + localUser);
			pair.binding->setShortTermPassword(peerPass);

			pair.binding->start();
		}
	}

	void write(int componentIndex, const QByteArray &datagram)
	{
		int at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].local.componentId - 1 == componentIndex && checkList.pairs[n].isValid)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		CandidatePair &pair = checkList.pairs[at];

		at = findLocalCandidate(pair.local.addr.addr, pair.local.addr.port);
		if(at == -1) // FIXME: assert?
			return;

		IceComponent::Candidate &lc = localCandidates[at];

		IceTransport *sock = lc.iceTransport;
		int path = lc.path;

		sock->writeDatagram(path, datagram, pair.remote.addr.addr, pair.remote.addr.port);

		// DOR-SR?
		QMetaObject::invokeMethod(q, "datagramsWritten", Qt::QueuedConnection, Q_ARG(int, componentIndex), Q_ARG(int, 1));
	}

	void flagComponentAsLowOverhead(int componentIndex)
	{
		// FIXME: ok to assume in order?
		Component &c = components[componentIndex];
		c.lowOverhead = true;

		// FIXME: actually do something
	}

private:
	int findComponent(const IceComponent *ic) const
	{
		for(int n = 0; n < components.count(); ++n)
		{
			if(components[n].ic == ic)
				return n;
		}

		return -1;
	}

	int findComponent(int id) const
	{
		for(int n = 0; n < components.count(); ++n)
		{
			if(components[n].id == id)
				return n;
		}

		return -1;
	}

	int findLocalCandidate(const IceTransport *iceTransport, int path) const
	{
		for(int n = 0; n < localCandidates.count(); ++n)
		{
			const IceComponent::Candidate &cc = localCandidates[n];
			if(cc.iceTransport == iceTransport && cc.path == path)
				return n;
		}

		return -1;
	}

	int findLocalCandidate(const QHostAddress &fromAddr, int fromPort)
	{
		for(int n = 0; n < localCandidates.count(); ++n)
		{
			const IceComponent::Candidate &cc = localCandidates[n];
			if(cc.info.addr.addr == fromAddr && cc.info.addr.port == fromPort)
				return n;
		}

		return -1;
	}

	static QString candidateType_to_string(IceComponent::CandidateType type)
	{
		QString out;
		switch(type)
		{
			case IceComponent::HostType: out = "host"; break;
			case IceComponent::PeerReflexiveType: out = "prflx"; break;
			case IceComponent::ServerReflexiveType: out = "srflx"; break;
			case IceComponent::RelayedType: out = "relay"; break;
			default: Q_ASSERT(0);
		}
		return out;
	}

	static int string_to_candidateType(const QString &in)
	{
		if(in == "host")
			return IceComponent::HostType;
		else if(in == "prflx")
			return IceComponent::PeerReflexiveType;
		else if(in == "srflx")
			return IceComponent::ServerReflexiveType;
		else if(in == "relay")
			return IceComponent::RelayedType;
		else
			return -1;
	}

	static int compare_pair(const CandidatePair &a, const CandidatePair &b)
	{
		// prefer remote srflx, for leap
		if(a.remote.type == IceComponent::ServerReflexiveType && b.remote.type != IceComponent::ServerReflexiveType && b.remote.addr.addr.protocol() != QAbstractSocket::IPv6Protocol)
			return -1;
		else if(b.remote.type == IceComponent::ServerReflexiveType && a.remote.type != IceComponent::ServerReflexiveType && a.remote.addr.addr.protocol() != QAbstractSocket::IPv6Protocol)
			return 1;

		if(a.priority > b.priority)
			return -1;
		else if(b.priority > a.priority)
			return 1;

		return 0;
	}

private slots:
	void postStop()
	{
		state = Stopped;
		emit q->stopped();
	}

	void ic_candidateAdded(const XMPP::IceComponent::Candidate &_cc)
	{
		IceComponent::Candidate cc = _cc;
		cc.info.id = randomCredential(10); // FIXME: ensure unique
		cc.info.foundation = "0"; // FIXME
		// TODO
		localCandidates += cc;

		printf("C%d: candidate added: %s;%d\n", cc.info.componentId, qPrintable(cc.info.addr.addr.toString()), cc.info.addr.port);

		if(!iceTransports.contains(cc.iceTransport))
		{
			connect(cc.iceTransport, SIGNAL(readyRead(int)), SLOT(it_readyRead(int)));
			connect(cc.iceTransport, SIGNAL(datagramsWritten(int, int, const QHostAddress &, int)), SLOT(it_datagramsWritten(int, int, const QHostAddress &, int)));

			iceTransports += cc.iceTransport;
		}

		if(state == Started && useTrickle)
		{
			QList<Ice176::Candidate> list;

			Ice176::Candidate c;
			c.component = cc.info.componentId;
			c.foundation = cc.info.foundation;
			c.generation = 0; // TODO
			c.id = cc.info.id;
			c.ip = cc.info.addr.addr;
			c.ip.setScopeId(QString());
			c.network = cc.info.network;
			c.port = cc.info.addr.port;
			c.priority = cc.info.priority;
			c.protocol = "udp";
			if(cc.info.type != IceComponent::HostType)
			{
				c.rel_addr = cc.info.base.addr;
				c.rel_addr.setScopeId(QString());
				c.rel_port = cc.info.base.port;
			}
			else
			{
				c.rel_addr = QHostAddress();
				c.rel_port = -1;
			}
			c.rem_addr = QHostAddress();
			c.rem_port = -1;
			c.type = candidateType_to_string(cc.info.type);
			list += c;

			emit q->localCandidatesReady(list);
		}
	}

	void ic_candidateRemoved(const XMPP::IceComponent::Candidate &cc)
	{
		// TODO
		printf("C%d: candidate removed: %s;%d\n", cc.info.componentId, qPrintable(cc.info.addr.addr.toString()), cc.info.addr.port);

		QStringList idList;
		for(int n = 0; n < localCandidates.count(); ++n)
		{
			if(localCandidates[n].id == cc.id && localCandidates[n].info.componentId == cc.info.componentId)
			{
				// FIXME: this is rather ridiculous I think
				idList += localCandidates[n].info.id;

				localCandidates.removeAt(n);
				--n; // adjust position
			}
		}

		bool iceTransportInUse = false;
		foreach(const IceComponent::Candidate &lc, localCandidates)
		{
			if(lc.iceTransport == cc.iceTransport)
			{
				iceTransportInUse = true;
				break;
			}
		}
		if(!iceTransportInUse)
		{
			cc.iceTransport->disconnect(this);
			iceTransports.remove(cc.iceTransport);
		}

		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(idList.contains(checkList.pairs[n].local.id))
			{
				StunBinding *binding = checkList.pairs[n].binding;
				StunTransactionPool *pool = checkList.pairs[n].pool;

				delete binding;

				if(pool)
				{
					pool->disconnect(this);
					pool->setParent(0);
					pool->deleteLater();
				}

				checkList.pairs.removeAt(n);
				--n; // adjust position
			}
		}
	}

	void ic_localFinished()
	{
		IceComponent *ic = (IceComponent *)sender();
		int at = findComponent(ic);
		Q_ASSERT(at != -1);

		components[at].localFinished = true;

		bool allFinished = true;
		foreach(const Component &c, components)
		{
			if(!c.localFinished)
			{
				allFinished = false;
				break;
			}
		}

		if(allFinished)
		{
			state = Started;

			emit q->started();

			if(!useTrickle)
			{
				// FIXME: there should be a way to not wait if
				//   we know for sure there is nothing else
				//   possibly coming
				collectTimer = new QTimer(this);
				connect(collectTimer, SIGNAL(timeout()), SLOT(collect_timeout()));
				collectTimer->setSingleShot(true);
				collectTimer->start(4000);
				return;
			}

			// FIXME: DOR-SS
			QList<Ice176::Candidate> list;
			foreach(const IceComponent::Candidate &cc, localCandidates)
			{
				Ice176::Candidate c;
				c.component = cc.info.componentId;
				c.foundation = cc.info.foundation;
				c.generation = 0; // TODO
				c.id = cc.info.id;
				c.ip = cc.info.addr.addr;
				c.ip.setScopeId(QString());
				c.network = cc.info.network;
				c.port = cc.info.addr.port;
				c.priority = cc.info.priority;
				c.protocol = "udp";
				if(cc.info.type != IceComponent::HostType)
				{
					c.rel_addr = cc.info.base.addr;
					c.rel_addr.setScopeId(QString());
					c.rel_port = cc.info.base.port;
				}
				else
				{
					c.rel_addr = QHostAddress();
					c.rel_port = -1;
				}
				c.rem_addr = QHostAddress();
				c.rem_port = -1;
				c.type = candidateType_to_string(cc.info.type);
				list += c;
			}
			if(!list.isEmpty())
				emit q->localCandidatesReady(list);
		}
	}

	void ic_stopped()
	{
		IceComponent *ic = (IceComponent *)sender();
		int at = findComponent(ic);
		Q_ASSERT(at != -1);

		components[at].stopped = true;

		bool allStopped = true;
		foreach(const Component &c, components)
		{
			if(!c.stopped)
			{
				allStopped = false;
				break;
			}
		}

		if(allStopped)
			postStop();
	}

	void ic_debugLine(const QString &line)
	{
		IceComponent *ic = (IceComponent *)sender();
		int at = findComponent(ic);
		Q_ASSERT(at != -1);

		// FIXME: components are always sorted?
		printf("C%d: %s\n", at + 1, qPrintable(line));
	}

	void collect_timeout()
	{
		collectTimer->disconnect(this);
		collectTimer->deleteLater();
		collectTimer = 0;

		QList<Ice176::Candidate> list;
		foreach(const IceComponent::Candidate &cc, localCandidates)
		{
			Ice176::Candidate c;
			c.component = cc.info.componentId;
			c.foundation = cc.info.foundation;
			c.generation = 0; // TODO
			c.id = cc.info.id;
			c.ip = cc.info.addr.addr;
			c.ip.setScopeId(QString());
			c.network = cc.info.network;
			c.port = cc.info.addr.port;
			c.priority = cc.info.priority;
			c.protocol = "udp";
			if(cc.info.type != IceComponent::HostType)
			{
				c.rel_addr = cc.info.base.addr;
				c.rel_addr.setScopeId(QString());
				c.rel_port = cc.info.base.port;
			}
			else
			{
				c.rel_addr = QHostAddress();
				c.rel_port = -1;
			}
			c.rem_addr = QHostAddress();
			c.rem_port = -1;
			c.type = candidateType_to_string(cc.info.type);
			list += c;
		}
		if(!list.isEmpty())
			emit q->localCandidatesReady(list);
	}

	void it_readyRead(int path)
	{
		IceTransport *it = (IceTransport *)sender();
		int at = findLocalCandidate(it, path);
		Q_ASSERT(at != -1);

		IceComponent::Candidate &cc = localCandidates[at];

		IceTransport *sock = it;

		while(sock->hasPendingDatagrams(path))
		{
			QHostAddress fromAddr;
			int fromPort;
			QByteArray buf = sock->readDatagram(path, &fromAddr, &fromPort);

			//printf("port %d: received packet (%d bytes)\n", lt->sock->localPort(), buf.size());

			QString requser = localUser + ':' + peerUser;
			QByteArray reqkey = localPass.toUtf8();

			StunMessage::ConvertResult result;
			StunMessage msg = StunMessage::fromBinary(buf, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
			if(!msg.isNull() && (msg.mclass() == StunMessage::Request || msg.mclass() == StunMessage::Indication))
			{
				printf("received validated request or indication from %s:%d\n", qPrintable(fromAddr.toString()), fromPort);
				QString user = QString::fromUtf8(msg.attribute(0x0006)); // USERNAME
				if(requser != user)
				{
					printf("user [%s] is wrong.  it should be [%s].  skipping\n", qPrintable(user), qPrintable(requser));
					continue;
				}

				if(msg.method() != 0x001)
				{
					printf("not a binding request.  skipping\n");
					continue;
				}

				StunMessage response;
				response.setClass(StunMessage::SuccessResponse);
				response.setMethod(0x001);
				response.setId(msg.id());

				quint16 port16 = fromPort;
				quint32 addr4 = fromAddr.toIPv4Address();
				QByteArray val(8, 0);
				quint8 *p = (quint8 *)val.data();
				const quint8 *magic = response.magic();
				p[0] = 0;
				p[1] = 0x01;
				p[2] = (port16 >> 8) & 0xff;
				p[2] ^= magic[0];
				p[3] = port16 & 0xff;
				p[3] ^= magic[1];
				p[4] = (addr4 >> 24) & 0xff;
				p[4] ^= magic[0];
				p[5] = (addr4 >> 16) & 0xff;
				p[5] ^= magic[1];
				p[6] = (addr4 >> 8) & 0xff;
				p[6] ^= magic[2];
				p[7] = addr4 & 0xff;
				p[7] ^= magic[3];

				QList<StunMessage::Attribute> list;
				StunMessage::Attribute attr;
				attr.type = 0x0020;
				attr.value = val;
				list += attr;

				response.setAttributes(list);

				QByteArray packet = response.toBinary(StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
				sock->writeDatagram(path, packet, fromAddr, fromPort);
			}
			else
			{
				QByteArray reskey = peerPass.toUtf8();
				StunMessage msg = StunMessage::fromBinary(buf, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, reskey);
				if(!msg.isNull() && (msg.mclass() == StunMessage::SuccessResponse || msg.mclass() == StunMessage::ErrorResponse))
				{
					printf("received validated response\n");

					// FIXME: this is so gross and completely defeats the point of having pools
					for(int n = 0; n < checkList.pairs.count(); ++n)
					{
						CandidatePair &pair = checkList.pairs[n];
						if(pair.local.addr.addr == cc.info.addr.addr && pair.local.addr.port == cc.info.addr.port)
							pair.pool->writeIncomingMessage(msg);
					}
				}
				else
				{
					//printf("received some non-stun or invalid stun packet\n");

					// FIXME: i don't know if this is good enough
					if(StunMessage::isProbablyStun(buf))
					{
						printf("unexpected stun packet (loopback?), skipping.\n");
						continue;
					}

					int at = -1;
					for(int n = 0; n < checkList.pairs.count(); ++n)
					{
						CandidatePair &pair = checkList.pairs[n];
						if(pair.local.addr.addr == cc.info.addr.addr && pair.local.addr.port == cc.info.addr.port)
						{
							at = n;
							break;
						}
					}
					if(at == -1)
					{
						printf("the local transport does not seem to be associated with a candidate?!\n");
						continue;
					}

					int componentIndex = checkList.pairs[at].local.componentId - 1;
					//printf("packet is considered to be application data for component index %d\n", componentIndex);

					// FIXME: this assumes components are ordered by id in our local arrays
					in[componentIndex] += buf;
					emit q->readyRead(componentIndex);
				}
			}
		}
	}

	void it_datagramsWritten(int path, int count, const QHostAddress &addr, int port)
	{
		// TODO
		Q_UNUSED(path);
		Q_UNUSED(count);
		Q_UNUSED(addr);
		Q_UNUSED(port);
	}

	void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &addr, int port)
	{
		Q_UNUSED(addr);
		Q_UNUSED(port);

		StunTransactionPool *pool = (StunTransactionPool *)sender();
		int at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].pool == pool)
			{
				at = n;
				break;
			}
		}
		if(at == -1) // FIXME: assert?
			return;

		CandidatePair &pair = checkList.pairs[at];

		at = findLocalCandidate(pair.local.addr.addr, pair.local.addr.port);
		if(at == -1) // FIXME: assert?
			return;

		IceComponent::Candidate &lc = localCandidates[at];

		IceTransport *sock = lc.iceTransport;
		int path = lc.path;

		printf("connectivity check from %s:%d to %s:%d\n", qPrintable(pair.local.addr.addr.toString()), pair.local.addr.port, qPrintable(pair.remote.addr.addr.toString()), pair.remote.addr.port);
		sock->writeDatagram(path, packet, pair.remote.addr.addr, pair.remote.addr.port);
	}

	void binding_success()
	{
		StunBinding *binding = (StunBinding *)sender();
		int at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].binding == binding)
			{
				at = n;
				break;
			}
		}
		if(at == -1)
			return;

		printf("check success\n");

		CandidatePair &pair = checkList.pairs[at];

		// TODO: if we were cool, we'd do something with the peer
		//   reflexive address received

		// TODO: we're also supposed to do triggered checks.  except
		//   that currently we check everything anyway so this is not
		//   relevant

		// check if there's a candidate already valid
		at = -1;
		for(int n = 0; n < checkList.pairs.count(); ++n)
		{
			if(checkList.pairs[n].local.componentId == pair.local.componentId && checkList.pairs[n].isValid)
			{
				at = n;
				break;
			}
		}

		pair.isValid = true;

		if(at == -1)
		{
			int at = findComponent(pair.local.componentId);
			Component &c = components[at];
			if(c.lowOverhead)
			{
				printf("component is flagged for low overhead.  setting up for %s;%d -> %s;%d\n",
					qPrintable(pair.local.addr.addr.toString()), pair.local.addr.port, qPrintable(pair.remote.addr.addr.toString()), pair.remote.addr.port);
				at = findLocalCandidate(pair.local.addr.addr, pair.local.addr.port);
				IceComponent::Candidate &cc = localCandidates[at];
				c.ic->flagPathAsLowOverhead(cc.id, pair.remote.addr.addr, pair.remote.addr.port);
			}

			emit q->componentReady(pair.local.componentId - 1);
		}
		else
		{
			printf("component %d already active, not signalling\n", pair.local.componentId);
		}
	}
};

Ice176::Ice176(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

Ice176::~Ice176()
{
	delete d;
}

void Ice176::reset()
{
	d->reset();
}

void Ice176::setProxy(const TurnClient::Proxy &proxy)
{
	d->proxy = proxy;
}

void Ice176::setPortReserver(UdpPortReserver *portReserver)
{
	Q_ASSERT(d->state == Private::Stopped);

	d->portReserver = portReserver;
}

void Ice176::setLocalAddresses(const QList<LocalAddress> &addrs)
{
	d->updateLocalAddresses(addrs);
}

void Ice176::setExternalAddresses(const QList<ExternalAddress> &addrs)
{
	d->updateExternalAddresses(addrs);
}

void Ice176::setStunBindService(const QHostAddress &addr, int port)
{
	d->stunBindAddr = addr;
	d->stunBindPort = port;
}

void Ice176::setStunRelayUdpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass)
{
	d->stunRelayUdpAddr = addr;
	d->stunRelayUdpPort = port;
	d->stunRelayUdpUser = user;
	d->stunRelayUdpPass = pass;
}

void Ice176::setStunRelayTcpService(const QHostAddress &addr, int port, const QString &user, const QCA::SecureArray &pass)
{
	d->stunRelayTcpAddr = addr;
	d->stunRelayTcpPort = port;
	d->stunRelayTcpUser = user;
	d->stunRelayTcpPass = pass;
}

void Ice176::setUseLocal(bool enabled)
{
	d->useLocal = enabled;
}

void Ice176::setUseStunBind(bool enabled)
{
	d->useStunBind = enabled;
}

void Ice176::setUseStunRelayUdp(bool enabled)
{
	d->useStunRelayUdp = enabled;
}

void Ice176::setUseStunRelayTcp(bool enabled)
{
	d->useStunRelayTcp = enabled;
}

void Ice176::setComponentCount(int count)
{
	Q_ASSERT(d->state == Private::Stopped);

	d->componentCount = count;
}

void Ice176::setLocalCandidateTrickle(bool enabled)
{
	d->useTrickle = enabled;
}

void Ice176::start(Mode mode)
{
	d->mode = mode;
	d->start();
}

void Ice176::stop()
{
	d->stop();
}

QString Ice176::localUfrag() const
{
	return d->localUser;
}

QString Ice176::localPassword() const
{
	return d->localPass;
}

void Ice176::setPeerUfrag(const QString &ufrag)
{
	d->peerUser = ufrag;
}

void Ice176::setPeerPassword(const QString &pass)
{
	d->peerPass = pass;
}

void Ice176::addRemoteCandidates(const QList<Candidate> &list)
{
	d->addRemoteCandidates(list);
}

bool Ice176::hasPendingDatagrams(int componentIndex) const
{
	return !d->in[componentIndex].isEmpty();
}

QByteArray Ice176::readDatagram(int componentIndex)
{
	return d->in[componentIndex].takeFirst();
}

void Ice176::writeDatagram(int componentIndex, const QByteArray &datagram)
{
	d->write(componentIndex, datagram);
}

void Ice176::flagComponentAsLowOverhead(int componentIndex)
{
	d->flagComponentAsLowOverhead(componentIndex);
}

bool Ice176::isIPv6LinkLocalAddress(const QHostAddress &addr)
{
	Q_ASSERT(addr.protocol() == QAbstractSocket::IPv6Protocol);
	Q_IPV6ADDR addr6 = addr.toIPv6Address();
	quint16 hi = addr6[0];
	hi <<= 8;
	hi += addr6[1];
	if((hi & 0xffc0) == 0xfe80)
		return true;
	else
		return false;
}

}

#include "ice176.moc"
