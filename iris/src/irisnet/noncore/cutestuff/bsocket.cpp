/*
 * bsocket.cpp - QSocket wrapper based on Bytestream with SRV DNS support
 * Copyright (C) 2003  Justin Karneges
 * Copyright (C) 2009-2010  Dennis Schridde
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

#include <QTcpSocket>
#include <QHostAddress>
#include <QMetaType>

#include <limits>

#include "bsocket.h"

//#include "safedelete.h"

//#define BS_DEBUG

#ifdef BS_DEBUG
# define BSDEBUG (qDebug() << this << "#" << __FUNCTION__ << ":")
#endif


#define READBUFSIZE 65536

// CS_NAMESPACE_BEGIN

class QTcpSocketSignalRelay : public QObject
{
	Q_OBJECT
public:
	QTcpSocketSignalRelay(QTcpSocket *sock, QObject *parent = 0)
	:QObject(parent)
	{
		qRegisterMetaType<QAbstractSocket::SocketError>("QAbstractSocket::SocketError");
		connect(sock, SIGNAL(hostFound()), SLOT(sock_hostFound()), Qt::QueuedConnection);
		connect(sock, SIGNAL(connected()), SLOT(sock_connected()), Qt::QueuedConnection);
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()), Qt::QueuedConnection);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()), Qt::QueuedConnection);
		connect(sock, SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)), Qt::QueuedConnection);
		connect(sock, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(sock_error(QAbstractSocket::SocketError)), Qt::QueuedConnection);
	}

signals:
	void hostFound();
	void connected();
	void disconnected();
	void readyRead();
	void bytesWritten(qint64);
	void error(QAbstractSocket::SocketError);

public slots:
	void sock_hostFound()
	{
		emit hostFound();
	}

	void sock_connected()
	{
		emit connected();
	}

	void sock_disconnected()
	{
		emit disconnected();
	}

	void sock_readyRead()
	{
		emit readyRead();
	}

	void sock_bytesWritten(qint64 x)
	{
		emit bytesWritten(x);
	}

	void sock_error(QAbstractSocket::SocketError x)
	{
		emit error(x);
	}
};


class BSocket::Private
{
public:
	Private()
	{
		qsock = 0;
		qsock_relay = 0;
		resolver = 0;
	}

	QTcpSocket *qsock;
	QTcpSocketSignalRelay *qsock_relay;
	int state;

	QString domain; //!< Domain we are currently connected to
	QString host; //!< Hostname we are currently connected to
	QHostAddress address; //!< IP address we are currently connected to
	quint16 port; //!< Port we are currently connected to

	//SafeDelete sd;

	/*!
	 * Resolver used for lookups,
	 * will be destroyed and recreated after each lookup
	 */
	XMPP::ServiceResolver *resolver;
};

BSocket::BSocket(QObject *parent)
:ByteStream(parent)
{
	d = new Private;
	reset();
}

BSocket::~BSocket()
{
	reset(true);
	delete d;
}

/*
Recreate teh resolver,
a safety measure in case the resolver behaves strange when doing multiple lookups in a row
*/
void BSocket::recreate_resolver() {
	if (d->resolver) {
		disconnect(d->resolver);
		d->resolver->stop();
		d->resolver->deleteLater();
	}

	d->resolver = new XMPP::ServiceResolver;
}

void BSocket::reset(bool clear)
{
#ifdef BS_DEBUG
	BSDEBUG << clear;
#endif
	if(d->qsock) {
		delete d->qsock_relay;
		d->qsock_relay = 0;

		/*d->qsock->disconnect(this);

		if(!clear && d->qsock->isOpen() && d->qsock->isValid()) {*/
			// move remaining into the local queue
			QByteArray block(d->qsock->bytesAvailable(), 0);
			d->qsock->read(block.data(), block.size());
			appendRead(block);
		//}

		//d->sd.deleteLater(d->qsock);
		d->qsock->deleteLater();
		d->qsock = 0;
	}
	else {
		if(clear)
			clearReadBuffer();
	}

	d->state = Idle;
	d->domain = "";
	d->host = "";
	d->address = QHostAddress();
	d->port = 0;
}

