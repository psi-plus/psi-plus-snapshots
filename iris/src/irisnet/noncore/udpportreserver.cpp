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

#include "udpportreserver.h"

#include <QUdpSocket>

namespace XMPP {

class UdpPortReserver::Private : public QObject
{
	Q_OBJECT

public:
	class Item
	{
	public:
		int port; // port to reserve
		bool lent;

		// list of sockets for this port, one socket per address.
		//   note that we may have sockets bound for addresses
		//   we no longer care about, if we are currently lending
		//   them out
		QList<QUdpSocket*> sockList;

		// keep track of which addresses we lent out
		QList<QHostAddress> lentAddrs;

		Item() :
			port(-1),
			lent(false)
		{
		}

		bool haveAddress(const QHostAddress &addr) const
		{
			foreach(const QUdpSocket *sock, sockList)
			{
				if(sock->localAddress() == addr)
					return true;
			}

			return false;
		}
	};

	UdpPortReserver *q;
	QList<QHostAddress> addrs;
	QList<int> ports; // sorted
	QList<Item> items; // in order sorted by port

	Private(UdpPortReserver *_q) :
		QObject(_q),
		q(_q)
	{
	}

	~Private()
	{
		bool lendingAny = false;
		foreach(const Item &i, items)
		{
			if(i.lent)
			{
				lendingAny = true;
				break;
			}
		}

		Q_ASSERT(!lendingAny);

		foreach(const Item &i, items)
		{
			foreach(QUdpSocket *sock, i.sockList)
				sock->deleteLater();
		}
	}

	void updateAddresses(const QList<QHostAddress> &newAddrs)
	{
		addrs = newAddrs;

		tryBind();
		tryCleanup();
	}

	void updatePorts(const QList<int> &newPorts)
	{
		QList<int> added;
		foreach(int x, newPorts)
		{
			bool found = false;
			foreach(const Item &i, items)
			{
				if(i.port == x)
				{
					found = true;
					break;
				}
			}

			if(!found)
				added += x;
		}

		ports = newPorts;

		// keep ports in sorted order
		qSort(ports);

		foreach(int x, added)
		{
			int insert_before = items.count();
			for(int n = 0; n < items.count(); ++n)
			{
				if(x < items[n].port)
				{
					insert_before = n;
					break;
				}
			}

			Item i;
			i.port = x;
			items.insert(insert_before, i);
		}

		tryBind();
		tryCleanup();
	}

	bool reservedAll() const
	{
		bool ok = true;
		foreach(const Item &i, items)
		{
			// skip ports we don't care about
			if(!ports.contains(i.port))
				continue;

			if(!isReserved(i))
			{
				ok = false;
				break;
			}
		}

		return ok;
	}

	QList<QUdpSocket*> borrowSockets(int portCount, QObject *parent)
	{
		Q_ASSERT(portCount > 0);

		QList<QUdpSocket*> out;

		if(portCount > 1)
		{
			// first try to see if we can find something all in a
			//   row, starting with best alignment to worst
			for(int align = portCount; align >= 2; align /= 2)
			{
				int at = findConsecutive(portCount, align);
				if(at != -1)
				{
					for(int n = 0; n < portCount; ++n)
						out += lendItem(&items[at + n], parent);

					break;
				}
			}

			if(out.isEmpty())
			{
				// otherwise, try splitting them up into
				//   smaller consecutive chunks
				int chunks[2];
				chunks[0] = portCount / 2 + (portCount % 2);
				chunks[1] = portCount / 2;
				for(int n = 0; n < 2; ++n)
					out += borrowSockets(chunks[n], parent);
			}
		}
		else
		{
			// take the next available port
			int at = findConsecutive(1, 1);
			if(at != -1)
				out += lendItem(&items[at], parent);
		}

		return out;
	}

	void returnSockets(const QList<QUdpSocket*> &sockList)
	{
		foreach(QUdpSocket *sock, sockList)
		{
			int at = -1;
			for(int n = 0; n < items.count(); ++n)
			{
				if(items[n].sockList.contains(sock))
				{
					at = n;
					break;
				}
			}

			Q_ASSERT(at != -1);

			Item &i = items[at];

			QHostAddress a = sock->localAddress();

			Q_ASSERT(i.lent);
			Q_ASSERT(i.lentAddrs.contains(a));

			sock->setParent(q);
			connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));

