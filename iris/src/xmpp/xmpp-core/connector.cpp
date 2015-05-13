/*
 * connector.cpp - establish a connection to an XMPP server
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

/*
  TODO:

  - Test and analyze all possible branches

  XMPP::AdvancedConnector is "good for now."  The only real issue is that
  most of what it provides is just to work around the old Jabber/XMPP 0.9
  connection behavior.  When XMPP 1.0 has taken over the world, we can
  greatly simplify this class.  - Sep 3rd, 2003.
*/

#include "xmpp.h"

#include <QPointer>
#include <QList>
#include <QUrl>
#include <QTimer>
#include <qca.h>

#include "bsocket.h"
#include "httpconnect.h"
#include "httppoll.h"
#include "socks.h"

//#define XMPP_DEBUG

#ifdef XMPP_DEBUG
# define XDEBUG (qDebug() << this << "#" << __FUNCTION__ << ":")
#endif

using namespace XMPP;

static const int XMPP_DEFAULT_PORT = 5222;
static const int XMPP_LEGACY_PORT = 5223;
static const char* XMPP_CLIENT_SRV = "xmpp-client";
static const char* XMPP_CLIENT_TRANSPORT = "tcp";


//----------------------------------------------------------------------------
// Connector
//----------------------------------------------------------------------------
Connector::Connector(QObject *parent)
:QObject(parent)
{
	setUseSSL(false);
	setPeerAddressNone();
}

Connector::~Connector()
{
}

bool Connector::useSSL() const
{
	return ssl;
}

bool Connector::havePeerAddress() const
{
	return haveaddr;
}

QHostAddress Connector::peerAddress() const
{
	return addr;
}

quint16 Connector::peerPort() const
{
	return port;
}

void Connector::setUseSSL(bool b)
{
	ssl = b;
}

void Connector::setPeerAddressNone()
{
	haveaddr = false;
	addr = QHostAddress();
	port = 0;
}

void Connector::setPeerAddress(const QHostAddress &_addr, quint16 _port)
{
	haveaddr = true;
	addr = _addr;
	port = _port;
}

QString Connector::host() const
{
	return QString();
}


//----------------------------------------------------------------------------
// AdvancedConnector::Proxy
//----------------------------------------------------------------------------
AdvancedConnector::Proxy::Proxy()
{
	t = None;
	v_poll = 30;
}

AdvancedConnector::Proxy::~Proxy()
{
}

int AdvancedConnector::Proxy::type() const
{
	return t;
}

QString AdvancedConnector::Proxy::host() const
{
	return v_host;
}

quint16 AdvancedConnector::Proxy::port() const
{
	return v_port;
}

QUrl AdvancedConnector::Proxy::url() const
{
	return v_url;
}

QString AdvancedConnector::Proxy::user() const
{
	return v_user;
}

QString AdvancedConnector::Proxy::pass() const
{
	return v_pass;
}

int AdvancedConnector::Proxy::pollInterval() const
{
	return v_poll;
}

void AdvancedConnector::Proxy::setHttpConnect(const QString &host, quint16 port)
{
	t = HttpConnect;
	v_host = host;
	v_port = port;
}

void AdvancedConnector::Proxy::setHttpPoll(const QString &host, quint16 port, const QUrl &url)
{
	t = HttpPoll;
	v_host = host;
	v_port = port;
	v_url = url;
}

void AdvancedConnector::Proxy::setSocks(const QString &host, quint16 port)
{
	t = Socks;
	v_host = host;
	v_port = port;
}

void AdvancedConnector::Proxy::setUserPass(const QString &user, const QString &pass)
{
	v_user = user;
	v_pass = pass;
}

void AdvancedConnector::Proxy::setPollInterval(int secs)
{
	v_poll = secs;
}


//----------------------------------------------------------------------------
// AdvancedConnector
//----------------------------------------------------------------------------
typedef enum { Idle, Connecting, Connected } Mode;
typedef enum { Force, Probe, Never } LegacySSL;

class AdvancedConnector::Private
{
public:
	ByteStream *bs; //!< Socket to use

	/* configuration values / "options" */
	QString opt_host; //!< explicit host from config
	quint16 opt_port; //!< explicit port from config
	LegacySSL opt_ssl; //!< Whether to use legacy SSL support
	Proxy proxy; //!< Proxy configuration

	/* State tracking values */
	Mode mode; //!< Idle, Connecting, Connected
	QString host; //!< Host we currently try to connect to, set from connectToServer()
	int port; //!< Port we currently try to connect to, set from connectToServer() and bs_error()
	int errorCode; //!< Current error, if any
};

AdvancedConnector::AdvancedConnector(QObject *parent)
:Connector(parent)
{
	d = new Private;
	d->bs = 0;
	d->opt_ssl = Never;
	cleanup();
	d->errorCode = 0;
}

AdvancedConnector::~AdvancedConnector()
{
	cleanup();
	delete d;
}

