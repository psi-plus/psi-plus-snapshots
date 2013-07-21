/*
 * httppoll.cpp - HTTP polling proxy
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

#include "httppoll.h"

#include <QUrl>
#include <qstringlist.h>
#include <qtimer.h>
#include <qpointer.h>
#include <QtCrypto>
#include <QByteArray>
#include <stdlib.h>
#include "bsocket.h"

#ifdef PROX_DEBUG
#include <stdio.h>
#endif

#define POLL_KEYS 64

// CS_NAMESPACE_BEGIN

static QByteArray randomArray(int size)
{
	QByteArray a;
  a.resize(size);
	for(int n = 0; n < size; ++n)
		a[n] = (char)(256.0*rand()/(RAND_MAX+1.0));
	return a;
}

//----------------------------------------------------------------------------
// HttpPoll
//----------------------------------------------------------------------------
static QString hpk(int n, const QString &s)
{
	if(n == 0)
		return s;
	else
		return QCA::Base64().arrayToString( QCA::Hash("sha1").hash( hpk(n - 1, s).toLatin1() ).toByteArray() );
}

class HttpPoll::Private
{
public:
	Private(HttpPoll *_q) :
		http(_q)
	{
	}

	HttpProxyPost http;
	QString host;
	int port;
	QString user, pass;
	QUrl url;
	bool use_proxy;

	QByteArray out;

	int state;
	bool closing;
	QString ident;

	QTimer *t;

	QString key[POLL_KEYS];
	int key_n;

	int polltime;
};

HttpPoll::HttpPoll(QObject *parent)
:ByteStream(parent)
{
	d = new Private(this);

	d->polltime = 30;
	d->t = new QTimer(this);
	d->t->setSingleShot(true);
	connect(d->t, SIGNAL(timeout()), SLOT(do_sync()));

	connect(&d->http, SIGNAL(result()), SLOT(http_result()));
	connect(&d->http, SIGNAL(error(int)), SLOT(http_error(int)));

	resetConnection(true);
}

HttpPoll::~HttpPoll()
{
	resetConnection(true);
	delete d->t;
	delete d;
}

QAbstractSocket* HttpPoll::abstractSocket() const
{
	return d->http.abstractSocket();
}

void HttpPoll::resetConnection(bool clear)
{
	if(d->http.isActive())
		d->http.stop();
	if(clear)
		clearReadBuffer();
	clearWriteBuffer();
	d->out.resize(0);
	d->state = 0;
	d->closing = false;
	d->t->stop();
}

void HttpPoll::setAuth(const QString &user, const QString &pass)
{
	d->user = user;
	d->pass = pass;
}

void HttpPoll::connectToUrl(const QUrl &url)
{
	connectToHost("", 0, url);
}

void HttpPoll::connectToHost(const QString &proxyHost, int proxyPort, const QUrl &url)
{
	resetConnection(true);

	bool useSsl = false;
	d->port = 80;
	// using proxy?
	if(!proxyHost.isEmpty()) {
		d->host = proxyHost;
		d->port = proxyPort;
		d->url = url;
		d->use_proxy = true;
	}
	else {
		d->host = url.host();
		if(url.port() != -1)
			d->port = url.port();
		else if (url.scheme() == "https") {
			d->port = 443;
			useSsl = true;
		}
#if QT_VERSION < 0x050000
		d->url = url.path() + "?" + url.encodedQuery();
#else
		d->url.setUrl(url.path() + "?" + url.query(QUrl::FullyEncoded), QUrl::StrictMode);
#endif
		d->use_proxy = false;
	}

	resetKey();
	bool last;
	QString key = getKey(&last);

#ifdef PROX_DEBUG
	fprintf(stderr, "HttpPoll: Connecting to %s:%d [%s]", d->host.latin1(), d->port, d->url.latin1());
	if(d->user.isEmpty())
		fprintf(stderr, "\n");
	else
		fprintf(stderr, ", auth {%s,%s}\n", d->user.latin1(), d->pass.latin1());
#endif
	QPointer<QObject> self = this;
	syncStarted();
	if(!self)
		return;

	d->state = 1;
	d->http.setUseSsl(useSsl);
	d->http.setAuth(d->user, d->pass);
	d->http.post(d->host, d->port, d->url, makePacket("0", key, "", QByteArray()), d->use_proxy);
}

QByteArray HttpPoll::makePacket(const QString &ident, const QString &key, const QString &newkey, const QByteArray &block)
{
	QString str = ident;
	if(!key.isEmpty()) {
		str += ';';
		str += key;
	}
	if(!newkey.isEmpty()) {
		str += ';';
		str += newkey;
	}
	str += ',';
	QByteArray cs = str.toLatin1();
	int len = cs.length();

	QByteArray a;
  a.resize(len + block.size());
	memcpy(a.data(), cs.data(), len);
	memcpy(a.data() + len, block.data(), block.size());
	return a;
}

int HttpPoll::pollInterval() const
{
	return d->polltime;
}

void HttpPoll::setPollInterval(int seconds)
{
	d->polltime = seconds;
}

bool HttpPoll::isOpen() const
{
	return (d->state == 2 ? true: false);
}

void HttpPoll::close()
{
	if(d->state == 0 || d->closing)
		return;

	if(bytesToWrite() == 0)
		resetConnection();
	else
		d->closing = true;
}

void HttpPoll::http_result()
{
	// check for death :)
	QPointer<QObject> self = this;
	syncFinished();
	if(!self)
		return;

	// get id and packet
	QString id;
	QString cookie = d->http.getHeader("Set-Cookie");
	int n = cookie.indexOf("ID=");
	if(n == -1) {
		resetConnection();
		setError(ErrRead);
		return;
	}
	n += 3;
	int n2 = cookie.indexOf(';', n);
	if(n2 != -1)
		id = cookie.mid(n, n2-n);
	else
		id = cookie.mid(n);
	QByteArray block = d->http.body();

	// session error?
	if(id.right(2) == ":0") {
		if(id == "0:0" && d->state == 2) {
			resetConnection();
			connectionClosed();
			return;
		}
		else {
			resetConnection();
			setError(ErrRead);
			return;
		}
	}

	d->ident = id;
	bool justNowConnected = false;
	if(d->state == 1) {
		d->state = 2;
		justNowConnected = true;
	}

	// sync up again soon
	if(bytesToWrite() > 0 || !d->closing) {
		d->t->start(d->polltime * 1000);
  }

	// connecting
	if(justNowConnected) {
		connected();
	}
	else {
		if(!d->out.isEmpty()) {
			int x = d->out.size();
			d->out.resize(0);
			takeWrite(x);
			bytesWritten(x);
		}
	}

	if(!self)
		return;

	if(!block.isEmpty()) {
		appendRead(block);
		readyRead();
	}

	if(!self)
		return;

	if(bytesToWrite() > 0) {
		do_sync();
	}
	else {
		if(d->closing) {
			resetConnection();
			delayedCloseFinished();
			return;
		}
	}
}

void HttpPoll::http_error(int x)
{
	resetConnection();
	if(x == HttpProxyPost::ErrConnectionRefused)
		setError(ErrConnectionRefused);
	else if(x == HttpProxyPost::ErrHostNotFound)
		setError(ErrHostNotFound);
	else if(x == HttpProxyPost::ErrSocket)
		setError(ErrRead);
	else if(x == HttpProxyPost::ErrProxyConnect)
		setError(ErrProxyConnect);
	else if(x == HttpProxyPost::ErrProxyNeg)
		setError(ErrProxyNeg);
	else if(x == HttpProxyPost::ErrProxyAuth)
		setError(ErrProxyAuth);
}

int HttpPoll::tryWrite()
{
	if(!d->http.isActive())
		do_sync();
	return 0;
}

void HttpPoll::do_sync()
{
	if(d->http.isActive())
		return;

	d->t->stop();
	d->out = takeWrite(0, false);

	bool last;
	QString key = getKey(&last);
	QString newkey;
	if(last) {
		resetKey();
		newkey = getKey(&last);
	}

	QPointer<QObject> self = this;
	syncStarted();
	if(!self)
		return;

	d->http.post(d->host, d->port, d->url, makePacket(d->ident, key, newkey, d->out), d->use_proxy);
}

void HttpPoll::resetKey()
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpPoll: reset key!\n");
#endif
	QByteArray a = randomArray(64);
	QString str = QString::fromLatin1(a.data(), a.size());

	d->key_n = POLL_KEYS;
	for(int n = 0; n < POLL_KEYS; ++n)
		d->key[n] = hpk(n+1, str);
}

const QString & HttpPoll::getKey(bool *last)
{
	*last = false;
	--(d->key_n);
	if(d->key_n == 0)
		*last = true;
	return d->key[d->key_n];
}


//----------------------------------------------------------------------------
// HttpProxyPost
//----------------------------------------------------------------------------
static QString extractLine(QByteArray *buf, bool *found)
{
	// scan for newline
	int n;
	for(n = 0; n < (int)buf->size()-1; ++n) {
		if(buf->at(n) == '\r' && buf->at(n+1) == '\n') {
			QByteArray cstr;
			cstr.resize(n);
			memcpy(cstr.data(), buf->data(), n);
			n += 2; // hack off CR/LF

			memmove(buf->data(), buf->data() + n, buf->size() - n);
			buf->resize(buf->size() - n);
			QString s = QString::fromUtf8(cstr);

			if(found)
				*found = true;
			return s;
		}
	}

	if(found)
		*found = false;
	return "";
}

static bool extractMainHeader(const QString &line, QString *proto, int *code, QString *msg)
{
	int n = line.indexOf(' ');
	if(n == -1)
		return false;
	if(proto)
		*proto = line.mid(0, n);
	++n;
	int n2 = line.indexOf(' ', n);
	if(n2 == -1)
		return false;
	if(code)
		*code = line.mid(n, n2-n).toInt();
	n = n2+1;
	if(msg)
		*msg = line.mid(n);
	return true;
}

class HttpProxyPost::Private
{
public:
	Private(HttpProxyPost *_q) :
		sock(_q),
		tls(0)
	{
	}

	~Private()
	{
		delete tls;
	}

	BSocket sock;
	QHostAddress lastAddress;
	QByteArray postdata, recvBuf, body;
	QUrl url;
	QString user, pass;
	bool inHeader;
	QStringList headerLines;
	bool asProxy;
	bool useSsl;
	QString host;
	QCA::TLS *tls;
};

HttpProxyPost::HttpProxyPost(QObject *parent)
:QObject(parent)
{
	d = new Private(this);
	connect(&d->sock, SIGNAL(connected()), SLOT(sock_connected()));
	connect(&d->sock, SIGNAL(connectionClosed()), SLOT(sock_connectionClosed()));
	connect(&d->sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
	connect(&d->sock, SIGNAL(error(int)), SLOT(sock_error(int)));
	resetConnection(true);
}

HttpProxyPost::~HttpProxyPost()
{
	resetConnection(true);
	delete d;
}

void HttpProxyPost::setUseSsl(bool state)
{
	d->useSsl = state;
}

QAbstractSocket* HttpProxyPost::abstractSocket() const
{
	return d->sock.abstractSocket();
}

void HttpProxyPost::resetConnection(bool clear)
{
	if(d->sock.state() != BSocket::Idle)
		d->sock.close();
	d->recvBuf.resize(0);
	if(clear)
		d->body.resize(0);
}

void HttpProxyPost::setAuth(const QString &user, const QString &pass)
{
	d->user = user;
	d->pass = pass;
}

bool HttpProxyPost::isActive() const
{
	return (d->sock.state() == BSocket::Idle ? false: true);
}

void HttpProxyPost::post(const QString &proxyHost, int proxyPort, const QUrl &url, const QByteArray &data, bool asProxy)
{
	resetConnection(true);

	d->host = proxyHost;
	d->url = url;
	d->postdata = data;
	d->asProxy = asProxy;

#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyPost: Connecting to %s:%d", proxyHost.latin1(), proxyPort);
	if(d->user.isEmpty())
		fprintf(stderr, "\n");
	else
		fprintf(stderr, ", auth {%s,%s}\n", d->user.latin1(), d->pass.latin1());
#endif
	if (d->sock.state() != QAbstractSocket::ConnectingState) { // in case of http/1.1 it may be connected
		if (d->lastAddress.isNull()) {
			d->sock.connectToHost(proxyHost, proxyPort);
		} else {
			d->sock.connectToHost(d->lastAddress, proxyPort);
		}
	}
}

void HttpProxyPost::stop()
{
	resetConnection();
}

QByteArray HttpProxyPost::body() const
{
	return d->body;
}

QString HttpProxyPost::getHeader(const QString &var) const
{
	foreach (const QString &s, d->headerLines) {
		int n = s.indexOf(": ");
		if(n == -1)
			continue;
		QString v = s.mid(0, n);
		if(v.toLower() == var.toLower())
			return s.mid(n+2);
	}
	return "";
}

void HttpProxyPost::sock_connected()
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyPost: Connected\n");
#endif
	if(d->useSsl) {
		d->tls = new QCA::TLS(this);
		connect(d->tls, SIGNAL(readyRead()), SLOT(tls_readyRead()));
		connect(d->tls, SIGNAL(readyReadOutgoing()), SLOT(tls_readyReadOutgoing()));
		connect(d->tls, SIGNAL(error()), SLOT(tls_error()));
		d->tls->startClient();
	}

	d->lastAddress = d->sock.peerAddress();
	d->inHeader = true;
	d->headerLines.clear();

	QUrl u = d->url;

	// connected, now send the request
	QByteArray s;
	s += QByteArray("POST ") + d->url.toEncoded() + " HTTP/1.1\r\n";
	if(d->asProxy) {
		if(!d->user.isEmpty()) {
			QByteArray str = d->user.toUtf8() + ':' + d->pass.toUtf8();
			s += QByteArray("Proxy-Authorization: Basic ") + str.toBase64() + "\r\n";
		}
		s += "Pragma: no-cache\r\n";
		s += QByteArray("Host: ") + u.host().toUtf8() + "\r\n";
	}
	else {
		s += QByteArray("Host: ") + d->host.toUtf8() + "\r\n";
	}
	s += "Content-Type: application/x-www-form-urlencoded\r\n";
	s += QByteArray("Content-Length: ") + QByteArray::number(d->postdata.size()) + "\r\n";
	s += "\r\n";

	if(d->useSsl) {
		// write request
		d->tls->write(s);

		// write postdata
		d->tls->write(d->postdata);
	} else {
		// write request
		d->sock.write(s);

		// write postdata
		d->sock.write(d->postdata);
	}
}

void HttpProxyPost::sock_connectionClosed()
{
	d->body = d->recvBuf;
	resetConnection();
	result();
}

void HttpProxyPost::tls_readyRead()
{
	//printf("tls_readyRead\n");
	processData(d->tls->read());
}

void HttpProxyPost::tls_readyReadOutgoing()
{
	//printf("tls_readyReadOutgoing\n");
	d->sock.write(d->tls->readOutgoing());
}

void HttpProxyPost::tls_error()
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyGetStream: ssl error: %d\n", d->tls->errorCode());
#endif
	resetConnection(true);
	error(ErrConnectionRefused); // FIXME: bogus error
}

void HttpProxyPost::sock_readyRead()
{
	QByteArray block = d->sock.readAll();
	if(d->useSsl)
		d->tls->writeIncoming(block);
	else
		processData(block);
}

void HttpProxyPost::processData(const QByteArray &block)
{
	d->recvBuf += block;

	if(d->inHeader) {
		// grab available lines
		while(1) {
			bool found;
			QString line = extractLine(&d->recvBuf, &found);
			if(!found)
				break;
			if(line.isEmpty()) {
				d->inHeader = false;
				break;
			}
			d->headerLines += line;
		}

		// done with grabbing the header?
		if(!d->inHeader) {
			QString str = d->headerLines.first();
			d->headerLines.takeFirst();

			QString proto;
			int code;
			QString msg;
			if(!extractMainHeader(str, &proto, &code, &msg)) {
#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyPost: invalid header!\n");
#endif
				resetConnection(true);
				error(ErrProxyNeg);
				return;
			}
			else {
#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyPost: header proto=[%s] code=[%d] msg=[%s]\n", proto.latin1(), code, msg.latin1());
				foreach (const QString &s, d->headerLines)
					fprintf(stderr, "HttpProxyPost: * [%s]\n", qPrintable(s));
#endif
			}

			if(code == 200) { // OK
#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyPost: << Success >>\n");
#endif
			}
			else {
				int err;
				QString errStr;
				if(code == 407) { // Authentication failed
					err = ErrProxyAuth;
					errStr = tr("Authentication failed");
				}
				else if(code == 404) { // Host not found
					err = ErrHostNotFound;
					errStr = tr("Host not found");
				}
				else if(code == 403) { // Access denied
					err = ErrProxyNeg;
					errStr = tr("Access denied");
				}
				else if(code == 503) { // Connection refused
					err = ErrConnectionRefused;
					errStr = tr("Connection refused");
				}
				else { // invalid reply
					err = ErrProxyNeg;
					errStr = tr("Invalid reply");
				}

#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyPost: << Error >> [%s]\n", errStr.latin1());
#endif
				resetConnection(true);
				error(err);
				return;
			}
		}
	}
}

void HttpProxyPost::sock_error(int x)
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyPost: socket error: %d\n", x);
#endif
	resetConnection(true);
	if(x == BSocket::ErrHostNotFound)
		error(ErrProxyConnect);
	else if(x == BSocket::ErrConnectionRefused)
		error(ErrProxyConnect);
	else if(x == BSocket::ErrRead)
		error(ErrProxyNeg);
}

//----------------------------------------------------------------------------
// HttpProxyGetStream
//----------------------------------------------------------------------------
class HttpProxyGetStream::Private
{
public:
	Private(HttpProxyGetStream *_q) :
		sock(_q)
	{
	}

	BSocket sock;
	QByteArray recvBuf;
	QString url;
	QString user, pass;
	bool inHeader;
	QStringList headerLines;
	bool use_ssl;
	bool asProxy;
	QString host;
	int length;

	QCA::TLS *tls;
};

HttpProxyGetStream::HttpProxyGetStream(QObject *parent)
:QObject(parent)
{
	d = new Private(this);
	d->tls = 0;
	connect(&d->sock, SIGNAL(connected()), SLOT(sock_connected()));
	connect(&d->sock, SIGNAL(connectionClosed()), SLOT(sock_connectionClosed()));
	connect(&d->sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
	connect(&d->sock, SIGNAL(error(int)), SLOT(sock_error(int)));
	resetConnection(true);
}

HttpProxyGetStream::~HttpProxyGetStream()
{
	resetConnection(true);
	delete d;
}

void HttpProxyGetStream::resetConnection(bool /*clear*/)
{
	if(d->tls) {
		delete d->tls;
		d->tls = 0;
	}
	if(d->sock.state() != BSocket::Idle)
		d->sock.close();
	d->recvBuf.resize(0);
	//if(clear)
	//	d->body.resize(0);
	d->length = -1;
}

