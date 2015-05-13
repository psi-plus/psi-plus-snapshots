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


class HappyEyeballsConnector : public QObject
{
	Q_OBJECT
public:
	enum State {
		Failure,
		Created,
		Resolve,
		Connecting,
		Connected
	};

	struct SockData {
		QTcpSocket *sock;
		QTcpSocketSignalRelay *relay;
		State state;
		XMPP::ServiceResolver *resolver;
	};

	/*! source data */
	QString service;
	QString transport;
	QString domain;
	quint16 port;
	QHostAddress address;
	QAbstractSocket::NetworkLayerProtocol fallbackProtocol;

	/*! runtime data */
	QString lastError;
	int lastIndex;
	QList<SockData> sockets;
	QTimer fallbackTimer;

	HappyEyeballsConnector(QObject *parent) :
		QObject(parent),
		port(0)
	{
		fallbackProtocol = QAbstractSocket::IPv4Protocol;
		fallbackTimer.setSingleShot(true);
		fallbackTimer.setInterval(250); /* rfc recommends 150-250ms */
		connect(&fallbackTimer, SIGNAL(timeout()), SLOT(startFallback()));
	}

	SockData& addSocket()
	{
		SockData sd;
		sd.state = Created;
		sd.sock = new QTcpSocket(this);
		sd.sock->setReadBufferSize(READBUFSIZE);
		sd.relay = new QTcpSocketSignalRelay(sd.sock, this);
		sd.resolver = 0;
		connect(sd.relay, SIGNAL(connected()), SLOT(qs_connected()));
		connect(sd.relay, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(qs_error(QAbstractSocket::SocketError)));
		sockets.append(sd);
		return sockets[sockets.count() - 1];
	}

	void cleanup()
	{
		for (int i = 0; i < sockets.count(); i++) {
			abortSocket(sockets[i]);
		}
		fallbackTimer.stop();
	}


	void connectToHost(const QHostAddress &address, quint16 port)
	{
#ifdef BS_DEBUG
		BSDEBUG << "a:" << address << "p:" << port;
#endif
		this->address = address;
		SockData &sd = addSocket();
		sd.state = Connecting;
		sd.sock->connectToHost(address, port);
	}

	/* Connect to a host via the specified protocol, or the default protocols if not specified */
	void connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol)
	{
#ifdef BS_DEBUG
		BSDEBUG << "h:" << host << "p:" << port << "pr:" << protocol;
#endif
		this->domain = host;
		this->port = port;
		SockData &sd = addSocket();
		sd.resolver = new XMPP::ServiceResolver;
		initResolver(sd.resolver);
		sd.resolver->setProtocol(protocol == QAbstractSocket::UnknownNetworkLayerProtocol?
			(fallbackProtocol == QAbstractSocket::IPv4Protocol? XMPP::ServiceResolver::IPv6 : XMPP::ServiceResolver::IPv4) :
			(protocol== QAbstractSocket::IPv4Protocol? XMPP::ServiceResolver::IPv4 : XMPP::ServiceResolver::IPv6));
		if (protocol == QAbstractSocket::UnknownNetworkLayerProtocol) {
			addSocket();
			fallbackTimer.start();
		}
		sd.state = Resolve;
		sd.resolver->start(domain, port);
	}

	void connectToHost(const QString &service, const QString &transport, const QString &domain, quint16 port)
	{
#ifdef BS_DEBUG
		BSDEBUG << "s:" << service << "t:" << transport << "d:" << domain;
#endif
		this->service = service;
		this->transport = transport;
		this->domain = domain;
		this->port = port;
		SockData &sd = addSocket();
		sd.resolver = new XMPP::ServiceResolver(this);
		sd.resolver->setProtocol(XMPP::ServiceResolver::HappyEyeballs);
		connect(sd.resolver, SIGNAL(srvReady()), SLOT(splitSrvResolvers()));
		// we don't care about special handling of fail. we have fallback host there anyway
		connect(sd.resolver, SIGNAL(srvFailed()), SLOT(splitSrvResolvers()));
		sd.state = Resolve;
		sd.resolver->start(service, transport, domain, port);
	}

	SockData takeCurrent(QObject *parent)
	{
		SockData csd = sockets.takeAt(lastIndex);
		lastIndex = -1;
		disconnect(csd.relay);
		csd.relay->setParent(parent);
		csd.sock->setParent(parent);
		delete csd.resolver; // FIME ensure it's accessible only from connected signal. we don't delete resolver from its slot
		csd.resolver = 0;
		return csd;
	}