void AdvancedConnector::cleanup()
{
	d->mode = Idle;

	// destroy the bytestream, if there is one
	delete d->bs;
	d->bs = 0;

	setUseSSL(false);
	setPeerAddressNone();
}

void AdvancedConnector::setProxy(const Proxy &proxy)
{
	if(d->mode != Idle)
		return;
	d->proxy = proxy;
}

void AdvancedConnector::setOptHostPort(const QString &_host, quint16 _port)
{
#ifdef XMPP_DEBUG
	XDEBUG << "h:" << _host << "p:" << _port;
#endif

	if(d->mode != Idle)
		return;

	// empty host means disable explicit host support
	if(_host.isEmpty()) {
		d->opt_host.clear();
		return;
	}
	d->opt_host = _host;
	d->opt_port = _port;
}

void AdvancedConnector::setOptProbe(bool b)
{
#ifdef XMPP_DEBUG
	XDEBUG << "b:" << b;
#endif

	if(d->mode != Idle)
		return;
	d->opt_ssl = (b ? Probe : Never);
}

void AdvancedConnector::setOptSSL(bool b)
{
#ifdef XMPP_DEBUG
	XDEBUG << "b:" << b;
#endif

	if(d->mode != Idle)
		return;
	d->opt_ssl = (b ? Force : Never);
}

void AdvancedConnector::connectToServer(const QString &server)
{
#ifdef XMPP_DEBUG
	XDEBUG << "s:" << server;
#endif

	if(d->mode != Idle)
		return;
	if(server.isEmpty())
		return;

	d->errorCode = 0;
	d->mode = Connecting;

	// Encode the servername
	d->host = QUrl::toAce(server);
	if (d->host == QByteArray()) {
		/* server contains invalid characters for DNS name, but maybe valid characters for connecting, like "::1" */
		d->host = server;
	}
	d->port = XMPP_DEFAULT_PORT;

	if (d->opt_ssl == Probe && (d->proxy.type() != Proxy::None || !d->opt_host.isEmpty())) {
#ifdef XMPP_DEBUG
		XDEBUG << "Don't probe ssl port because of incompatible params";
#endif
		d->opt_ssl = Never; // probe is possible only with direct connect
	}

	if(d->proxy.type() == Proxy::HttpPoll) {
		HttpPoll *s = new HttpPoll;
		d->bs = s;

		connect(s, SIGNAL(connected()), SLOT(bs_connected()));
		connect(s, SIGNAL(syncStarted()), SLOT(http_syncStarted()));
		connect(s, SIGNAL(syncFinished()), SLOT(http_syncFinished()));
		connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

		if(!d->proxy.user().isEmpty())
			s->setAuth(d->proxy.user(), d->proxy.pass());
		s->setPollInterval(d->proxy.pollInterval());

		if(d->proxy.host().isEmpty())
			s->connectToUrl(d->proxy.url());
		else
			s->connectToHost(d->proxy.host(), d->proxy.port(), d->proxy.url());
	}
	else if (d->proxy.type() == Proxy::HttpConnect) {
		HttpConnect *s = new HttpConnect;
		d->bs = s;

		connect(s, SIGNAL(connected()), SLOT(bs_connected()));
		connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

		if(!d->opt_host.isEmpty()) {
			d->host = d->opt_host;
			d->port = d->opt_port;
		}

		if(!d->proxy.user().isEmpty())
			s->setAuth(d->proxy.user(), d->proxy.pass());

		s->connectToHost(d->proxy.host(), d->proxy.port(), d->host, d->port);
	}
	else if (d->proxy.type() == Proxy::Socks) {
		SocksClient *s = new SocksClient;
		d->bs = s;

		connect(s, SIGNAL(connected()), SLOT(bs_connected()));
		connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

		if(!d->opt_host.isEmpty()) {
			d->host = d->opt_host;
			d->port = d->opt_port;
		}

		if(!d->proxy.user().isEmpty())
			s->setAuth(d->proxy.user(), d->proxy.pass());

		s->connectToHost(d->proxy.host(), d->proxy.port(), d->host, d->port);
	}
	else {
		BSocket *s = new BSocket;
		d->bs = s;
#ifdef XMPP_DEBUG
		XDEBUG << "Adding socket:" << s;
#endif

		connect(s, SIGNAL(connected()), SLOT(bs_connected()));
		connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

		if(!d->opt_host.isEmpty()) { /* if custom host:port */
			d->host = d->opt_host;
			d->port = d->opt_port;
			s->connectToHost(d->host, d->port);
			return;
		} else if (d->opt_ssl != Never) { /* if ssl forced or should be probed */
			d->port = XMPP_LEGACY_PORT;
		}

		s->connectToHost(XMPP_CLIENT_SRV, XMPP_CLIENT_TRANSPORT, d->host, d->port);
	}
}