void HttpProxyGetStream::setAuth(const QString &user, const QString &pass)
{
	d->user = user;
	d->pass = pass;
}

bool HttpProxyGetStream::isActive() const
{
	return (d->sock.state() == BSocket::Idle ? false: true);
}

void HttpProxyGetStream::get(const QString &proxyHost, int proxyPort, const QString &url, bool ssl, bool asProxy)
{
	resetConnection(true);

	d->host = proxyHost;
	d->url = url;
	d->use_ssl = ssl;
	d->asProxy = asProxy;

#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyGetStream: Connecting to %s:%d", proxyHost.latin1(), proxyPort);
	if(d->user.isEmpty())
		fprintf(stderr, "\n");
	else
		fprintf(stderr, ", auth {%s,%s}\n", d->user.latin1(), d->pass.latin1());
#endif
	d->sock.connectToHost(proxyHost, proxyPort);
}

void HttpProxyGetStream::stop()
{
	resetConnection();
}

QString HttpProxyGetStream::getHeader(const QString &var) const
{
	foreach (const QString &s, d->headerLines) {
		int n = s.indexOf(": ");
		if(n == -1)
			continue;
		QString v = s.mid(0, n);
		if(v.toLower() == var.toLower())
			return s.mid(n+2);
	}
	return "";
}

