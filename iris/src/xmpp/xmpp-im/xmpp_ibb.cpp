/*
 * ibb.cpp - Inband bytestream
 * Copyright (C) 2001, 2002  Justin Karneges
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

#include "xmpp_ibb.h"

#include <qtimer.h>
#include "xmpp_xmlcommon.h"
#include <QtCrypto>

#include <stdlib.h>

#define IBB_PACKET_SIZE   4096
#define IBB_PACKET_DELAY  0

using namespace XMPP;

static int num_conn = 0;
static int id_conn = 0;
static const char *IBB_NS = "http://jabber.org/protocol/ibb";

//----------------------------------------------------------------------------
// IBBConnection
//----------------------------------------------------------------------------
class IBBConnection::Private
{
public:
	Private() {}

	int state;
	quint16 seq;
	Jid peer;
	QString sid;
	IBBManager *m;
	JT_IBB *j;
	QString iq_id;
	QString stanza;

	int blockSize;
	//QByteArray recvBuf, sendBuf;
	bool closePending, closing;

	int id; // connection id
};

IBBConnection::IBBConnection(IBBManager *m)
	: BSConnection(m)
{
	d = new Private;
	d->m = m;
	d->j = 0;
	d->blockSize = IBB_PACKET_SIZE;
	resetConnection();

	++num_conn;
	d->id = id_conn++;
#ifdef IBB_DEBUG
	qDebug("IBBConnection[%d]: constructing, count=%d", d->id, num_conn);
#endif
}

void IBBConnection::resetConnection(bool clear)
{
	d->m->unlink(this);
	d->state = Idle;
	d->closePending = false;
	d->closing = false;
	d->seq = 0;

	delete d->j;
	d->j = 0;

	clearWriteBuffer();
	if(clear)
		clearReadBuffer();
	setOpenMode(clear || !bytesAvailable()? QIODevice::NotOpen : QIODevice::ReadOnly);
}

IBBConnection::~IBBConnection()
{
	clearWriteBuffer(); // drop buffer to make closing procedure fast
	close();

	--num_conn;
#ifdef IBB_DEBUG
	qDebug("IBBConnection[%d]: destructing, count=%d", d->id, num_conn);
#endif

	delete d;
}

void IBBConnection::connectToJid(const Jid &peer, const QString &sid)
{
	close();
	resetConnection(true);

	d->state = Requesting;
	d->peer = peer;
	d->sid = sid;

#ifdef IBB_DEBUG
	qDebug("IBBConnection[%d]: initiating request to %s", d->id, qPrintable(peer.full()));
#endif

	d->j = new JT_IBB(d->m->client()->rootTask());
	connect(d->j, SIGNAL(finished()), SLOT(ibb_finished()));
	d->j->request(d->peer, d->sid);
	d->j->go(true);
}

void IBBConnection::accept()
{
	if(d->state != WaitingForAccept)
		return;

#ifdef IBB_DEBUG
	qDebug("IBBConnection[%d]: accepting %s [%s]", d->id,
		   qPrintable(d->peer.full()), qPrintable(d->sid));
#endif

	d->m->doAccept(this, d->iq_id);
	d->state = Active;
	setOpenMode(QIODevice::ReadWrite);
	d->m->link(this);

	emit connected(); // to be compatible with S5B
}

void IBBConnection::close()
{
	if(d->state == Idle)
		return;

	if(d->state == WaitingForAccept) {
		d->m->doReject(this, d->iq_id, Stanza::Error::Forbidden, "Rejected");
		resetConnection();
		return;
	}

#ifdef IBB_DEBUG
	qDebug("IBBConnection[%d]: closing", d->id);
#endif

	if(d->state == Active) {
		d->closePending = true;
		trySend();

		// if there is data pending to be written, then pend the closing
		if(bytesToWrite() > 0) {
			return;
		}
	}

	resetConnection();
}

int IBBConnection::state() const
{
	return d->state;
}

Jid IBBConnection::peer() const
{
	return d->peer;
}

QString IBBConnection::sid() const
{
	return d->sid;
}

BytestreamManager* IBBConnection::manager() const
{
	return d->m;
}

bool IBBConnection::isOpen() const
{
	if(d->state == Active)
		return true;
	else
		return false;
}

qint64 IBBConnection::writeData(const char *data, qint64 maxSize)
{
	if(d->state != Active || d->closePending || d->closing) {
		setErrorString("read only");
		return 0;
	}

	ByteStream::appendWrite(QByteArray::fromRawData(data, maxSize));
	trySend();
	return maxSize;
}

void IBBConnection::waitForAccept(const Jid &peer, const QString &iq_id,
								  const QString &sid, int blockSize,
								  const QString &stanza)
{
	close();
	resetConnection(true);

	d->state = WaitingForAccept;
	d->peer = peer;
	d->iq_id = iq_id;
	d->sid = sid;
	d->blockSize = blockSize;
	d->stanza = stanza;

}

void IBBConnection::takeIncomingData(const IBBData &ibbData)
{
	if (ibbData.seq != d->seq) {
		d->m->doReject(this, d->iq_id, Stanza::Error::UnexpectedRequest, "Invalid sequence");
		return;
	}
	if (ibbData.data.size() > d->blockSize) {
		d->m->doReject(this, d->iq_id, Stanza::Error::BadRequest, "Too much data");
		return;
	}
	d->seq++;
	appendRead(ibbData.data);

	emit readyRead();
}

void IBBConnection::setRemoteClosed()
{
	resetConnection();
	emit connectionClosed();
}

void IBBConnection::ibb_finished()
{
	JT_IBB *j = d->j;
	d->j = 0;

	if(j->success()) {
		if(j->mode() == JT_IBB::ModeRequest) {

#ifdef IBB_DEBUG
			qDebug("IBBConnection[%d]: %s [%s] accepted.", d->id,
				   qPrintable(d->peer.full()), qPrintable(d->sid));
#endif
			d->state = Active;
			setOpenMode(QIODevice::ReadWrite);
			d->m->link(this);
			emit connected();
		}
		else {
			if(d->closing) {
				resetConnection();
				emit delayedCloseFinished();
			}

			if(bytesToWrite() || d->closePending)
				QTimer::singleShot(IBB_PACKET_DELAY, this, SLOT(trySend()));

			emit bytesWritten(j->bytesWritten()); // will delete this connection if no bytes left.
		}
	}
	else {
		if(j->mode() == JT_IBB::ModeRequest) {
#ifdef IBB_DEBUG
			qDebug("IBBConnection[%d]: %s refused.", d->id, qPrintable(d->peer.full()));
#endif
			resetConnection(true);
			setError(ErrRequest);
		}
		else {
			resetConnection(true);
			setError(ErrData);
		}
	}
}

void IBBConnection::trySend()
{
	// if we already have an active task, then don't do anything
	if(d->j)
		return;

	QByteArray a = takeWrite(d->blockSize);

	if(a.isEmpty()) {
		if (!d->closePending)
			return; // null operation?
		d->closePending = false;
		d->closing = true;
#ifdef IBB_DEBUG
		qDebug("IBBConnection[%d]: closing", d->id);
#endif
	}
	else {
#ifdef IBB_DEBUG
		qDebug("IBBConnection[%d]: sending [%d] bytes (%d bytes left)",
			   d->id, a.size(), bytesToWrite());
#endif
	}

	d->j = new JT_IBB(d->m->client()->rootTask());
	connect(d->j, SIGNAL(finished()), SLOT(ibb_finished()));
	if (d->closing) {
		d->j->close(d->peer, d->sid);
	}
	else {
		d->j->sendData(d->peer, IBBData(d->sid, d->seq++, a));
	}
	d->j->go(true);
}



//----------------------------------------------------------------------------
// IBBData
//----------------------------------------------------------------------------
IBBData& IBBData::fromXml(const QDomElement &e)
{
	sid = e.attribute("sid"),
	seq = e.attribute("seq").toInt(),
	data = QCA::Base64().stringToArray(e.text()).toByteArray();
	return *this;
}

QDomElement IBBData::toXml(QDomDocument *doc) const
{
	QDomElement query = textTag(doc, "data",
						QCA::Base64().arrayToString(data)).toElement();
	query.setAttribute("xmlns", IBB_NS);
	query.setAttribute("seq", QString::number(seq));
	query.setAttribute("sid", sid);
	return query;
}

//----------------------------------------------------------------------------
// IBBManager
//----------------------------------------------------------------------------
class IBBManager::Private
{
public:
	Private() {}

	Client *client;
	IBBConnectionList activeConns;
	IBBConnectionList incomingConns;
	JT_IBB *ibb;
};

IBBManager::IBBManager(Client *parent)
	: BytestreamManager(parent)
{
	d = new Private;
	d->client = parent;

	d->ibb = new JT_IBB(d->client->rootTask(), true);
	connect(d->ibb,
			SIGNAL(incomingRequest(Jid,QString,QString,int,QString)),
			SLOT(ibb_incomingRequest(Jid,QString,QString,int,QString)));
	connect(d->ibb,
			SIGNAL(incomingData(Jid,QString,IBBData,Stanza::Kind)),
			SLOT(takeIncomingData(Jid,QString,IBBData,Stanza::Kind)));
	connect(d->ibb,
			SIGNAL(closeRequest(Jid,QString,QString)),
			SLOT(ibb_closeRequest(Jid,QString,QString)));
}

IBBManager::~IBBManager()
{
	qDeleteAll(d->incomingConns);
	d->incomingConns.clear();
	delete d->ibb;
	delete d;
}

const char* IBBManager::ns()
{
	return IBB_NS;
}

Client *IBBManager::client() const
{
	return d->client;
}

BSConnection *IBBManager::createConnection()
{
	return new IBBConnection(this);
}

IBBConnection *IBBManager::takeIncoming()
{
	return d->incomingConns.isEmpty()? 0 : d->incomingConns.takeFirst();
}

void IBBManager::ibb_incomingRequest(const Jid &from, const QString &id,
									 const QString &sid, int blockSize,
									 const QString &stanza)
{
	// create a "waiting" connection
	IBBConnection *c = new IBBConnection(this);
	c->waitForAccept(from, id, sid, blockSize, stanza);
	d->incomingConns.append(c);
	emit incomingReady();
}

void IBBManager::takeIncomingData(const Jid &from, const QString &id,
								  const IBBData &data, Stanza::Kind sKind)
{
	IBBConnection *c = findConnection(data.sid, from);
	if(!c) {
		if (sKind == Stanza::IQ) {
			d->ibb->respondError(from, id, Stanza::Error::ItemNotFound, "No such stream");
		}
		// TODO imeplement xep-0079 error processing in case of Stanza::Message
	}
	else {
		if (sKind == Stanza::IQ) {
			d->ibb->respondAck(from, id);
		}
		c->takeIncomingData(data);
	}
}

void IBBManager::ibb_closeRequest(const Jid &from, const QString &id,
								  const QString &sid)
{
	IBBConnection *c = findConnection(sid, from);
	if(!c) {
		d->ibb->respondError(from, id, Stanza::Error::ItemNotFound, "No such stream");
	}
	else {
		d->ibb->respondAck(from, id);
		c->setRemoteClosed();
	}
}

bool IBBManager::isAcceptableSID(const XMPP::Jid &jid, const QString&sid) const
{
	return findConnection(sid, jid) == NULL;
}

const char* IBBManager::sidPrefix() const
{
	return "ibb_";
}

void IBBManager::link(IBBConnection *c)
{
	d->activeConns.append(c);
}

void IBBManager::unlink(IBBConnection *c)
{
	d->activeConns.removeAll(c);
}

IBBConnection *IBBManager::findConnection(const QString &sid, const Jid &peer) const
{
	foreach(IBBConnection* c, d->activeConns) {
		if(c->sid() == sid && (peer.isEmpty() || c->peer().compare(peer)) )
			return c;
	}
	return 0;
}

void IBBManager::doAccept(IBBConnection *c, const QString &id)
{
	d->ibb->respondAck(c->peer(), id);
}

void IBBManager::doReject(IBBConnection *c, const QString &id,
						  Stanza::Error::ErrorCond cond, const QString &str)
{
	d->ibb->respondError(c->peer(), id, cond, str);
}


//----------------------------------------------------------------------------
// JT_IBB
//----------------------------------------------------------------------------
class JT_IBB::Private
{
public:
	Private() {}

	QDomElement iq;
	int mode;
	bool serve;
	Jid to;
	QString sid;
	int bytesWritten;
};

JT_IBB::JT_IBB(Task *parent, bool serve)
:Task(parent)
{
	d = new Private;
	d->serve = serve;
}

JT_IBB::~JT_IBB()
{
	delete d;
}

void JT_IBB::request(const Jid &to, const QString &sid)
{
	d->mode = ModeRequest;
	QDomElement iq;
	d->to = to;
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("open");
	//genUniqueKey
	query.setAttribute("xmlns", IBB_NS);
	query.setAttribute("sid", sid);
	query.setAttribute("block-size", IBB_PACKET_SIZE);
	query.setAttribute("stanza", "iq");
	iq.appendChild(query);
	d->iq = iq;
}

void JT_IBB::sendData(const Jid &to, const IBBData &ibbData)
{
	d->mode = ModeSendData;
	QDomElement iq;
	d->to = to;
	d->bytesWritten = ibbData.data.size();
	iq = createIQ(doc(), "set", to.full(), id());
	iq.appendChild(ibbData.toXml(doc()));
	d->iq = iq;
}

void JT_IBB::close(const Jid &to, const QString &sid)
{
	d->mode = ModeSendData;
	QDomElement iq;
	d->to = to;
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = iq.appendChild(doc()->createElement("close")).toElement();
	query.setAttribute("xmlns", IBB_NS);
	query.setAttribute("sid", sid);

	d->iq = iq;
}

void JT_IBB::respondError(const Jid &to, const QString &id,
						  Stanza::Error::ErrorCond cond, const QString &text)
{
	QDomElement iq = createIQ(doc(), "error", to.full(), id);
	Stanza::Error error(Stanza::Error::Cancel, cond, text);
	iq.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
	send(iq);
}

void JT_IBB::respondAck(const Jid &to, const QString &id)
{
	send( createIQ(doc(), "result", to.full(), id) );
}

void JT_IBB::onGo()
{
	send(d->iq);
}

bool JT_IBB::take(const QDomElement &e)
{
	if(d->serve) {
		// must be an iq-set tag
		if(e.tagName() != "iq" || e.attribute("type") != "set")
			return false;

		QString id = e.attribute("id");
		QString from = e.attribute("from");
		QDomElement openEl = e.firstChildElement("open");
		if (!openEl.isNull() && openEl.attribute("xmlns") == IBB_NS) {
			emit incomingRequest(Jid(from), id,
							openEl.attribute("sid"),
							openEl.attribute("block-size").toInt(),
							openEl.attribute("stanza"));
			return true;
		}
		QDomElement dataEl = e.firstChildElement("data");
		if (!dataEl.isNull() && dataEl.attribute("xmlns") == IBB_NS) {
			IBBData data;
			emit incomingData(Jid(from), id, data.fromXml(dataEl), Stanza::IQ);
			return true;
		}
		QDomElement closeEl = e.firstChildElement("close");
		if (!closeEl.isNull() && closeEl.attribute("xmlns") == IBB_NS) {
			emit closeRequest(Jid(from), id, closeEl.attribute("sid"));
			return true;
		}
		return false;
	}
	else {
		Jid from(e.attribute("from"));
		if(e.attribute("id") != id() || !d->to.compare(from))
			return false;

		if(e.attribute("type") == "result") {
			setSuccess();
		}
		else {
			setError(e);
		}

		return true;
	}
}

Jid JT_IBB::jid() const
{
	return d->to;
}

int JT_IBB::mode() const
{
	return d->mode;
}

int JT_IBB::bytesWritten() const
{
	return d->bytesWritten;
}