void BSocket::ensureSocket()
{
	if(!d->qsock) {
		d->qsock = new QTcpSocket(this);
#if QT_VERSION >= 0x030200
		d->qsock->setReadBufferSize(READBUFSIZE);
#endif
		d->qsock_relay = new QTcpSocketSignalRelay(d->qsock, this);
		connect(d->qsock_relay, SIGNAL(hostFound()), SLOT(qs_hostFound()));
		connect(d->qsock_relay, SIGNAL(connected()), SLOT(qs_connected()));
		connect(d->qsock_relay, SIGNAL(disconnected()), SLOT(qs_closed()));
		connect(d->qsock_relay, SIGNAL(readyRead()), SLOT(qs_readyRead()));
		connect(d->qsock_relay, SIGNAL(bytesWritten(qint64)), SLOT(qs_bytesWritten(qint64)));
		connect(d->qsock_relay, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(qs_error(QAbstractSocket::SocketError)));
	}
}

/* Connect to an already resolved host */
void BSocket::connectToHost(const QHostAddress &address, quint16 port)
{
#ifdef BS_DEBUG
	BSDEBUG << "a:" << address << "p:" << port;
#endif

	reset(true);
	d->address = address;
	d->port = port;
	d->state = Connecting;

	ensureSocket();
	d->qsock->connectToHost(address, port);
}

/* Connect to a host via the specified protocol, or the default protocols if not specified */
void BSocket::connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol)
{
#ifdef BS_DEBUG
	BSDEBUG << "h:" << host << "p:" << port << "pr:" << protocol;
#endif

	reset(true);
	d->host = host;
	d->port = port;
	d->state = HostLookup;

	/* cleanup resolver for the new query */
	recreate_resolver();

	switch (protocol) {
		case QAbstractSocket::IPv6Protocol:
			d->resolver->setProtocol(XMPP::ServiceResolver::IPv6);
			break;
		case QAbstractSocket::IPv4Protocol:
			d->resolver->setProtocol(XMPP::ServiceResolver::IPv4);
			break;
		case QAbstractSocket::UnknownNetworkLayerProtocol:
			/* use ServiceResolver's default in this case */
			break;
	}

	connect(d->resolver, SIGNAL(resultReady(const QHostAddress&, quint16)), this, SLOT(handle_dns_ready(const QHostAddress&, quint16)));
	connect(d->resolver, SIGNAL(error(XMPP::ServiceResolver::Error)), this, SLOT(handle_dns_error(XMPP::ServiceResolver::Error)));
	d->resolver->start(host, port);
}

/* Connect to the hosts for the specified service */
void BSocket::connectToHost(const QString &service, const QString &transport, const QString &domain, quint16 port)
{
#ifdef BS_DEBUG
	BSDEBUG << "s:" << service << "t:" << transport << "d:" << domain;
#endif

	reset(true);
	d->domain = domain;
	d->state = HostLookup;

	/* cleanup resolver for the new query */
	recreate_resolver();

	connect(d->resolver, SIGNAL(resultReady(const QHostAddress&, quint16)), this, SLOT(handle_dns_ready(const QHostAddress&, quint16)));
	connect(d->resolver, SIGNAL(error(XMPP::ServiceResolver::Error)), this, SLOT(handle_dns_error(XMPP::ServiceResolver::Error)));
	d->resolver->start(service, transport, domain, port);
}

/* host resolved, now try to connect to it */
void BSocket::handle_dns_ready(const QHostAddress &address, quint16 port)
{
#ifdef BS_DEBUG
	BSDEBUG << "a:" << address << "p:" << port;
#endif

	connectToHost(address, port);
}

/* resolver failed the dns lookup */
void BSocket::handle_dns_error(XMPP::ServiceResolver::Error e) {
#ifdef BS_DEBUG
	BSDEBUG << "e:" << e;
#endif

	emit error(ErrHostNotFound);
}

/* failed to connect to host */
void BSocket::handle_connect_error(QAbstractSocket::SocketError e) {
#ifdef BS_DEBUG
	BSDEBUG << "d->r:" << d->resolver;
#endif

	/* try the next host for this service */
	Q_ASSERT(d->resolver);
	d->resolver->tryNext();
}