int HttpProxyGetStream::length() const
{
	return d->length;
}

void HttpProxyGetStream::sock_connected()
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyGetStream: Connected\n");
#endif
	if(d->use_ssl) {
		d->tls = new QCA::TLS(this);
		connect(d->tls, SIGNAL(readyRead()), SLOT(tls_readyRead()));
		connect(d->tls, SIGNAL(readyReadOutgoing()), SLOT(tls_readyReadOutgoing()));
		connect(d->tls, SIGNAL(error()), SLOT(tls_error()));
		d->tls->startClient();
	}

	d->inHeader = true;
	d->headerLines.clear();

	QUrl u = d->url;

	// connected, now send the request
	QString s;
	s += QString("GET ") + d->url + " HTTP/1.0\r\n";
	if(d->asProxy) {
		if(!d->user.isEmpty()) {
			QString str = d->user + ':' + d->pass;
			s += QString("Proxy-Authorization: Basic ") + QCA::Base64().encodeString(str) + "\r\n";
		}
		s += "Pragma: no-cache\r\n";
		s += QString("Host: ") + u.host() + "\r\n";
	}
	else {
		s += QString("Host: ") + d->host + "\r\n";
	}
	s += "\r\n";

	// write request
	if(d->use_ssl)
		d->tls->write(s.toUtf8());
	else
		d->sock.write(s.toUtf8());
}