			i.lentAddrs.removeAll(a);
			if(i.lentAddrs.isEmpty())
				i.lent = false;
		}

		tryCleanup();
	}

private slots:
	void sock_readyRead()
	{
		QUdpSocket *sock = (QUdpSocket *)sender();

		// eat all packets
		while(sock->hasPendingDatagrams())
			sock->readDatagram(0, 0);
	}

private:
	void tryBind()
	{
		for(int n = 0; n < items.count(); ++n)
		{
			Item &i = items[n];

			// skip ports we don't care about
			if(!ports.contains(i.port))
				continue;

			QList<QHostAddress> neededAddrs;
			foreach(const QHostAddress &a, addrs)
			{
				if(!i.haveAddress(a))
					neededAddrs += a;
			}

			foreach(const QHostAddress &a, neededAddrs)
			{
				QUdpSocket *sock = new QUdpSocket(q);

				if(!sock->bind(a, i.port))
				{
					delete sock;
					continue;
				}

				connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));

				i.sockList += sock;
			}
		}
	}

	void tryCleanup()
	{
		for(int n = 0; n < items.count(); ++n)
		{
			Item &i = items[n];

			// don't care about this port anymore?
			if(!i.lent && !ports.contains(i.port))
			{
				foreach(QUdpSocket *sock, i.sockList)
					sock->deleteLater();

				items.removeAt(n);
				--n; // adjust position
				continue;
			}

			// any addresses we don't care about?
			for(int k = 0; k < i.sockList.count(); ++k)
			{
				QUdpSocket *sock = i.sockList[k];

				QHostAddress a = sock->localAddress();

				if(!addrs.contains(a) && !i.lentAddrs.contains(a))
				{
					sock->deleteLater();
					i.sockList.removeAt(k);
					--k; // adjust position
					continue;
				}
			}
		}
	}

	bool isReserved(const Item &i) const
	{
		// must have desired addrs to consider a port reserved
		if(addrs.isEmpty())
			return false;

		foreach(const QHostAddress &a, addrs)
		{
			if(!i.haveAddress(a))
				return false;
		}

		return true;
	}

	bool isConsecutive(int at, int count) const
	{
		if(at + count > items.count())
			return false;

		for(int n = 0; n < count; ++n)
		{
			const Item &i = items[at + n];

			if(i.lent || !isReserved(i))
				return false;

			if(n > 0 && (i.port != items[at + n - 1].port + 1))
				return false;
		}

		return true;
	}

	int findConsecutive(int count, int align) const
	{
		for(int n = 0; n < items.count(); n += align)
		{
			if(isConsecutive(n, count))
				return n;
		}

		return -1;
	}

	QList<QUdpSocket*> lendItem(Item *i, QObject *parent)
	{
		QList<QUdpSocket*> out;

		i->lent = true;
		foreach(QUdpSocket *sock, i->sockList)
		{
			i->lentAddrs += sock->localAddress();
			sock->disconnect(this);
			sock->setParent(parent);
			out += sock;
		}

		return out;
	}
};

UdpPortReserver::UdpPortReserver(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

UdpPortReserver::~UdpPortReserver()
{
	delete d;
}

void UdpPortReserver::setAddresses(const QList<QHostAddress> &addrs)
{
	d->updateAddresses(addrs);
}

void UdpPortReserver::setPorts(int start, int len)
{
	QList<int> ports;
	for(int n = 0; n < len; ++n)
		ports += start + n;
	setPorts(ports);
}

void UdpPortReserver::setPorts(const QList<int> &ports)
{
	d->updatePorts(ports);
}

bool UdpPortReserver::reservedAll() const
{
	return d->reservedAll();
}

QList<QUdpSocket*> UdpPortReserver::borrowSockets(int portCount, QObject *parent)
{
	return d->borrowSockets(portCount, parent);
}

void UdpPortReserver::returnSockets(const QList<QUdpSocket*> &sockList)
{
	d->returnSockets(sockList);
}

}

#include "udpportreserver.moc"
