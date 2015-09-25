/*
 * s5b.cpp - direct connection protocol via tcp
 * Copyright (C) 2003  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "s5b.h"

#include <QTimer>
#include <QPointer>
#include <QByteArray>
#include <stdlib.h>
#include <qca.h>
#include "xmpp_xmlcommon.h"
#include "im.h"
#include "socks.h"
#include "safedelete.h"

#ifdef Q_OS_WIN
# include <windows.h>
#else
# include <netinet/in.h>
#endif

#define MAXSTREAMHOSTS 5

static const char *S5B_NS = "http://jabber.org/protocol/bytestreams";

namespace XMPP {

static QString makeKey(const QString &sid, const Jid &requester, const Jid &target)
{
#ifdef S5B_DEBUG
	qDebug("makeKey: sid=%s requester=%s target=%s %s", qPrintable(sid),
		   qPrintable(requester.full()), qPrintable(target.full()),
		   qPrintable(QCA::Hash("sha1").hashToString(QString(sid + requester.full() + target.full()).toUtf8())));
#endif
	QString str = sid + requester.full() + target.full();
	return QCA::Hash("sha1").hashToString(str.toUtf8());
}

static bool haveHost(const StreamHostList &list, const Jid &j)
{
	for(StreamHostList::ConstIterator it = list.begin(); it != list.end(); ++it) {
		if((*it).jid().compare(j))
			return true;
	}
	return false;
}

class S5BManager::Item : public QObject
{
	Q_OBJECT
public:
	enum { Idle, Requester, Target, Active };
	enum { ErrRefused, ErrConnect, ErrWrongHost, ErrProxy };
	enum { Unknown, Fast, NotFast };
	S5BManager *m;
	int state;
	QString sid, key, out_key, out_id, in_id;
	Jid self, peer;
	StreamHostList in_hosts;
	JT_S5B *task, *proxy_task;
	SocksClient *client, *client_out;
	SocksUDP *client_udp, *client_out_udp;
	S5BConnector *conn, *proxy_conn;
	bool wantFast;
	StreamHost proxy;
	int targetMode; // requester sets this once it figures it out
	bool fast; // target sets this
	bool activated;
	bool lateProxy;
	bool connSuccess;
	bool localFailed, remoteFailed;
	bool allowIncoming;
	bool udp;
	int statusCode;
	Jid activatedStream;

	Item(S5BManager *manager);
	~Item();

	void resetConnection();
	void startRequester(const QString &_sid, const Jid &_self, const Jid &_peer, bool fast, bool udp);
	void startTarget(const QString &_sid, const Jid &_self, const Jid &_peer,
					 const QString &_dstaddr, const StreamHostList &hosts,
					 const QString &iq_id, bool fast, bool udp);
	void handleFast(const StreamHostList &hosts, const QString &iq_id);

	void doOutgoing();
	void doIncoming();
	void setIncomingClient(SocksClient *sc);
	void incomingActivate(const Jid &streamHost);

signals:
	void accepted();
	void tryingHosts(const StreamHostList &list);
	void proxyConnect();
	void waitingForActivation();
	void connected();
	void error(int);

private slots:
	void jt_finished();
	void conn_result(bool b);
	void proxy_result(bool b);
	void proxy_finished();
	void sc_readyRead();
	void sc_bytesWritten(qint64);
	void sc_error(int);

private:
	void doConnectError();
	void tryActivation();
	void checkForActivation();
	void checkFailure();
	void finished();
};

//----------------------------------------------------------------------------
// S5BDatagram
//----------------------------------------------------------------------------
S5BDatagram::S5BDatagram()
{
	_source = 0;
	_dest = 0;
}

S5BDatagram::S5BDatagram(int source, int dest, const QByteArray &data)
{
	_source = source;
	_dest = dest;
	_buf = data;
}

int S5BDatagram::sourcePort() const
{
	return _source;
}

int S5BDatagram::destPort() const
{
	return _dest;
}

QByteArray S5BDatagram::data() const
{
	return _buf;
}

//----------------------------------------------------------------------------
// S5BConnection
//----------------------------------------------------------------------------
class S5BConnection::Private
{
public:
	S5BManager *m;
	SocksClient *sc;
	SocksUDP *su;
	int state;
	Jid peer;
	QString sid;
	bool remote;
	bool switched;
	bool notifyRead, notifyClose;
	int id;
	S5BRequest req;
	Jid proxy;
	Mode mode;
	QList<S5BDatagram*> dglist;
};

static int id_conn = 0;
static int num_conn = 0;

S5BConnection::S5BConnection(S5BManager *m, QObject *parent)
	: BSConnection(parent)
{
	d = new Private;
	d->m = m;
	d->sc = 0;
	d->su = 0;

	++num_conn;
	d->id = id_conn++;
#ifdef S5B_DEBUG
	qDebug("S5BConnection[%d]: constructing, count=%d, %p\n", d->id, num_conn, this);
#endif

	resetConnection();
}

S5BConnection::~S5BConnection()
{
	resetConnection(true);

	--num_conn;
#ifdef S5B_DEBUG
	qDebug("S5BConnection[%d]: destructing, count=%d\n", d->id, num_conn);
#endif

	delete d;
}

void S5BConnection::resetConnection(bool clear)
{
	d->m->con_unlink(this);
	if(clear && d->sc) {
		delete d->sc;
		d->sc = 0;
	}
	delete d->su;
	d->su = 0;
	if(clear) {
		while (!d->dglist.isEmpty()) {
			delete d->dglist.takeFirst();
		}
	}
	d->state = Idle;
	setOpenMode(QIODevice::NotOpen);
	d->peer = Jid();
	d->sid = QString();
	d->remote = false;
	d->switched = false;
	d->notifyRead = false;
	d->notifyClose = false;
}

Jid S5BConnection::proxy() const
{
	return d->proxy;
}

void S5BConnection::setProxy(const Jid &proxy)
{
	d->proxy = proxy;
}

void S5BConnection::connectToJid(const Jid &peer, const QString &sid, Mode m)
{
	resetConnection(true);
	if(!d->m->isAcceptableSID(peer, sid))
		return;

	d->peer = peer;
	d->sid = sid;
	d->state = Requesting;
	d->mode = m;
#ifdef S5B_DEBUG
	qDebug("S5BConnection[%d]: connecting %s [%s]\n", d->id, qPrintable(d->peer.full()), qPrintable(d->sid));
#endif
	d->m->con_connect(this);
}

void S5BConnection::accept()
{
	if(d->state != WaitingForAccept)
		return;

	d->state = Connecting;
#ifdef S5B_DEBUG
	qDebug("S5BConnection[%d]: accepting %s [%s]\n", d->id, qPrintable(d->peer.full()), qPrintable(d->sid));
#endif
	d->m->con_accept(this);
}

void S5BConnection::close()
{
	if(d->state == Idle)
		return;

	if(d->state == WaitingForAccept)
		d->m->con_reject(this);
	else if(d->state == Active)
		d->sc->close();
#ifdef S5B_DEBUG
	qDebug("S5BConnection[%d]: closing %s [%s]\n", d->id, qPrintable(d->peer.full()), qPrintable(d->sid));
#endif
	resetConnection();
}

Jid S5BConnection::peer() const
{
	return d->peer;
}

QString S5BConnection::sid() const
{
	return d->sid;
}

BytestreamManager* S5BConnection::manager() const
{
	return d->m;
}

bool S5BConnection::isRemote() const
{
	return d->remote;
}

S5BConnection::Mode S5BConnection::mode() const
{
	return d->mode;
}

int S5BConnection::state() const
{
	return d->state;
}

qint64 S5BConnection::writeData(const char *data, qint64 maxSize)
{
	if(d->state == Active && d->mode == Stream)
		return d->sc->write(data, maxSize);
	return 0;
}

qint64 S5BConnection::readData(char *data, qint64 maxSize)
{
	if(d->sc)
		return d->sc->read(data, maxSize);
	else
		return 0;
}

qint64 S5BConnection::bytesAvailable() const
{
	if(d->sc)
		return d->sc->bytesAvailable();
	else
		return 0;
}

qint64 S5BConnection::bytesToWrite() const
{
	if(d->state == Active)
		return d->sc->bytesToWrite();
	else
		return 0;
}

void S5BConnection::writeDatagram(const S5BDatagram &i)
{
	QByteArray buf;
	buf.resize(i.data().size() + 4);
	ushort ssp = htons(i.sourcePort());
	ushort sdp = htons(i.destPort());
	QByteArray data = i.data();
	memcpy(buf.data(), &ssp, 2);
	memcpy(buf.data() + 2, &sdp, 2);
	memcpy(buf.data() + 4, data.data(), data.size());
	sendUDP(buf);
}

S5BDatagram S5BConnection::readDatagram()
{
	if(d->dglist.isEmpty())
		return S5BDatagram();
	S5BDatagram *i = d->dglist.takeFirst();
	S5BDatagram val = *i;
	delete i;
	return val;
}

int S5BConnection::datagramsAvailable() const
{
	return d->dglist.count();
}

void S5BConnection::man_waitForAccept(const S5BRequest &r)
{
	d->state = WaitingForAccept;
	d->remote = true;
	d->req = r;
	d->peer = r.from;
	d->sid = r.sid;
	d->mode = r.udp ? Datagram : Stream;
}

void S5BConnection::man_clientReady(SocksClient *sc, SocksUDP *sc_udp)
{
	d->sc = sc;
	connect(d->sc, SIGNAL(connectionClosed()), SLOT(sc_connectionClosed()));
	connect(d->sc, SIGNAL(delayedCloseFinished()), SLOT(sc_delayedCloseFinished()));
	connect(d->sc, SIGNAL(readyRead()), SLOT(sc_readyRead()));
	connect(d->sc, SIGNAL(bytesWritten(qint64)), SLOT(sc_bytesWritten(qint64)));
	connect(d->sc, SIGNAL(error(int)), SLOT(sc_error(int)));

	if(sc_udp) {
		d->su = sc_udp;
		connect(d->su, SIGNAL(packetReady(QByteArray)), SLOT(su_packetReady(QByteArray)));
	}

	d->state = Active;
	setOpenMode(QIODevice::ReadWrite);
#ifdef S5B_DEBUG
	qDebug("S5BConnection[%d]: %s [%s] <<< success >>>\n", d->id, qPrintable(d->peer.full()), qPrintable(d->sid));
#endif

	// bytes already in the stream?
	if(d->sc->bytesAvailable()) {
#ifdef S5B_DEBUG
		qDebug("Stream has %d bytes in it.\n", (int)d->sc->bytesAvailable());
#endif
		d->notifyRead = true;
	}
	// closed before it got here?
	if(!d->sc->isOpen()) {
#ifdef S5B_DEBUG
		qDebug("Stream was closed before S5B request finished?\n");
#endif
		d->notifyClose = true;
	}
	if(d->notifyRead || d->notifyClose)
		QTimer::singleShot(0, this, SLOT(doPending()));
	emit connected();
}

void S5BConnection::doPending()
{
	if(d->notifyRead) {
		if(d->notifyClose)
			QTimer::singleShot(0, this, SLOT(doPending()));
		sc_readyRead();
	}
	else if(d->notifyClose)
		sc_connectionClosed();
}

void S5BConnection::man_udpReady(const QByteArray &buf)
{
	handleUDP(buf);
}

void S5BConnection::man_failed(int x)
{
	resetConnection(true);
	if(x == S5BManager::Item::ErrRefused)
		setError(ErrRefused);
	if(x == S5BManager::Item::ErrConnect)
		setError(ErrConnect);
	if(x == S5BManager::Item::ErrWrongHost)
		setError(ErrConnect);
	if(x == S5BManager::Item::ErrProxy)
		setError(ErrProxy);
}

void S5BConnection::sc_connectionClosed()
{
	// if we have a pending read notification, postpone close
	if(d->notifyRead) {
#ifdef S5B_DEBUG
		qDebug("closed while pending read\n");
#endif
		d->notifyClose = true;
		return;
	}
	d->notifyClose = false;
	resetConnection();
	connectionClosed();
}

void S5BConnection::sc_delayedCloseFinished()
{
	// echo
	emit delayedCloseFinished();
}

void S5BConnection::sc_readyRead()
{
	if(d->mode == Datagram) {
		// throw the data away
		d->sc->readAll();
		return;
	}

	d->notifyRead = false;
	// echo
	emit readyRead();
}

void S5BConnection::sc_bytesWritten(qint64 x)
{
	// echo
	bytesWritten(x);
}

void S5BConnection::sc_error(int)
{
	resetConnection();
	setError(ErrSocket);
}

void S5BConnection::su_packetReady(const QByteArray &buf)
{
	handleUDP(buf);
}

void S5BConnection::handleUDP(const QByteArray &buf)
{
	// must be at least 4 bytes, to accomodate virtual ports
	if(buf.size() < 4)
		return; // drop

	ushort ssp, sdp;
	memcpy(&ssp, buf.data(), 2);
	memcpy(&sdp, buf.data() + 2, 2);
	int source = ntohs(ssp);
	int dest = ntohs(sdp);
	QByteArray data;
	data.resize(buf.size() - 4);
	memcpy(data.data(), buf.data() + 4, data.size());
	d->dglist.append(new S5BDatagram(source, dest, data));

	datagramReady();
}

void S5BConnection::sendUDP(const QByteArray &buf)
{
	if(d->su)
		d->su->write(buf);
	else
		d->m->con_sendUDP(this, buf);
}

//----------------------------------------------------------------------------
// S5BManager
//----------------------------------------------------------------------------
class S5BManager::Entry
{
public:
	Entry()
	{
		i = 0;
		query = 0;
		udp_init = false;
	}

	~Entry()
	{
		delete query;
	}

	S5BConnection *c;
	Item *i;
	QString sid;
	JT_S5B *query;
	StreamHost proxyInfo;
	QPointer<S5BServer> relatedServer;

	bool udp_init;
	QHostAddress udp_addr;
	int udp_port;
};

class S5BManager::Private
{
public:
	Client *client;
	S5BServer *serv;
	QList<Entry*> activeList;
	S5BConnectionList incomingConns;
	JT_PushS5B *ps;
};

S5BManager::S5BManager(Client *parent)
	: BytestreamManager(parent)
{
	// S5B needs SHA1
	//if(!QCA::isSupported(QCA::CAP_SHA1))
	//	QCA::insertProvider(createProviderHash());

	d = new Private;
	d->client = parent;
	d->serv = 0;

	d->ps = new JT_PushS5B(d->client->rootTask());
	connect(d->ps, SIGNAL(incoming(S5BRequest)), SLOT(ps_incoming(S5BRequest)));
	connect(d->ps, SIGNAL(incomingUDPSuccess(Jid,QString)), SLOT(ps_incomingUDPSuccess(Jid,QString)));
	connect(d->ps, SIGNAL(incomingActivate(Jid,QString,Jid)), SLOT(ps_incomingActivate(Jid,QString,Jid)));
}

S5BManager::~S5BManager()
{
	setServer(0);
	while (!d->incomingConns.isEmpty()) {
		delete d->incomingConns.takeFirst();
	}
	delete d->ps;
	delete d;
}

const char* S5BManager::ns()
{
	return S5B_NS;
}

Client *S5BManager::client() const
{
	return d->client;
}

S5BServer *S5BManager::server() const
{
	return d->serv;
}

void S5BManager::setServer(S5BServer *serv)
{
	if(d->serv) {
		d->serv->unlink(this);
		d->serv = 0;
	}

	if(serv) {
		d->serv = serv;
		d->serv->link(this);
	}
}

BSConnection *S5BManager::createConnection()
{
	return new S5BConnection(this);
}

S5BConnection *S5BManager::takeIncoming()
{
	if(d->incomingConns.isEmpty())
		return 0;

	S5BConnection *c = d->incomingConns.takeFirst();

	// move to activeList
	Entry *e = new Entry;
	e->c = c;
	e->sid = c->d->sid;
	d->activeList.append(e);

	return c;
}

void S5BManager::ps_incoming(const S5BRequest &req)
{
#ifdef S5B_DEBUG
	qDebug("S5BManager: incoming from %s\n", qPrintable(req.from.full()));
#endif

	bool ok = false;
	// ensure we don't already have an incoming connection from this peer+sid
	S5BConnection *c = findIncoming(req.from, req.sid);
	if(!c) {
		// do we have an active entry with this sid already?
		Entry *e = findEntryBySID(req.from, req.sid);
		if(e) {
			if(e->i) {
				// loopback
				if(req.from.compare(d->client->jid()) && (req.id == e->i->out_id)) {
#ifdef S5B_DEBUG
					qDebug("ALLOWED: loopback\n");
#endif
					ok = true;
				}
				// allowed by 'fast mode'
				else if(e->i->state == Item::Requester && e->i->targetMode == Item::Unknown) {
#ifdef S5B_DEBUG
					qDebug("ALLOWED: fast-mode\n");
#endif
					e->i->handleFast(req.hosts, req.id);
					return;
				}
			}
		}
		else {
#ifdef S5B_DEBUG
			qDebug("ALLOWED: we don't have it\n");
#endif
			ok = true;
		}
	}
	if(!ok) {
		d->ps->respondError(req.from, req.id, Stanza::Error::NotAcceptable, "SID in use");
		return;
	}

	// create an incoming connection
	c = new S5BConnection(this);
	c->man_waitForAccept(req);
	d->incomingConns.append(c);
	emit incomingReady();
}

void S5BManager::ps_incomingUDPSuccess(const Jid &from, const QString &key)
{
	Entry *e = findEntryByHash(key);
	if(e && e->i) {
		if(e->i->conn)
			e->i->conn->man_udpSuccess(from);
		else if(e->i->proxy_conn)
			e->i->proxy_conn->man_udpSuccess(from);
	}
}

void S5BManager::ps_incomingActivate(const Jid &from, const QString &sid, const Jid &streamHost)
{
	Entry *e = findEntryBySID(from, sid);
	if(e && e->i)
		e->i->incomingActivate(streamHost);
}

void S5BManager::doSuccess(const Jid &peer, const QString &id, const Jid &streamHost)
{
	d->ps->respondSuccess(peer, id, streamHost);
}

void S5BManager::doError(const Jid &peer, const QString &id,
						 Stanza::Error::ErrorCond cond, const QString &str)
{
	d->ps->respondError(peer, id, cond, str);
}

void S5BManager::doActivate(const Jid &peer, const QString &sid, const Jid &streamHost)
{
	d->ps->sendActivate(peer, sid, streamHost);
}

bool S5BManager::isAcceptableSID(const Jid &peer, const QString &sid) const
{
	QString key = makeKey(sid, d->client->jid(), peer);
	QString key_out = makeKey(sid, peer, d->client->jid()); //not valid in muc via proxy

	// if we have a server, then check through it
	if(d->serv) {
		if(findServerEntryByHash(key) || findServerEntryByHash(key_out))
			return false;
	}
	else {
		if(findEntryByHash(key) || findEntryByHash(key_out))
			return false;
	}
	return true;
}

const char* S5BManager::sidPrefix() const
{
	return "s5b_";
}

S5BConnection *S5BManager::findIncoming(const Jid &from, const QString &sid) const
{
	foreach(S5BConnection *c, d->incomingConns) {
		if(c->d->peer.compare(from) && c->d->sid == sid)
			return c;
	}
	return 0;
}

S5BManager::Entry *S5BManager::findEntry(S5BConnection *c) const
{
	foreach(Entry *e, d->activeList) {
		if(e->c == c)
			return e;
	}
	return 0;
}

S5BManager::Entry *S5BManager::findEntry(Item *i) const
{
	foreach(Entry *e, d->activeList) {
		if(e->i == i)
			return e;
	}
	return 0;
}

S5BManager::Entry *S5BManager::findEntryByHash(const QString &key) const
{
	foreach(Entry *e, d->activeList) {
		if(e->i && e->i->key == key)
			return e;
	}
	return 0;
}

S5BManager::Entry *S5BManager::findEntryBySID(const Jid &peer, const QString &sid) const
{
	foreach(Entry *e, d->activeList) {
		if(e->i && e->i->peer.compare(peer) && e->sid == sid)
			return e;
	}
	return 0;
}

S5BManager::Entry *S5BManager::findServerEntryByHash(const QString &key) const
{
	const QList<S5BManager*> &manList = d->serv->managerList();
	foreach(S5BManager *m, manList) {
		Entry *e = m->findEntryByHash(key);
		if(e)
			return e;
	}
	return 0;
}

bool S5BManager::srv_ownsHash(const QString &key) const
{
	if(findEntryByHash(key))
		return true;
	return false;
}

void S5BManager::srv_incomingReady(SocksClient *sc, const QString &key)
{
	Entry *e = findEntryByHash(key);
	if(!e->i->allowIncoming) {
		sc->requestDeny();
		SafeDelete::deleteSingle(sc);
		return;
	}
	if(e->c->d->mode == S5BConnection::Datagram)
		sc->grantUDPAssociate("", 0);
	else
		sc->grantConnect();
	e->relatedServer = (S5BServer *)sender();
	e->i->setIncomingClient(sc);
}

void S5BManager::srv_incomingUDP(bool init, const QHostAddress &addr, int port, const QString &key, const QByteArray &data)
{
	Entry *e = findEntryByHash(key);
	if(e->c->d->mode != S5BConnection::Datagram)
		return; // this key isn't in udp mode?  drop!

	if(init) {
		if(e->udp_init)
			return; // only init once

		// lock on to this sender
		e->udp_addr = addr;
		e->udp_port = port;
		e->udp_init = true;

		// reply that initialization was successful
		d->ps->sendUDPSuccess(e->c->d->peer, key);
		return;
	}

	// not initialized yet?  something went wrong
	if(!e->udp_init)
		return;

	// must come from same source as when initialized
	if(addr.toString() != e->udp_addr.toString() || port != e->udp_port)
		return;

	e->c->man_udpReady(data);
}

void S5BManager::srv_unlink()
{
	d->serv = 0;
}

void S5BManager::con_connect(S5BConnection *c)
{
	if(findEntry(c))
		return;
	Entry *e = new Entry;
	e->c = c;
	e->sid = c->d->sid;
	d->activeList.append(e);

	if(c->d->proxy.isValid()) {
		queryProxy(e);
		return;
	}
	entryContinue(e);
}

void S5BManager::con_accept(S5BConnection *c)
{
	Entry *e = findEntry(c);
	if(!e)
		return;

	if(e->c->d->req.fast) {
		if(targetShouldOfferProxy(e)) {
			queryProxy(e);
			return;
		}
	}
	entryContinue(e);
}

void S5BManager::con_reject(S5BConnection *c)
{
	d->ps->respondError(c->d->peer, c->d->req.id, Stanza::Error::NotAcceptable,
						"Not acceptable");
}

void S5BManager::con_unlink(S5BConnection *c)
{
	Entry *e = findEntry(c);
	if(!e)
		return;

	// active incoming request?  cancel it
	if(e->i && e->i->conn)
		d->ps->respondError(e->i->peer, e->i->out_id,
							Stanza::Error::NotAcceptable, "Not acceptable");
	delete e->i;
	d->activeList.removeAll(e);
	delete e;
}

void S5BManager::con_sendUDP(S5BConnection *c, const QByteArray &buf)
{
	Entry *e = findEntry(c);
	if(!e)
		return;
	if(!e->udp_init)
		return;

	if(e->relatedServer)
		e->relatedServer->writeUDP(e->udp_addr, e->udp_port, buf);
}

void S5BManager::item_accepted()
{
	Item *i = (Item *)sender();
	Entry *e = findEntry(i);

	emit e->c->accepted(); // signal
}

void S5BManager::item_tryingHosts(const StreamHostList &list)
{
	Item *i = (Item *)sender();
	Entry *e = findEntry(i);

	e->c->tryingHosts(list); // signal
}

void S5BManager::item_proxyConnect()
{
	Item *i = (Item *)sender();
	Entry *e = findEntry(i);

	e->c->proxyConnect(); // signal
}

void S5BManager::item_waitingForActivation()
{
	Item *i = (Item *)sender();
	Entry *e = findEntry(i);

	e->c->waitingForActivation(); // signal
}

void S5BManager::item_connected()
{
	Item *i = (Item *)sender();
	Entry *e = findEntry(i);

	// grab the client
	SocksClient *client = i->client;
	i->client = 0;
	SocksUDP *client_udp = i->client_udp;
	i->client_udp = 0;

	// give it to the connection
	e->c->man_clientReady(client, client_udp);
}

void S5BManager::item_error(int x)
{
	Item *i = (Item *)sender();
	Entry *e = findEntry(i);

	e->c->man_failed(x);
}

void S5BManager::entryContinue(Entry *e)
{
	e->i = new Item(this);
	e->i->proxy = e->proxyInfo;

	connect(e->i, SIGNAL(accepted()), SLOT(item_accepted()));
	connect(e->i, SIGNAL(tryingHosts(StreamHostList)), SLOT(item_tryingHosts(StreamHostList)));
	connect(e->i, SIGNAL(proxyConnect()), SLOT(item_proxyConnect()));
	connect(e->i, SIGNAL(waitingForActivation()), SLOT(item_waitingForActivation()));
	connect(e->i, SIGNAL(connected()), SLOT(item_connected()));
	connect(e->i, SIGNAL(error(int)), SLOT(item_error(int)));

	if(e->c->isRemote()) {
		const S5BRequest &req = e->c->d->req;
		e->i->startTarget(e->sid, d->client->jid(), e->c->d->peer, req.dstaddr, req.hosts, req.id, req.fast, req.udp);
	}
	else {
		e->i->startRequester(e->sid, d->client->jid(), e->c->d->peer, true, e->c->d->mode == S5BConnection::Datagram ? true: false);
		e->c->requesting(); // signal
	}
}

void S5BManager::queryProxy(Entry *e)
{
	QPointer<QObject> self = this;
	e->c->proxyQuery(); // signal
	if(!self)
		return;

#ifdef S5B_DEBUG
	qDebug("querying proxy: [%s]\n", qPrintable(e->c->d->proxy.full()));
#endif
	e->query = new JT_S5B(d->client->rootTask());
	connect(e->query, SIGNAL(finished()), SLOT(query_finished()));
	e->query->requestProxyInfo(e->c->d->proxy);
	e->query->go(true);
}

void S5BManager::query_finished()
{
	JT_S5B *query = (JT_S5B *)sender();
	Entry* e = 0;
	foreach(Entry* i, d->activeList) {
		if(i->query == query) {
			e = i;
			break;
		}
	}
	if(!e)
		return;
	e->query = 0;

#ifdef S5B_DEBUG
	qDebug("query finished: ");
#endif
	if(query->success()) {
		e->proxyInfo = query->proxyInfo();
#ifdef S5B_DEBUG
		qDebug("host/ip=[%s] port=[%d]\n", qPrintable(e->proxyInfo.host()), e->proxyInfo.port());
#endif
	}
	else {
#ifdef S5B_DEBUG
		qDebug("fail\n");
#endif
	}

	QPointer<QObject> self = this;
	e->c->proxyResult(query->success()); // signal
	if(!self)
		return;

	entryContinue(e);
}

bool S5BManager::targetShouldOfferProxy(Entry *e)
{
	if(!e->c->d->proxy.isValid())
		return false;

	// if target, don't offer any proxy if the requester already did
	const StreamHostList &hosts = e->c->d->req.hosts;
	for(StreamHostList::ConstIterator it = hosts.begin(); it != hosts.end(); ++it) {
		if((*it).isProxy())
			return false;
	}

	// ensure we don't offer the same proxy as the requester
	if(haveHost(hosts, e->c->d->proxy))
		return false;

	return true;
}

//----------------------------------------------------------------------------
// S5BManager::Item
//----------------------------------------------------------------------------
S5BManager::Item::Item(S5BManager *manager) : QObject(0)
{
	m = manager;
	task = 0;
	proxy_task = 0;
	conn = 0;
	proxy_conn = 0;
	client_udp = 0;
	client = 0;
	client_out_udp = 0;
	client_out = 0;
	resetConnection();
}

S5BManager::Item::~Item()
{
	resetConnection();
}

void S5BManager::Item::resetConnection()
{
	delete task;
	task = 0;

	delete proxy_task;
	proxy_task = 0;

	delete conn;
	conn = 0;

	delete proxy_conn;
	proxy_conn = 0;

	delete client_udp;
	client_udp = 0;

	delete client;
	client = 0;

	delete client_out_udp;
	client_out_udp = 0;

	delete client_out;
	client_out = 0;

	state = Idle;
	wantFast = false;
	targetMode = Unknown;
	fast = false;
	activated = false;
	lateProxy = false;
	connSuccess = false;
	localFailed = false;
	remoteFailed = false;
	allowIncoming = false;
	udp = false;
}

void S5BManager::Item::startRequester(const QString &_sid, const Jid &_self, const Jid &_peer, bool fast, bool _udp)
{
	sid = _sid;
	self = _self;
	peer = _peer;
	key = makeKey(sid, self, peer);
	out_key = makeKey(sid, peer, self);
	wantFast = fast;
	udp = _udp;

#ifdef S5B_DEBUG
	qDebug("S5BManager::Item initiating request %s [%s] (inhash=%s)\n", qPrintable(peer.full()), qPrintable(sid), qPrintable(key));
#endif
	state = Requester;
	doOutgoing();
}

void S5BManager::Item::startTarget(const QString &_sid, const Jid &_self,
								   const Jid &_peer, const QString &_dstaddr,
								   const StreamHostList &hosts, const QString &iq_id,
								   bool _fast, bool _udp)
{
	sid = _sid;
	peer = _peer;
	self = _self;
	in_hosts = hosts;
	in_id = iq_id;
	fast = _fast;
	key = makeKey(sid, self, peer);
	out_key = _dstaddr.isEmpty() ? makeKey(sid, peer, self) : _dstaddr;
	udp = _udp;

#ifdef S5B_DEBUG
	qDebug("S5BManager::Item incoming request %s [%s] (inhash=%s)\n", qPrintable(peer.full()), qPrintable(sid), qPrintable(key));
#endif
	state = Target;
	if(fast)
		doOutgoing();
	doIncoming();
}

void S5BManager::Item::handleFast(const StreamHostList &hosts, const QString &iq_id)
{
	targetMode = Fast;

	QPointer<QObject> self = this;
	emit accepted();
	if(!self)
		return;

	// if we already have a stream, then bounce this request
	if(client) {
		m->doError(peer, iq_id, Stanza::Error::NotAcceptable, "Not acceptable");
	}
	else {
		in_hosts = hosts;
		in_id = iq_id;
		doIncoming();
	}
}

void S5BManager::Item::doOutgoing()
{
	StreamHostList hosts;
	S5BServer *serv = m->server();
	if(serv && serv->isActive() && !haveHost(in_hosts, self)) {
		QStringList hostList = serv->hostList();
		foreach (const QString & it, hostList) {
			StreamHost h;
			h.setJid(self);
			h.setHost(it);
			h.setPort(serv->port());
			hosts += h;
		}
	}

	// if the proxy is valid, then it's ok to add (the manager already ensured that it doesn't conflict)
	if(proxy.jid().isValid())
		hosts += proxy;

	// if we're the target and we have no streamhosts of our own, then don't even bother with fast-mode
	if(state == Target && hosts.isEmpty()) {
		fast = false;
		return;
	}

	allowIncoming = true;

	task = new JT_S5B(m->client()->rootTask());
	connect(task, SIGNAL(finished()), SLOT(jt_finished()));
	task->request(peer, sid, key, hosts, state == Requester ? wantFast : false, udp);
	out_id = task->id();
	task->go(true);
}

void S5BManager::Item::doIncoming()
{
	if(in_hosts.isEmpty()) {
		doConnectError();
		return;
	}

	StreamHostList list;
	if(lateProxy) {
		// take just the proxy streamhosts
		foreach (const StreamHost& it, in_hosts) {
			if (it.isProxy())
				list += it;
		}
		lateProxy = false;
	}
	else {
		// only try doing the late proxy trick if using fast mode AND we did not offer a proxy
		if((state == Requester || (state == Target && fast)) && !proxy.jid().isValid()) {
			// take just the non-proxy streamhosts
			bool hasProxies = false;
			foreach (const StreamHost& it, in_hosts) {
				if (it.isProxy())
					hasProxies = true;
				else
					list += it;
			}
			if(hasProxies) {
				lateProxy = true;

				// no regular streamhosts?  wait for remote error
				if(list.isEmpty())
					return;
			}
		}
		else
			list = in_hosts;
	}

	conn = new S5BConnector;
	connect(conn, SIGNAL(result(bool)), SLOT(conn_result(bool)));

	QPointer<QObject> self = this;
	tryingHosts(list);
	if(!self)
		return;

	conn->start(this->self, list, out_key, udp, lateProxy ? 10 : 30);
}

void S5BManager::Item::setIncomingClient(SocksClient *sc)
{
#ifdef S5B_DEBUG
	qDebug("S5BManager::Item: %s [%s] successful incoming connection\n", qPrintable(peer.full()), qPrintable(sid));
#endif

	connect(sc, SIGNAL(readyRead()), SLOT(sc_readyRead()));
	connect(sc, SIGNAL(bytesWritten(qint64)), SLOT(sc_bytesWritten(qint64)));
	connect(sc, SIGNAL(error(int)), SLOT(sc_error(int)));

	client = sc;
	allowIncoming = false;
}

void S5BManager::Item::incomingActivate(const Jid &streamHost)
{
	if(!activated) {
		activatedStream = streamHost;
		checkForActivation();
	}
}

void S5BManager::Item::jt_finished()
{
	JT_S5B *j = task;
	task = 0;

#ifdef S5B_DEBUG
	qDebug("jt_finished: state=%s, %s\n", state == Requester ? "requester" : "target", j->success() ? "ok" : "fail");
#endif

	if(state == Requester) {
		if(targetMode == Unknown) {
			targetMode = NotFast;
			QPointer<QObject> self = this;
			emit accepted();
			if(!self)
				return;
		}
	}

	// if we've already reported successfully connecting to them, then this response doesn't matter
	if(state == Requester && connSuccess) {
		tryActivation();
		return;
	}

	if(j->success()) {
		// stop connecting out
		if(conn || lateProxy) {
			delete conn;
			conn = 0;
			doConnectError();
		}

		Jid streamHost = j->streamHostUsed();

		// they connected to us?
		if(streamHost.compare(self)) {
			if(client) {
				if(state == Requester) {
					activatedStream = streamHost;
					tryActivation();
				}
				else
					checkForActivation();
			}
			else {
#ifdef S5B_DEBUG
				qDebug("S5BManager::Item %s claims to have connected to us, but we don't see this\n", qPrintable(peer.full()));
#endif
				resetConnection();
				error(ErrWrongHost);
			}
		}
		else if(streamHost.compare(proxy.jid())) {
			// toss out any direct incoming, since it won't be used
			delete client;
			client = 0;
			allowIncoming = false;

#ifdef S5B_DEBUG
			qDebug("attempting to connect to proxy\n");
#endif
			// connect to the proxy
			proxy_conn = new S5BConnector;
			connect(proxy_conn, SIGNAL(result(bool)), SLOT(proxy_result(bool)));
			StreamHostList list;
			list += proxy;

			QPointer<QObject> self = this;
			proxyConnect();
			if(!self)
				return;

			proxy_conn->start(this->self, list, key, udp, 30);
		}
		else {
#ifdef S5B_DEBUG
			qDebug("S5BManager::Item %s claims to have connected to a streamhost we never offered\n", qPrintable(peer.full()));
#endif
			resetConnection();
			error(ErrWrongHost);
		}
	}
	else {
#ifdef S5B_DEBUG
		qDebug("S5BManager::Item %s [%s] error\n", qPrintable(peer.full()), qPrintable(sid));
#endif
		remoteFailed = true;
		statusCode = j->statusCode();

		if(lateProxy) {
			if(!conn)
				doIncoming();
		}
		else {
			// if connSuccess is true at this point, then we're a Target
			if(connSuccess)
				checkForActivation();
			else
				checkFailure();
		}
	}
}

void S5BManager::Item::conn_result(bool b)
{
	if(b) {
		SocksClient *sc = conn->takeClient();
		SocksUDP *sc_udp = conn->takeUDP();
		StreamHost h = conn->streamHostUsed();
		delete conn;
		conn = 0;
		connSuccess = true;

#ifdef S5B_DEBUG
		qDebug("S5BManager::Item: %s [%s] successful outgoing connection\n",
			   qPrintable(peer.full()), qPrintable(sid));
#endif

		connect(sc, SIGNAL(readyRead()), SLOT(sc_readyRead()));
		connect(sc, SIGNAL(bytesWritten(qint64)), SLOT(sc_bytesWritten(qint64)));
		connect(sc, SIGNAL(error(int)), SLOT(sc_error(int)));

		m->doSuccess(peer, in_id, h.jid());

		// if the first batch works, don't try proxy
		lateProxy = false;

		// if requester, run with this one
		if(state == Requester) {
			// if we had an incoming one, toss it
			delete client_udp;
			client_udp = sc_udp;
			delete client;
			client = sc;
			allowIncoming = false;
			activatedStream = peer;
			tryActivation();
		}
		else {
			client_out_udp = sc_udp;
			client_out = sc;
			checkForActivation();
		}
	}
	else {
		delete conn;
		conn = 0;

		// if we delayed the proxies for later, try now
		if(lateProxy) {
			if(remoteFailed)
				doIncoming();
		}
		else
			doConnectError();
	}
}

void S5BManager::Item::proxy_result(bool b)
{
#ifdef S5B_DEBUG
	qDebug("proxy_result: %s\n", b ? "ok" : "fail");
#endif
	if(b) {
		SocksClient *sc = proxy_conn->takeClient();
		SocksUDP *sc_udp = proxy_conn->takeUDP();
		delete proxy_conn;
		proxy_conn = 0;

		connect(sc, SIGNAL(readyRead()), SLOT(sc_readyRead()));
		connect(sc, SIGNAL(bytesWritten(qint64)), SLOT(sc_bytesWritten(qint64)));
		connect(sc, SIGNAL(error(int)), SLOT(sc_error(int)));

		client = sc;
		client_udp = sc_udp;

		// activate
#ifdef S5B_DEBUG
		qDebug("activating proxy stream\n");
#endif
		proxy_task = new JT_S5B(m->client()->rootTask());
		connect(proxy_task, SIGNAL(finished()), SLOT(proxy_finished()));
		proxy_task->requestActivation(proxy.jid(), sid, peer);
		proxy_task->go(true);
	}
	else {
		delete proxy_conn;
		proxy_conn = 0;
		resetConnection();
		error(ErrProxy);
	}
}

void S5BManager::Item::proxy_finished()
{
	JT_S5B *j = proxy_task;
	proxy_task = 0;

	if(j->success()) {
#ifdef S5B_DEBUG
		qDebug("proxy stream activated\n");
#endif
		if(state == Requester) {
			activatedStream = proxy.jid();
			tryActivation();
		}
		else
			checkForActivation();
	}
	else {
		resetConnection();
		error(ErrProxy);
	}
}

void S5BManager::Item::sc_readyRead()
{
#ifdef S5B_DEBUG
	qDebug("sc_readyRead\n");
#endif
	// only targets check for activation, and only should do it if there is no pending outgoing iq-set
	if(state == Target && !task && !proxy_task)
		checkForActivation();
}

void S5BManager::Item::sc_bytesWritten(qint64)
{
#ifdef S5B_DEBUG
	qDebug("sc_bytesWritten\n");
#endif
	// this should only happen to the requester, and should always be 1 byte (the '\r' sent earlier)
	finished();
}

void S5BManager::Item::sc_error(int)
{
#ifdef S5B_DEBUG
	qDebug("sc_error\n");
#endif
	resetConnection();
	error(ErrConnect);
}

void S5BManager::Item::doConnectError()
{
	localFailed = true;
	m->doError(peer, in_id, Stanza::Error::RemoteServerNotFound,
			   "Could not connect to given hosts");
	checkFailure();
}

void S5BManager::Item::tryActivation()
{
#ifdef S5B_DEBUG
	qDebug("tryActivation\n");
#endif
	if(activated) {
#ifdef S5B_DEBUG
		qDebug("already activated !?\n");
#endif
		return;
	}

	if(targetMode == NotFast) {
#ifdef S5B_DEBUG
		qDebug("tryActivation: NotFast\n");
#endif
		// nothing to activate, we're done
		finished();
	}
	else if(targetMode == Fast) {
		// with fast mode, we don't wait for the iq reply, so delete the task (if any)
		delete task;
		task = 0;

		activated = true;

		// if udp, activate using special stanza
		if(udp) {
			m->doActivate(peer, sid, activatedStream);
		}
		else {
#ifdef S5B_DEBUG
			qDebug("sending extra CR\n");
#endif
			// must send [CR] to activate target streamhost
			client->write("\r", 1);
		}
	}
}

void S5BManager::Item::checkForActivation()
{
	QList<SocksClient*> clientList;
	if(client)
		clientList.append(client);
	if(client_out)
		clientList.append(client_out);
	foreach(SocksClient *sc, clientList) {
#ifdef S5B_DEBUG
		qDebug("checking for activation\n");
#endif
		if(fast) {
			bool ok = false;
			if(udp) {
				if((sc == client_out && activatedStream.compare(self)) || (sc == client && !activatedStream.compare(self))) {
					clientList.removeAll(sc);
					ok = true;
				}
			}
			else {
#ifdef S5B_DEBUG
				qDebug("need CR\n");
#endif
				if(sc->bytesAvailable() >= 1) {
					clientList.removeAll(sc);
					char c;
					if(!sc->getChar(&c) || c != '\r') {
						delete sc; // FIXME breaks S5BManager::Item destructor?
						return;
					}
					ok = true;
				}
			}

			if(ok) {
				SocksUDP *sc_udp = 0;
				if(sc == client) {
					delete client_out_udp;
					client_out_udp = 0;
					sc_udp = client_udp;
				}
				else if(sc == client_out) {
					delete client_udp;
					client_udp = 0;
					sc_udp = client_out_udp;
				}

				sc->disconnect(this);
				while (!clientList.isEmpty()) {
					delete clientList.takeFirst();
				}
				client = sc;
				client_out = 0;
				client_udp = sc_udp;
				activated = true;
#ifdef S5B_DEBUG
				qDebug("activation success\n");
#endif
				break;
			}
		}
		else {
#ifdef S5B_DEBUG
			qDebug("not fast mode, no need to wait for anything\n");
#endif
			clientList.removeAll(sc);
			sc->disconnect(this);
			while (!clientList.isEmpty()) {
				delete clientList.takeFirst();
			}
			client = sc;
			client_out = 0;
			activated = true;
			break;
		}
	}

	if(activated) {
		finished();
	}
	else {
		// only emit waitingForActivation if there is nothing left to do
		if((connSuccess || localFailed) && !proxy_task && !proxy_conn)
			waitingForActivation();
	}
}

void S5BManager::Item::checkFailure()
{
	bool failed = false;
	if(state == Requester) {
		if(remoteFailed) {
			if((localFailed && targetMode == Fast) || targetMode == NotFast)
				failed = true;
		}
	}
	else {
		if(localFailed) {
			if((remoteFailed && fast) || !fast)
				failed = true;
		}
	}

	if(failed) {
		if(state == Requester) {
			resetConnection();
			if(statusCode == 404)
				error(ErrConnect);
			else
				error(ErrRefused);
		}
		else {
			resetConnection();
			error(ErrConnect);
		}
	}
}

void S5BManager::Item::finished()
{
	client->disconnect(this);
	state = Active;
#ifdef S5B_DEBUG
	qDebug("S5BManager::Item %s [%s] linked successfully\n", qPrintable(peer.full()), qPrintable(sid));
#endif
	emit connected();
}

//----------------------------------------------------------------------------
// S5BConnector
//----------------------------------------------------------------------------
class S5BConnector::Item : public QObject
{
	Q_OBJECT
public:
	SocksClient *client;
	SocksUDP *client_udp;
	StreamHost host;
	QString key;
	bool udp;
	int udp_tries;
	QTimer t;
	Jid jid;

	Item(const Jid &self, const StreamHost &_host, const QString &_key, bool _udp) : QObject(0)
	{
		jid = self;
		host = _host;
		key = _key;
		udp = _udp;
		client = new SocksClient;
		client_udp = 0;
		connect(client, SIGNAL(connected()), SLOT(sc_connected()));
		connect(client, SIGNAL(error(int)), SLOT(sc_error(int)));
		connect(&t, SIGNAL(timeout()), SLOT(trySendUDP()));
	}

	~Item()
	{
		cleanup();
	}

	void start()
	{
		client->connectToHost(host.host(), host.port(), key, 0, udp);
	}

	void udpSuccess()
	{
		t.stop();
		client_udp->change(key, 0); // flip over to the data port
		success();
	}

signals:
	void result(bool);

private slots:
	void sc_connected()
	{
		// if udp, need to send init packet before we are good
		if(udp) {
			// port 1 is init
			client_udp = client->createUDP(key, 1, client->peerAddress(), client->peerPort());
			udp_tries = 0;
			t.start(5000);
			trySendUDP();
			return;
		}

		success();
	}

	void sc_error(int)
	{
#ifdef S5B_DEBUG
		qDebug("S5BConnector[%s]: error\n", qPrintable(host.host()));
#endif
		cleanup();
		result(false);
	}

	void trySendUDP()
	{
		if(udp_tries == 5) {
			t.stop();
			cleanup();
			result(false);
			return;
		}

		// send initialization with our JID
		QByteArray a(jid.full().toUtf8());
		client_udp->write(a);
		++udp_tries;
	}

private:
	void cleanup()
	{
		delete client_udp;
		client_udp = 0;
		delete client;
		client = 0;
	}

	void success()
	{
#ifdef S5B_DEBUG
		qDebug("S5BConnector[%s]: success\n", qPrintable(host.host()));
#endif
		client->disconnect(this);
		result(true);
	}
};

class S5BConnector::Private
{
public:
	SocksClient *active;
	SocksUDP *active_udp;
	QList<Item*> itemList;
	QString key;
	StreamHost activeHost;
	QTimer t;
};

S5BConnector::S5BConnector(QObject *parent)
:QObject(parent)
{
	d = new Private;
	d->active = 0;
	d->active_udp = 0;
	connect(&d->t, SIGNAL(timeout()), SLOT(t_timeout()));
}

S5BConnector::~S5BConnector()
{
	resetConnection();
	delete d;
}

void S5BConnector::resetConnection()
{
	d->t.stop();
	delete d->active_udp;
	d->active_udp = 0;
	delete d->active;
	d->active = 0;
	while (!d->itemList.empty()) {
		delete d->itemList.takeFirst();
	}
}

void S5BConnector::start(const Jid &self, const StreamHostList &hosts, const QString &key, bool udp, int timeout)
{
	resetConnection();

#ifdef S5B_DEBUG
	qDebug("S5BConnector: starting [%p]!\n", this);
#endif
	for(StreamHostList::ConstIterator it = hosts.begin(); it != hosts.end(); ++it) {
		Item *i = new Item(self, *it, key, udp);
		connect(i, SIGNAL(result(bool)), SLOT(item_result(bool)));
		d->itemList.append(i);
		i->start();
	}
	d->t.start(timeout * 1000);
}

SocksClient *S5BConnector::takeClient()
{
	SocksClient *c = d->active;
	d->active = 0;
	return c;
}

SocksUDP *S5BConnector::takeUDP()
{
	SocksUDP *c = d->active_udp;
	d->active_udp = 0;
	return c;
}

StreamHost S5BConnector::streamHostUsed() const
{
	return d->activeHost;
}

void S5BConnector::item_result(bool b)
{
	Item *i = (Item *)sender();
	if(b) {
		d->active = i->client;
		i->client = 0;
		d->active_udp = i->client_udp;
		i->client_udp = 0;
		d->activeHost = i->host;
		while (!d->itemList.isEmpty()) {
			delete d->itemList.takeFirst();
		}
		d->t.stop();
#ifdef S5B_DEBUG
		qDebug("S5BConnector: complete! [%p]\n", this);
#endif
		emit result(true);
	}
	else {
		d->itemList.removeAll(i);
		delete i;
		if(d->itemList.isEmpty()) {
			d->t.stop();
#ifdef S5B_DEBUG
			qDebug("S5BConnector: failed! [%p]\n", this);
#endif
			emit result(false);
		}
	}
}

void S5BConnector::t_timeout()
{
	resetConnection();
#ifdef S5B_DEBUG
	qDebug("S5BConnector: failed! (timeout)\n");
#endif
	result(false);
}

void S5BConnector::man_udpSuccess(const Jid &streamHost)
{
	// was anyone sending to this streamhost?
	foreach(Item *i, d->itemList) {
		if(i->host.jid().compare(streamHost) && i->client_udp) {
			i->udpSuccess();
			return;
		}
	}
}

//----------------------------------------------------------------------------
// S5BServer
//----------------------------------------------------------------------------
class S5BServer::Item : public QObject
{
	Q_OBJECT
public:
	SocksClient *client;
	QString host;
	QTimer expire;

	Item(SocksClient *c) : QObject(0)
	{
		client = c;
		connect(client, SIGNAL(incomingMethods(int)), SLOT(sc_incomingMethods(int)));
		connect(client, SIGNAL(incomingConnectRequest(QString,int)), SLOT(sc_incomingConnectRequest(QString,int)));
		connect(client, SIGNAL(error(int)), SLOT(sc_error(int)));

		connect(&expire, SIGNAL(timeout()), SLOT(doError()));
		resetExpiration();
	}

	~Item()
	{
		delete client;
	}

	void resetExpiration()
	{
		expire.start(30000);
	}

signals:
	void result(bool);

private slots:
	void doError()
	{
		expire.stop();
		delete client;
		client = 0;
		result(false);
	}

	void sc_incomingMethods(int m)
	{
		if(m & SocksClient::AuthNone)
			client->chooseMethod(SocksClient::AuthNone);
		else
			doError();
	}

	void sc_incomingConnectRequest(const QString &_host, int port)
	{
		if(port == 0) {
			host = _host;
			client->disconnect(this);
			emit result(true);
		}
		else
			doError();
	}

	void sc_error(int)
	{
		doError();
	}
};

class S5BServer::Private
{
public:
	SocksServer serv;
	QStringList hostList;
	QList<S5BManager*> manList;
	QList<Item*> itemList;
};

S5BServer::S5BServer(QObject *parent)
:QObject(parent)
{
	d = new Private;
	connect(&d->serv, SIGNAL(incomingReady()), SLOT(ss_incomingReady()));
	connect(&d->serv, SIGNAL(incomingUDP(QString,int,QHostAddress,int,QByteArray)), SLOT(ss_incomingUDP(QString,int,QHostAddress,int,QByteArray)));
}

S5BServer::~S5BServer()
{
	unlinkAll();
	delete d;
}

bool S5BServer::isActive() const
{
	return d->serv.isActive();
}

bool S5BServer::start(int port)
{
	d->serv.stop();
	//return d->serv.listen(port, true);
	return d->serv.listen(port);
}

void S5BServer::stop()
{
	d->serv.stop();
}

void S5BServer::setHostList(const QStringList &list)
{
	d->hostList = list;
}

QStringList S5BServer::hostList() const
{
	return d->hostList;
}

int S5BServer::port() const
{
	return d->serv.port();
}

void S5BServer::ss_incomingReady()
{
	Item *i = new Item(d->serv.takeIncoming());
#ifdef S5B_DEBUG
	qDebug("S5BServer: incoming connection from %s:%d\n", qPrintable(i->client->peerAddress().toString()), i->client->peerPort());
#endif
	connect(i, SIGNAL(result(bool)), SLOT(item_result(bool)));
	d->itemList.append(i);
}

void S5BServer::ss_incomingUDP(const QString &host, int port, const QHostAddress &addr, int sourcePort, const QByteArray &data)
{
	if(port != 0 || port != 1)
		return;

	foreach(S5BManager* m, d->manList) {
		if(m->srv_ownsHash(host)) {
			m->srv_incomingUDP(port == 1 ? true : false, addr, sourcePort, host, data);
			return;
		}
	}
}

void S5BServer::item_result(bool b)
{
	Item *i = (Item *)sender();
#ifdef S5B_DEBUG
	qDebug("S5BServer item result: %d\n", b);
#endif
	if(!b) {
		d->itemList.removeAll(i);
		delete i;
		return;
	}

	SocksClient *c = i->client;
	i->client = 0;
	QString key = i->host;
	d->itemList.removeAll(i);
	delete i;

	// find the appropriate manager for this incoming connection
	foreach(S5BManager *m, d->manList) {
		if(m->srv_ownsHash(key)) {
			m->srv_incomingReady(c, key);
			return;
		}
	}

#ifdef S5B_DEBUG
	qDebug("S5BServer item result: unknown hash [%s]\n", qPrintable(key));
#endif

	// throw it away
	delete c;
}

void S5BServer::link(S5BManager *m)
{
	d->manList.append(m);
}

void S5BServer::unlink(S5BManager *m)
{
	d->manList.removeAll(m);
}

void S5BServer::unlinkAll()
{
	foreach(S5BManager *m, d->manList) {
		m->srv_unlink();
	}
	d->manList.clear();
}

const QList<S5BManager*> & S5BServer::managerList() const
{
	return d->manList;
}

void S5BServer::writeUDP(const QHostAddress &addr, int port, const QByteArray &data)
{
	d->serv.writeUDP(addr, port, data);
}

//----------------------------------------------------------------------------
// JT_S5B
//----------------------------------------------------------------------------
class JT_S5B::Private
{
public:
	QDomElement iq;
	Jid to;
	Jid streamHost;
	StreamHost proxyInfo;
	int mode;
	QTimer t;
};

JT_S5B::JT_S5B(Task *parent)
:Task(parent)
{
	d = new Private;
	d->mode = -1;
	connect(&d->t, SIGNAL(timeout()), SLOT(t_timeout()));
}

JT_S5B::~JT_S5B()
{
	delete d;
}

void JT_S5B::request(const Jid &to, const QString &sid, const QString &dstaddr,
					 const StreamHostList &hosts, bool fast, bool udp)
{
	d->mode = 0;

	QDomElement iq;
	d->to = to;
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", S5B_NS);
	query.setAttribute("sid", sid);
	if (!client()->groupChatNick(to.domain(), to.node()).isEmpty()) {
		query.setAttribute("dstaddr", dstaddr); // special case for muc as in xep-0065rc3
	}
	query.setAttribute("mode", udp ? "udp" : "tcp" );
	iq.appendChild(query);
	for(StreamHostList::ConstIterator it = hosts.begin(); it != hosts.end(); ++it) {
		QDomElement shost = doc()->createElement("streamhost");
		shost.setAttribute("jid", (*it).jid().full());
		shost.setAttribute("host", (*it).host());
		shost.setAttribute("port", QString::number((*it).port()));
		if((*it).isProxy()) {
			QDomElement p = doc()->createElement("proxy");
			p.setAttribute("xmlns", "http://affinix.com/jabber/stream");
			shost.appendChild(p);
		}
		query.appendChild(shost);
	}
	if(fast) {
		QDomElement e = doc()->createElement("fast");
		e.setAttribute("xmlns", "http://affinix.com/jabber/stream");
		query.appendChild(e);
	}
	d->iq = iq;
}

void JT_S5B::requestProxyInfo(const Jid &to)
{
	d->mode = 1;

	QDomElement iq;
	d->to = to;
	iq = createIQ(doc(), "get", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", S5B_NS);
	iq.appendChild(query);
	d->iq = iq;
}

void JT_S5B::requestActivation(const Jid &to, const QString &sid, const Jid &target)
{
	d->mode = 2;

	QDomElement iq;
	d->to = to;
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", S5B_NS);
	query.setAttribute("sid", sid);
	iq.appendChild(query);
	QDomElement act = doc()->createElement("activate");
	act.appendChild(doc()->createTextNode(target.full()));
	query.appendChild(act);
	d->iq = iq;
}

void JT_S5B::onGo()
{
	if(d->mode == 1) {
		d->t.setSingleShot(true);
		d->t.start(15000);
	}
	send(d->iq);
}

void JT_S5B::onDisconnect()
{
	d->t.stop();
}

bool JT_S5B::take(const QDomElement &x)
{
	if(d->mode == -1)
		return false;

	if(!iqVerify(x, d->to, id()))
		return false;

	d->t.stop();

	if(x.attribute("type") == "result") {
		QDomElement q = queryTag(x);
		if(d->mode == 0) {
			d->streamHost = "";
			if(!q.isNull()) {
				QDomElement shost = q.elementsByTagName("streamhost-used").item(0).toElement();
				if(!shost.isNull())
					d->streamHost = shost.attribute("jid");
			}

			setSuccess();
		}
		else if(d->mode == 1) {
			if(!q.isNull()) {
				QDomElement shost = q.elementsByTagName("streamhost").item(0).toElement();
				if(!shost.isNull()) {
					Jid j = shost.attribute("jid");
					if(j.isValid()) {
						QString host = shost.attribute("host");
						if(!host.isEmpty()) {
							int port = shost.attribute("port").toInt();
							StreamHost h;
							h.setJid(j);
							h.setHost(host);
							h.setPort(port);
							h.setIsProxy(true);
							d->proxyInfo = h;
						}
					}
				}
			}

			setSuccess();
		}
		else {
			setSuccess();
		}
	}
	else {
		setError(x);
	}

	return true;
}

void JT_S5B::t_timeout()
{
	d->mode = -1;
	setError(500, "Timed out");
}

Jid JT_S5B::streamHostUsed() const
{
	return d->streamHost;
}

StreamHost JT_S5B::proxyInfo() const
{
	return d->proxyInfo;
}

//----------------------------------------------------------------------------
// JT_PushS5B
//----------------------------------------------------------------------------
JT_PushS5B::JT_PushS5B(Task *parent)
:Task(parent)
{
}

JT_PushS5B::~JT_PushS5B()
{
}

int JT_PushS5B::priority() const
{
	return 1;
}

bool JT_PushS5B::take(const QDomElement &e)
{
	// look for udpsuccess
	if(e.tagName() == "message") {
		QDomElement x = e.elementsByTagName("udpsuccess").item(0).toElement();
		if(!x.isNull() && x.attribute("xmlns") == S5B_NS) {
			incomingUDPSuccess(Jid(x.attribute("from")), x.attribute("dstaddr"));
			return true;
		}
		x = e.elementsByTagName("activate").item(0).toElement();
		if(!x.isNull() && x.attribute("xmlns") == "http://affinix.com/jabber/stream") {
			incomingActivate(Jid(x.attribute("from")), x.attribute("sid"), Jid(x.attribute("jid")));
			return true;
		}
		return false;
	}

	// must be an iq-set tag
	if(e.tagName() != "iq")
		return false;
	if(e.attribute("type") != "set")
		return false;
	if(queryNS(e) != S5B_NS)
		return false;

	Jid from(e.attribute("from"));
	QDomElement q = queryTag(e);
	QString sid = q.attribute("sid");

	StreamHostList hosts;
	QDomNodeList nl = q.elementsByTagName("streamhost");
	for(int n = 0; n < nl.count(); ++n) {
		QDomElement shost = nl.item(n).toElement();
		if(hosts.count() < MAXSTREAMHOSTS) {
			Jid j = shost.attribute("jid");
			if(!j.isValid())
				continue;
			QString host = shost.attribute("host");
			if(host.isEmpty())
				continue;
			int port = shost.attribute("port").toInt();
			QDomElement p = shost.elementsByTagName("proxy").item(0).toElement();
			bool isProxy = false;
			if(!p.isNull() && p.attribute("xmlns") == "http://affinix.com/jabber/stream")
				isProxy = true;

			StreamHost h;
			h.setJid(j);
			h.setHost(host);
			h.setPort(port);
			h.setIsProxy(isProxy);
			hosts += h;
		}
	}

	bool fast = false;
	QDomElement t;
	t = q.elementsByTagName("fast").item(0).toElement();
	if(!t.isNull() && t.attribute("xmlns") == "http://affinix.com/jabber/stream")
		fast = true;

	S5BRequest r;
	r.from = from;
	r.id = e.attribute("id");
	r.sid = sid;
	r.dstaddr = q.attribute("dstaddr"); // special case for muc as in xep-0065rc3
	r.hosts = hosts;
	r.fast = fast;
	r.udp = q.attribute("mode") == "udp" ? true: false;

	emit incoming(r);
	return true;
}

void JT_PushS5B::respondSuccess(const Jid &to, const QString &id, const Jid &streamHost)
{
	QDomElement iq = createIQ(doc(), "result", to.full(), id);
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", S5B_NS);
	iq.appendChild(query);
	QDomElement shost = doc()->createElement("streamhost-used");
	shost.setAttribute("jid", streamHost.full());
	query.appendChild(shost);
	send(iq);
}

void JT_PushS5B::respondError(const Jid &to, const QString &id,
							  Stanza::Error::ErrorCond cond, const QString &str)
{
	QDomElement iq = createIQ(doc(), "error", to.full(), id);
	Stanza::Error error(Stanza::Error::Cancel, cond, str);
	iq.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
	send(iq);
}

void JT_PushS5B::sendUDPSuccess(const Jid &to, const QString &dstaddr)
{
	QDomElement m = doc()->createElement("message");
	m.setAttribute("to", to.full());
	QDomElement u = doc()->createElement("udpsuccess");
	u.setAttribute("xmlns", S5B_NS);
	u.setAttribute("dstaddr", dstaddr);
	m.appendChild(u);
	send(m);
}

void JT_PushS5B::sendActivate(const Jid &to, const QString &sid, const Jid &streamHost)
{
	QDomElement m = doc()->createElement("message");
	m.setAttribute("to", to.full());
	QDomElement act = doc()->createElement("activate");
	act.setAttribute("xmlns", "http://affinix.com/jabber/stream");
	act.setAttribute("sid", sid);
	act.setAttribute("jid", streamHost.full());
	m.appendChild(act);
	send(m);
}

//----------------------------------------------------------------------------
// StreamHost
//----------------------------------------------------------------------------
StreamHost::StreamHost()
{
	v_port = -1;
	proxy = false;
}

const Jid & StreamHost::jid() const
{
	return j;
}

const QString & StreamHost::host() const
{
	return v_host;
}

int StreamHost::port() const
{
	return v_port;
}

bool StreamHost::isProxy() const
{
	return proxy;
}

void StreamHost::setJid(const Jid &_j)
{
	j = _j;
}

void StreamHost::setHost(const QString &host)
{
	v_host = host;
}

void StreamHost::setPort(int port)
{
	v_port = port;
}

void StreamHost::setIsProxy(bool b)
{
	proxy = b;
}

}

#include "s5b.moc"