void HttpProxyGetStream::sock_connectionClosed()
{
	//d->body = d->recvBuf;
	resetConnection();
	emit finished();
}

void HttpProxyGetStream::sock_readyRead()
{
	QByteArray block = d->sock.readAll();

	if(d->use_ssl)
		d->tls->writeIncoming(block);
	else
		processData(block);
}

void HttpProxyGetStream::processData(const QByteArray &block)
{
	printf("processData: %d bytes\n", block.size());
	if(!d->inHeader) {
		emit dataReady(block);
		return;
	}

	d->recvBuf += block;

	if(d->inHeader) {
		// grab available lines
		while(1) {
			bool found;
			QString line = extractLine(&d->recvBuf, &found);
			if(!found)
				break;
			if(line.isEmpty()) {
				printf("empty line\n");
				d->inHeader = false;
				break;
			}
			d->headerLines += line;
			printf("headerLine: [%s]\n", qPrintable(line));
		}

		// done with grabbing the header?
		if(!d->inHeader) {
			QString str = d->headerLines.first();
			d->headerLines.takeFirst();

			QString proto;
			int code;
			QString msg;
			if(!extractMainHeader(str, &proto, &code, &msg)) {
#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyGetStream: invalid header!\n");
#endif
				resetConnection(true);
				error(ErrProxyNeg);
				return;
			}
			else {
#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyGetStream: header proto=[%s] code=[%d] msg=[%s]\n", proto.latin1(), code, msg.latin1());
				foreach (const QString &s, d->headerLines)
					fprintf(stderr, "HttpProxyGetStream: * [%s]\n", qPrintable(s));
#endif
			}

			if(code == 200) { // OK
#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyGetStream: << Success >>\n");
#endif

				bool ok;
				int x = getHeader("Content-Length").toInt(&ok);
				if(ok)
					d->length = x;

				QPointer<QObject> self = this;
				emit handshaken();
				if(!self)
					return;
			}
			else {
				int err;
				QString errStr;
				if(code == 407) { // Authentication failed
					err = ErrProxyAuth;
					errStr = tr("Authentication failed");
				}
				else if(code == 404) { // Host not found
					err = ErrHostNotFound;
					errStr = tr("Host not found");
				}
				else if(code == 403) { // Access denied
					err = ErrProxyNeg;
					errStr = tr("Access denied");
				}
				else if(code == 503) { // Connection refused
					err = ErrConnectionRefused;
					errStr = tr("Connection refused");
				}
				else { // invalid reply
					err = ErrProxyNeg;
					errStr = tr("Invalid reply");
				}

#ifdef PROX_DEBUG
				fprintf(stderr, "HttpProxyGetStream: << Error >> [%s]\n", errStr.latin1());
#endif
				resetConnection(true);
				error(err);
				return;
			}

			if(!d->recvBuf.isEmpty()) {
				QByteArray a = d->recvBuf;
				d->recvBuf.clear();
				emit dataReady(a);
			}
		}
	}
}

void HttpProxyGetStream::sock_error(int x)
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyGetStream: socket error: %d\n", x);
#endif
	resetConnection(true);
	if(x == BSocket::ErrHostNotFound)
		error(ErrProxyConnect);
	else if(x == BSocket::ErrConnectionRefused)
		error(ErrProxyConnect);
	else if(x == BSocket::ErrRead)
		error(ErrProxyNeg);
}

void HttpProxyGetStream::tls_readyRead()
{
	//printf("tls_readyRead\n");
	processData(d->tls->read());
}

void HttpProxyGetStream::tls_readyReadOutgoing()
{
	//printf("tls_readyReadOutgoing\n");
	d->sock.write(d->tls->readOutgoing());
}

void HttpProxyGetStream::tls_error()
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpProxyGetStream: ssl error: %d\n", d->tls->errorCode());
#endif
	resetConnection(true);
	error(ErrConnectionRefused); // FIXME: bogus error
}

// CS_NAMESPACE_END