int BSocket::socket() const
{
	if(d->qsock)
		return d->qsock->socketDescriptor();
	else
		return -1;
}

void BSocket::setSocket(int s)
{
	reset(true);
	ensureSocket();
	d->state = Connected;
	d->qsock->setSocketDescriptor(s);
}

int BSocket::state() const
{
	return d->state;
}

bool BSocket::isOpen() const
{
	if(d->state == Connected)
		return true;
	else
		return false;
}

void BSocket::close()
{
	if(d->state == Idle)
		return;

	if(d->qsock) {
		d->qsock->close();
		d->state = Closing;
		if(d->qsock->bytesToWrite() == 0)
			reset();
	}
	else {
		reset();
	}
}

void BSocket::write(const QByteArray &a)
{
	if(d->state != Connected)
		return;
#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "- [" << a.size() << "]: {" << a << "}";
#endif
	d->qsock->write(a.data(), a.size());
}

QByteArray BSocket::read(int bytes)
{
	QByteArray block;
	if(d->qsock) {
		int max = bytesAvailable();
		if(bytes <= 0 || bytes > max)
			bytes = max;
		block.resize(bytes);
		d->qsock->read(block.data(), block.size());
	}
	else
		block = ByteStream::read(bytes);

#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "- [" << block.size() << "]: {" << block << "}";
#endif
	return block;
}

int BSocket::bytesAvailable() const
{
	if(d->qsock)
		return d->qsock->bytesAvailable();
	else
		return ByteStream::bytesAvailable();
}

int BSocket::bytesToWrite() const
{
	if(!d->qsock)
		return 0;
	return d->qsock->bytesToWrite();
}

QHostAddress BSocket::address() const
{
	if(d->qsock)
		return d->qsock->localAddress();
	else
		return QHostAddress();
}

quint16 BSocket::port() const
{
	if(d->qsock)
		return d->qsock->localPort();
	else
		return 0;
}

QHostAddress BSocket::peerAddress() const
{
	if(d->qsock)
		return d->qsock->peerAddress();
	else
		return QHostAddress();
}

quint16 BSocket::peerPort() const
{
	if(d->qsock)
		return d->qsock->peerPort();
	else
		return 0;
}

void BSocket::qs_hostFound()
{
	//SafeDeleteLock s(&d->sd);
}

void BSocket::qs_connected()
{
	d->state = Connected;
#ifdef BS_DEBUG
	BSDEBUG << "Connected";
#endif
	//SafeDeleteLock s(&d->sd);
	emit connected();
}

void BSocket::qs_closed()
{
	if(d->state == Closing)
	{
#ifdef BS_DEBUG
		BSDEBUG << "Delayed Close Finished";
#endif
		//SafeDeleteLock s(&d->sd);
		reset();
		emit delayedCloseFinished();
	}
}

void BSocket::qs_readyRead()
{
	//SafeDeleteLock s(&d->sd);
	emit readyRead();
}

void BSocket::qs_bytesWritten(qint64 x64)
{
	int x = x64;
#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "BytesWritten [" << x << "]";
#endif
	//SafeDeleteLock s(&d->sd);
	emit bytesWritten(x);
}

void BSocket::qs_error(QAbstractSocket::SocketError x)
{
	/* arriving here from connectToHost() */
	if (d->state == Connecting) {
		/* We do our own special error handling in this case */
		handle_connect_error(x);
		return;
	}

	if(x == QTcpSocket::RemoteHostClosedError) {
#ifdef BS_DEBUG
		BSDEBUG << "Connection Closed";
#endif
		//SafeDeleteLock s(&d->sd);
		reset();
		emit connectionClosed();
		return;
	}

#ifdef BS_DEBUG
	BSDEBUG << "Error";
#endif
	//SafeDeleteLock s(&d->sd);

	reset();
	if(x == QTcpSocket::ConnectionRefusedError)
		emit error(ErrConnectionRefused);
	else if(x == QTcpSocket::HostNotFoundError)
		emit error(ErrHostNotFound);
	else
		emit error(ErrRead);
}

#include "bsocket.moc"

// CS_NAMESPACE_END
