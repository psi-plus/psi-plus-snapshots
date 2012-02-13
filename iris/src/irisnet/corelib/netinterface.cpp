/*
 * Copyright (C) 2006  Justin Karneges
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "netinterface.h"

#include "irisnetplugin.h"
#include "irisnetglobal_p.h"

#include <QWaitCondition>
#include <QPointer>
#include <QDebug>

namespace XMPP {

//----------------------------------------------------------------------------
// NetTracker
//----------------------------------------------------------------------------
class NetTracker : public QObject {
	Q_OBJECT
public:
	QList<NetInterfaceProvider::Info> getInterfaces() {
		QMutexLocker locker(&m);

		return info;
	}

	NetTracker() {
		QList<IrisNetProvider*> list = irisNetProviders();

		c = 0;
		foreach(IrisNetProvider* p, list) {
			c = p->createNetInterfaceProvider();
			if(c) break;
		}
		Q_ASSERT(c); // we have built-in support, so this should never fail
		connect(c, SIGNAL(updated()), SLOT(c_updated()));

		c->start();
		info = filterList(c->interfaces());
	}

	~NetTracker() {
		QMutexLocker locker(&m);
		delete c;
	}


signals:
	void updated();
private:


	static QList<NetInterfaceProvider::Info> filterList(const QList<NetInterfaceProvider::Info> &in) {
		QList<NetInterfaceProvider::Info> out;
		for(int n = 0; n < in.count(); ++n)
		{
			if(!in[n].isLoopback) out += in[n];
		}
		return out;
	}

private slots:
	void c_updated() {
		{
			QMutexLocker locker(&m);
			info = filterList(c->interfaces());
		}
		emit updated();
	}


private:
	// this are all protected by m
	NetInterfaceProvider *c;
	QMutex m;
	QList<NetInterfaceProvider::Info> info;

};


// Global because static getRef needs this too.
Q_GLOBAL_STATIC(QMutex, nettracker_mutex)

class NetTrackerThread : public QThread {
	Q_OBJECT
public:
	/** Get a reference to the NetTracker singleton.
	    Calls to getInterfaces will immediately give valid results
	 */
	static NetTrackerThread* getRef() {
		QMutexLocker locker(nettracker_mutex());

		if (!self) {
			self = new NetTrackerThread();
		}
		self->refs++;
		return self;
	}

	/** Release reference.
	 */
	void releaseRef() {
		QMutexLocker locker(nettracker_mutex());

		Q_ASSERT(refs > 0);
		refs--;
		if (refs <= 0) {
			exit(0);
			wait();
			delete this;
			self = 0;
		}
	}

	QList<NetInterfaceProvider::Info> getInterfaces() {
		return nettracker->getInterfaces();
	}


	~NetTrackerThread() {
		// locked from caller
	}


signals:
	void updated();
private:
	NetTrackerThread() {
		// locked from caller
		refs = 0;
		moveToThread(QCoreApplication::instance()->thread());
		startMutex = new QMutex();
		{
			QMutexLocker startLocker(startMutex);
			start();
			startCond.wait(startMutex); // wait for thread startup finished
		}
		delete startMutex;
		startMutex = 0;
	}

	void run() {
		{
			QMutexLocker locker(startMutex);

			nettracker = new NetTracker();
			connect(nettracker, SIGNAL(updated()), SIGNAL(updated()), Qt::DirectConnection);

			startCond.wakeOne(); // we're ready to serve.
		}
		exec();
		delete nettracker;
		nettracker = 0;
	}

private:
	QWaitCondition startCond;
	QMutex *startMutex;
	// these are all protected by global nettracker_mutex.
	int refs;
	static NetTrackerThread *self;
	NetTracker *nettracker;
};

NetTrackerThread *NetTrackerThread::self = 0;


//----------------------------------------------------------------------------
// NetInterface
//----------------------------------------------------------------------------
class NetInterfacePrivate : public QObject
{
	Q_OBJECT
public:
	friend class NetInterfaceManagerPrivate;

	NetInterface *q;

	QPointer<NetInterfaceManager> man;
	bool valid;
	QString id, name;
	QList<QHostAddress> addrs;
	QHostAddress gw;

	NetInterfacePrivate(NetInterface *_q) : QObject(_q), q(_q)
	{
		valid = false;
	}

	void doUnavailable()
	{
		if (!valid) return;
		valid = false;
		if (man.isNull()) return;
		man->unreg(q);
		emit q->unavailable();
	}
};

NetInterface::NetInterface(const QString &id, NetInterfaceManager *manager)
				: QObject(manager)
{
	d = new NetInterfacePrivate(this);
	d->man = manager;

	NetInterfaceProvider::Info *info = (NetInterfaceProvider::Info *)d->man->reg(id, this);
	if (info) {
		d->valid = true;
		d->id = info->id;
		d->name = info->name;
		d->addrs = info->addresses;
		d->gw = info->gateway;
		delete info;
	}
}