void AdvancedConnector::changePollInterval(int secs)
{
	if(d->bs && (d->bs->inherits("XMPP::HttpPoll") || d->bs->inherits("HttpPoll"))) {
		HttpPoll *s = static_cast<HttpPoll*>(d->bs);
		s->setPollInterval(secs);
	}
}

ByteStream *AdvancedConnector::stream() const
{
	if(d->mode == Connected)
		return d->bs;
	else
		return 0;
}

void AdvancedConnector::done()
{
	cleanup();
}

int AdvancedConnector::errorCode() const
{
	return d->errorCode;
}

void AdvancedConnector::bs_connected()
{
#ifdef XMPP_DEBUG
	XDEBUG;
#endif
	if(d->proxy.type() == Proxy::None) {
		QHostAddress h = (static_cast<BSocket*>(d->bs))->peerAddress();
		int p = (static_cast<BSocket*>(d->bs))->peerPort();
		setPeerAddress(h, p);
	}

	// We won't use ssl with HttpPoll since it has ow tls handler enabled for https.
	// The only variant for ssl is legacy port in probing or forced mde.
	if(d->proxy.type() != Proxy::HttpPoll  && (d->opt_ssl == Force || (
		d->opt_ssl == Probe && peerPort() == XMPP_LEGACY_PORT)))
	{
		// in case of Probe it's ok to check actual peer "port" since we are sure Proxy=None
		setUseSSL(true);
	}

	d->mode = Connected;
	emit connected();
}

void AdvancedConnector::bs_error(int x)
{
#ifdef XMPP_DEBUG
	XDEBUG << "e:" << x;
#endif

	if(d->mode == Connected) {
		d->errorCode = ErrStream;
		emit error();
		return;
	}

	bool proxyError = false;
	int err = ErrConnectionRefused;
	int t = d->proxy.type();

#ifdef XMPP_DEBUG
	qDebug("bse1");
#endif

	// figure out the error
	if(t == Proxy::None) {
		if(x == BSocket::ErrHostNotFound)
			err = ErrHostNotFound;
		else
			err = ErrConnectionRefused;
	}
	else if(t == Proxy::HttpConnect) {
		if(x == HttpConnect::ErrConnectionRefused)
			err = ErrConnectionRefused;
		else if(x == HttpConnect::ErrHostNotFound)
			err = ErrHostNotFound;
		else {
			proxyError = true;
			if(x == HttpConnect::ErrProxyAuth)
				err = ErrProxyAuth;
			else if(x == HttpConnect::ErrProxyNeg)
				err = ErrProxyNeg;
			else
				err = ErrProxyConnect;
		}
	}
	else if(t == Proxy::HttpPoll) {
		if(x == HttpPoll::ErrConnectionRefused)
			err = ErrConnectionRefused;
		else if(x == HttpPoll::ErrHostNotFound)
			err = ErrHostNotFound;
		else {
			proxyError = true;
			if(x == HttpPoll::ErrProxyAuth)
				err = ErrProxyAuth;
			else if(x == HttpPoll::ErrProxyNeg)
				err = ErrProxyNeg;
			else
				err = ErrProxyConnect;
		}
	}
	else if(t == Proxy::Socks) {
		if(x == SocksClient::ErrConnectionRefused)
			err = ErrConnectionRefused;
		else if(x == SocksClient::ErrHostNotFound)
			err = ErrHostNotFound;
		else {
			proxyError = true;
			if(x == SocksClient::ErrProxyAuth)
				err = ErrProxyAuth;
			else if(x == SocksClient::ErrProxyNeg)
				err = ErrProxyNeg;
			else
				err = ErrProxyConnect;
		}
	}

	// no-multi or proxy error means we quit
	if(proxyError) {
		cleanup();
		d->errorCode = err;
		emit error();
		return;
	}

	/*
		if we shall probe the ssl legacy port, and we just did that (port=legacy),
		then try to connect to the normal port instead
	*/
	if(d->opt_ssl == Probe && d->port == XMPP_LEGACY_PORT) {
#ifdef XMPP_DEBUG
		qDebug("bse1.2");
#endif
		BSocket *s = static_cast<BSocket*>(d->bs);
		d->port = XMPP_DEFAULT_PORT;
		// at this moment we already tried everything from srv. so just try the host itself
		s->connectToHost(d->host, d->port);
	}
	/* otherwise we have no fallbacks and must have failed to connect */
	else {
#ifdef XMPP_DEBUG
		qDebug("bse1.3");
#endif
		cleanup();
		d->errorCode = ErrConnectionRefused;
		emit error();
	}
}

void AdvancedConnector::http_syncStarted()
{
	httpSyncStarted();
}

void AdvancedConnector::http_syncFinished()
{
	httpSyncFinished();
}

void AdvancedConnector::t_timeout()
{
	//bs_error(-1);
}

QString AdvancedConnector::host() const
{
	return d->host;
}