private:
	void abortSocket(SockData &sd)
	{
		sd.relay->disconnect(this);
		if (sd.state >= Connecting) {
			sd.sock->abort();
		}
		if (sd.resolver) {
			sd.resolver->stop();
			disconnect(sd.resolver);
			sd.resolver->deleteLater(); // or just delete ?
		}
		delete sd.relay;
		delete sd.sock;
	}

	void initResolver(XMPP::ServiceResolver *resolver)
	{
		resolver->setParent(this);
		connect(resolver, SIGNAL(resultReady(QHostAddress,quint16)), this, SLOT(handleDnsReady(QHostAddress,quint16)));
		connect(resolver, SIGNAL(error(XMPP::ServiceResolver::Error)), this, SLOT(handleDnsError(XMPP::ServiceResolver::Error)));
	}

	void setCurrentByResolver(XMPP::ServiceResolver *resolver)
	{
		for (int i = 0; i < sockets.count(); i++) {
			if (sockets.at(i).resolver == resolver) {
				lastIndex = i;
				return;
			}
		}
		lastIndex = -1;
	}

	void setCurrentByRelay(QTcpSocketSignalRelay *relay)
	{
		for (int i = 0; i < sockets.count(); i++) {
			if (sockets.at(i).relay == relay) {
				lastIndex = i;
				return;
			}
		}
		lastIndex = -1;
	}

private slots:
	/*
	Notice: most probably recipient should reparent socket and relay
	*/
	void qs_connected()
	{
#ifdef BS_DEBUG
		BSDEBUG;
#endif
		setCurrentByRelay(static_cast<QTcpSocketSignalRelay*>(sender()));
		for (int i = 0; i < sockets.count(); i++) {
			if (i != lastIndex) {
				abortSocket(sockets[i]);
			} else {
				disconnect(sockets[i].relay);
				sockets[i].state = Connected;
			}
			emit connected();
		}
	}

	void qs_error(QAbstractSocket::SocketError errorCode)
	{
		setCurrentByRelay(static_cast<QTcpSocketSignalRelay*>(sender()));
		// TODO remember error code
		lastError = sockets[lastIndex].sock->errorString();
#ifdef BS_DEBUG
		BSDEBUG << "error:" << lastError;
#endif
		if (sockets[lastIndex].resolver) {
			sockets[lastIndex].sock->abort();
			sockets[lastIndex].state = Resolve;
			sockets[lastIndex].resolver->tryNext();
		} else {
			// it seems we connect by hostaddress. just one socket w/o resolver
			emit error(errorCode);
		}
	}

	void splitSrvResolvers()
	{
#ifdef BS_DEBUG
		BSDEBUG << "splitting resolvers";
#endif
		setCurrentByResolver(static_cast<XMPP::ServiceResolver*>(sender()));
		SockData &sdv4 = sockets[lastIndex];
		SockData &sdv6 = addSocket();
		XMPP::ServiceResolver::ProtoSplit ps = sdv4.resolver->happySplit();
		initResolver(ps.ipv4);
		initResolver(ps.ipv6);

		disconnect(sdv4.resolver);
		sdv4.resolver->deleteLater();

		sdv4.resolver = ps.ipv4;
		sdv4.state = Created;
		sdv6.resolver = ps.ipv6;

		if (fallbackProtocol == QAbstractSocket::IPv4Protocol) {
			sdv6.state = Resolve;
			sdv6.resolver->tryNext();
		} else {
			sdv4.state = Resolve;
			sdv4.resolver->tryNext();
		}
		fallbackTimer.start();
	}

	/* host resolved, now try to connect to it */
	void handleDnsReady(const QHostAddress &address, quint16 port)
	{
#ifdef BS_DEBUG
		BSDEBUG << "a:" << address << "p:" << port;
#endif
		setCurrentByResolver(static_cast<XMPP::ServiceResolver*>(sender()));
		sockets[lastIndex].state = Connecting;
		sockets[lastIndex].sock->connectToHost(address, port);
	}

	/* resolver failed the dns lookup */
	void handleDnsError(XMPP::ServiceResolver::Error e) {
#ifdef BS_DEBUG
		BSDEBUG << "e:" << e;
#else
		Q_UNUSED(e)
#endif
		if (!fallbackTimer.isActive()) {
			emit error(QAbstractSocket::HostNotFoundError);
		}
	}

	void startFallback()
	{
#ifdef BS_DEBUG
		BSDEBUG;
#endif
		for(int i = 0; i < sockets.count(); i++) {
			SockData &sd = sockets[i];
			if (sd.state == Created) {
				sd.state = Resolve;
				if (sd.resolver) {
					sd.resolver->tryNext();
				} else {
					sd.resolver = new XMPP::ServiceResolver;
					initResolver(sd.resolver);
					sd.resolver->setProtocol(fallbackProtocol == QAbstractSocket::IPv4Protocol?
												 XMPP::ServiceResolver::IPv4 : XMPP::ServiceResolver::IPv6);
					sd.resolver->start(domain, port);
				}
			}
		}
	}

signals:
	void connected();
	void error(QAbstractSocket::SocketError);
};

class BSocket::Private
{
public:
	Private()
	{
		qsock = 0;
		qsock_relay = 0;
	}

	QTcpSocket *qsock;
	QTcpSocketSignalRelay *qsock_relay;
	int state;

	QString domain; //!< Domain we are currently connected to
	QString host; //!< Hostname we are currently connected to
	QHostAddress address; //!< IP address we are currently connected to
	quint16 port; //!< Port we are currently connected to

	//SafeDelete sd;

	QPointer<HappyEyeballsConnector> connector;
};

BSocket::BSocket(QObject *parent)
:ByteStream(parent)
{
	d = new Private;
	resetConnection();
}

