/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#include "addressresolver.h"

#include "objectsession.h"
#include "netnames.h"

namespace XMPP {

class AddressResolver::Private : public QObject
{
	Q_OBJECT

public:
	enum State
	{
		AddressWait,
		AddressFirstCome
	};

	AddressResolver *q;
	ObjectSession sess;
	State state;
	NameResolver req6;
	NameResolver req4;
	bool done6;
	bool done4;
	QList<QHostAddress> addrs6;
	QList<QHostAddress> addrs4;
	QTimer *opTimer;

	Private(AddressResolver *_q) :
		QObject(_q),
		q(_q),
		sess(this),
		req6(this),
		req4(this)
	{
		connect(&req6, SIGNAL(resultsReady(QList<XMPP::NameRecord>)), SLOT(req6_resultsReady(QList<XMPP::NameRecord>)));
		connect(&req6, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(req6_error(XMPP::NameResolver::Error)));

		connect(&req4, SIGNAL(resultsReady(QList<XMPP::NameRecord>)), SLOT(req4_resultsReady(QList<XMPP::NameRecord>)));
		connect(&req4, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(req4_error(XMPP::NameResolver::Error)));

		opTimer = new QTimer(this);
		connect(opTimer, SIGNAL(timeout()), SLOT(op_timeout()));
		opTimer->setSingleShot(true);
	}

	~Private()
	{
		opTimer->disconnect(this);
		opTimer->setParent(0);
		opTimer->deleteLater();
	}

	void start(const QByteArray &hostName)
	{
		state = AddressWait;

		// was an IP address used as input?
		QHostAddress addr;
		if(addr.setAddress(QString::fromLatin1(hostName)))
		{
			// use this as the result, no need to perform dns query
			done6 = true;
			done4 = true;
			if(addr.protocol() == QAbstractSocket::IPv6Protocol)
				addrs6 += addr;
			else
				addrs4 += addr;

			sess.defer(this, "ipAddress_input");
			return;
		}

		done6 = false;
		done4 = false;

		// wait at least 5 seconds for one of AAAA or A, to be
		//   consistent with netnames_jdns' dns-sd resolves
		opTimer->start(5000);

		req6.start(hostName, NameRecord::Aaaa);
		req4.start(hostName, NameRecord::A);
	}

	void stop()
	{
		cleanup();
	}

private:
	void cleanup()
	{
		sess.reset();

		req6.stop();
		req4.stop();
		opTimer->stop();

		addrs6.clear();
		addrs4.clear();
	}

	bool tryDone()
	{
		if((done6 && done4) || (state == AddressFirstCome && (done6 || done4)))
		{
			QList<QHostAddress> results = addrs6 + addrs4;
			cleanup();

			if(!results.isEmpty())
				emit q->resultsReady(results);
			else
				emit q->error(ErrorGeneric);

			return true;
		}

		return false;
	}

private slots:
	void req6_resultsReady(const QList<XMPP::NameRecord> &results)
	{
		foreach(const NameRecord &rec, results)
			addrs6 += rec.address();

		done6 = true;
		tryDone();
	}

	void req6_error(XMPP::NameResolver::Error e)
	{
		Q_UNUSED(e);

		done6 = true;
		tryDone();
	}

	void req4_resultsReady(const QList<XMPP::NameRecord> &results)
	{
		foreach(const NameRecord &rec, results)
			addrs4 += rec.address();

		done4 = true;
		tryDone();
	}

	void req4_error(XMPP::NameResolver::Error e)
	{
		Q_UNUSED(e);

		done4 = true;
		tryDone();
	}

	void op_timeout()
	{
		state = AddressFirstCome;

		if(done6 || done4)
			tryDone();
	}

	void ipAddress_input()
	{
		tryDone();
	}
};

AddressResolver::AddressResolver(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

AddressResolver::~AddressResolver()
{
	delete d;
}

void AddressResolver::start(const QByteArray &hostName)
{
	d->start(hostName);
}

void AddressResolver::stop()
{
	d->stop();
}

}

#include "addressresolver.moc"