NetInterface::~NetInterface()
{
	if (d->valid && !d->man.isNull()) d->man->unreg(this);
	delete d;
}

bool NetInterface::isValid() const
{
	return d->valid && !d->man.isNull();
}

QString NetInterface::id() const
{
	return d->id;
}

QString NetInterface::name() const
{
	return d->name;
}

QList<QHostAddress> NetInterface::addresses() const
{
	return d->addrs;
}

QHostAddress NetInterface::gateway() const
{
	return d->gw;
}

//----------------------------------------------------------------------------
// NetInterfaceManager
//----------------------------------------------------------------------------
class NetInterfaceManagerPrivate : public QObject
{
	Q_OBJECT
public:
	NetInterfaceManager *q;

	QList<NetInterfaceProvider::Info> info;
	QList<NetInterface*> listeners;
	NetTrackerThread *tracker;

	bool pending;

	NetInterfaceManagerPrivate(NetInterfaceManager *_q) : QObject(_q), q(_q)
	{
		tracker = NetTrackerThread::getRef();
		pending = false;
		connect(tracker, SIGNAL(updated()), SLOT(tracker_updated()));
	}

	~NetInterfaceManagerPrivate() {
		tracker->releaseRef();
		tracker = 0;
	}

	static int lookup(const QList<NetInterfaceProvider::Info> &list, const QString &id)
	{
		for(int n = 0; n < list.count(); ++n) {
			if(list[n].id == id) return n;
		}
		return -1;
	}

	static bool sameContent(const NetInterfaceProvider::Info &a, const NetInterfaceProvider::Info &b)
	{
		// assume ids are the same already
		return (a.name == b.name && a.isLoopback == b.isLoopback && a.addresses == b.addresses && a.gateway == b.gateway);
	}

	void do_update()
	{
		// grab the latest info
		QList<NetInterfaceProvider::Info> newinfo = tracker->getInterfaces();

		QStringList here_ids, gone_ids;

		// removed / changed
		for(int n = 0; n < info.count(); ++n)
		{
			int i = lookup(newinfo, info[n].id);
			// id is still here
			if(i != -1) {
				// content changed?
				if(!sameContent(info[n], newinfo[i])) {
					gone_ids += info[n].id;
					here_ids += info[n].id;
				}
			} else { // id is gone
				gone_ids += info[n].id;
			}
		}

		// added
		for(int n = 0; n < newinfo.count(); ++n) {
			int i = lookup(info, newinfo[n].id);
			if(i == -1)
				here_ids += newinfo[n].id;
		}
		info = newinfo;

		// announce gone
		for(int n = 0; n < gone_ids.count(); ++n) {
			// work on a copy, just in case the list changes.
			//   it is important to make the copy here, and not
			//   outside the outer loop, in case the items
			//   get deleted
			QList<NetInterface*> list = listeners;
			for(int i = 0; i < list.count(); ++i) {
				if(list[i]->d->id == gone_ids[n]) {
					list[i]->d->doUnavailable();
				}
			}
		}

		// announce here
		for(int n = 0; n < here_ids.count(); ++n)
			emit q->interfaceAvailable(here_ids[n]);
	}

public slots:
	void tracker_updated()
	{
		// collapse multiple updates by queuing up an update if there isn't any queued yet.
		if(!pending) {
			QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
			pending = true;
		}
	}

	void update()
	{
		pending = false;
		do_update();
	}
};

NetInterfaceManager::NetInterfaceManager(QObject *parent)
				:QObject(parent)
{
	d = new NetInterfaceManagerPrivate(this);
}

NetInterfaceManager::~NetInterfaceManager()
{
	delete d;
}

QStringList NetInterfaceManager::interfaces() const
{
	d->info = d->tracker->getInterfaces();
	QStringList out;
	for(int n = 0; n < d->info.count(); ++n) {
		out += d->info[n].id;
	}
	return out;
}

QString NetInterfaceManager::interfaceForAddress(const QHostAddress &a)
{
	NetInterfaceManager netman;
	QStringList list = netman.interfaces();
	for(int n = 0; n < list.count(); ++n) {
		NetInterface iface(list[n], &netman);
		if(iface.addresses().contains(a)) return list[n];
	}
	return QString();
}

void *NetInterfaceManager::reg(const QString &id, NetInterface *i)
{
	for(int n = 0; n < d->info.count(); ++n) {
		if(d->info[n].id == id) {
			d->listeners += i;
			return new NetInterfaceProvider::Info(d->info[n]);
		}
	}
	return 0;
}

void NetInterfaceManager::unreg(NetInterface *i)
{
	d->listeners.removeAll(i);
}

}

#include "netinterface.moc"