BSocket::~BSocket()
{
	resetConnection(true);
	delete d;
}


void BSocket::resetConnection(bool clear)
{
#ifdef BS_DEBUG
	BSDEBUG << clear;
#endif
	if (d->connector) {
		delete d->connector; // fixme: deleteLater?
	}

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
	setOpenMode(QIODevice::NotOpen);
}

void BSocket::ensureConnector()
{
	if(!d->connector) {
		d->connector = new HappyEyeballsConnector(this);
		connect(d->connector, SIGNAL(connected()), SLOT(qs_connected()));
		connect(d->connector, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(qs_error(QAbstractSocket::SocketError)));
	}
}

/* Connect to an already resolved host */
void BSocket::connectToHost(const QHostAddress &address, quint16 port)
{
	resetConnection(true);
	d->address = address;
	d->port = port;
	d->state = Connecting;

	ensureConnector();
	d->connector->connectToHost(address, port);
}

/* Connect to a host via the specified protocol, or the default protocols if not specified */
void BSocket::connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol)
{
	resetConnection(true);
	d->host = host;
	d->port = port;
	d->state = Connecting;

	ensureConnector();
	d->connector->connectToHost(host, port, protocol);
}

/* Connect to the hosts for the specified service */
void BSocket::connectToHost(const QString &service, const QString &transport, const QString &domain, quint16 port)
{
	resetConnection(true);
	d->domain = domain;
	d->state = Connecting;

	ensureConnector();
	d->connector->connectToHost(service, transport, domain, port);
}

QAbstractSocket* BSocket::abstractSocket() const
{
	return d->qsock;
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
	resetConnection(true);
	d->qsock = new QTcpSocket(this);
	d->qsock->setSocketDescriptor(s);
	d->qsock_relay = new QTcpSocketSignalRelay(d->qsock, this);
	qs_connected_step2();
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
			resetConnection();
	}
	else {
		resetConnection();
	}
}

qint64 BSocket::writeData(const char *data, qint64 maxSize)
{
	if(d->state != Connected)
		return 0;
#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "- [" << maxSize << "]: {" << QByteArray::fromRawData(data, maxSize) << "}";
#endif
	return d->qsock->write(data, maxSize);
}

qint64 BSocket::readData(char *data, qint64 maxSize)
{
	if(!maxSize) {
		return 0;
	}
	quint64 readSize;
	if(d->qsock) {
		int max = bytesAvailable();
		if(maxSize <= 0 || maxSize > max) {
			maxSize = max;
		}
		readSize = d->qsock->read(data, maxSize);
	} else {
		readSize = ByteStream::readData(data, maxSize);
	}

#ifdef BS_DEBUG_EXTRA
	BSDEBUG << "- [" << readSize << "]: {" << QByteArray::fromRawData(data, readSize) << "}";
#endif
	return readSize;
}

qint64 BSocket::bytesAvailable() const
{
	if(d->qsock)
		return d->qsock->bytesAvailable();
	else
		return ByteStream::bytesAvailable();
}

qint64 BSocket::bytesToWrite() const
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

void BSocket::qs_connected()
{
	HappyEyeballsConnector::SockData sd = d->connector->takeCurrent(this);
	d->qsock = sd.sock;
	d->qsock_relay = sd.relay;
	d->connector->deleteLater();
	qs_connected_step2();
}

void BSocket::qs_connected_step2()
{
	connect(d->qsock_relay, SIGNAL(disconnected()), SLOT(qs_closed()));
	connect(d->qsock_relay, SIGNAL(readyRead()), SLOT(qs_readyRead()));
	connect(d->qsock_relay, SIGNAL(bytesWritten(qint64)), SLOT(qs_bytesWritten(qint64)));
	connect(d->qsock_relay, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(qs_error(QAbstractSocket::SocketError)));

	setOpenMode(QIODevice::ReadWrite);
	d->state = Connected;
#ifdef BS_DEBUG
	BSDEBUG << "Connected";
#endif
	//SafeDeleteLock s(&d->sd);
	emit connected();

	if (d->qsock->bytesAvailable()) {
		qs_readyRead();
	}
}

void BSocket::qs_closed()
{
	if(d->state == Closing)
	{
#ifdef BS_DEBUG
		BSDEBUG << "Delayed Close Finished";
#endif
		//SafeDeleteLock s(&d->sd);
		resetConnection();
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
	if(x == QTcpSocket::RemoteHostClosedError) {
#ifdef BS_DEBUG
		BSDEBUG << "Connection Closed";
#endif
		//SafeDeleteLock s(&d->sd);
		resetConnection();
		emit connectionClosed();
		return;
	}

#ifdef BS_DEBUG
	BSDEBUG << "Error";
#endif
	//SafeDeleteLock s(&d->sd);

	resetConnection();
	if(x == QTcpSocket::ConnectionRefusedError)
		emit error(ErrConnectionRefused);
	else if(x == QTcpSocket::HostNotFoundError)
		emit error(ErrHostNotFound);
	else
		emit error(ErrRead);
}

#include "bsocket.moc"

// CS_NAMESPACE_END
