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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
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

#include "bsocket.h"
#include "httpconnect.h"
#include "httppoll.h"
#include "netnames.h"
#include "remotenameprovider.h"
#include "socks.h"
#include "xmpp.h"

#include <QList>
#include <QNetworkProxy>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <qca.h>

// #define XMPP_DEBUG
#ifdef XMPP_DEBUG
#define XDEBUG (qDebug() << this << "#" << __FUNCTION__ << ":")
#endif

using namespace XMPP;

static const int   XMPP_DEFAULT_PORT     = 5222;
static const int   XMPP_LEGACY_PORT      = 5223;
static const char *XMPP_CLIENT_SRV       = "xmpp-client";
static const char *XMPP_CLIENT_TLS_SRV   = "xmpps-client";
static const char *XMPP_CLIENT_TRANSPORT = "tcp";

//----------------------------------------------------------------------------
// Connector
//----------------------------------------------------------------------------
Connector::Connector(QObject *parent) : QObject(parent)
{
    setUseSSL(false);
    setPeerAddressNone();
}

Connector::~Connector() { }

bool Connector::useSSL() const { return ssl; }

bool Connector::havePeerAddress() const { return haveaddr; }

QHostAddress Connector::peerAddress() const { return addr; }

quint16 Connector::peerPort() const { return port; }

void Connector::setUseSSL(bool b) { ssl = b; }

void Connector::setPeerAddressNone()
{
    haveaddr = false;
    addr     = QHostAddress();
    port     = 0;
}

void Connector::setPeerAddress(const QHostAddress &_addr, quint16 _port)
{
    haveaddr = true;
    addr     = _addr;
    port     = _port;
}

QString Connector::host() const { return QString(); }

//----------------------------------------------------------------------------
// AdvancedConnector::Proxy
//----------------------------------------------------------------------------

int AdvancedConnector::Proxy::type() const { return t; }

QString AdvancedConnector::Proxy::host() const { return v_host; }

quint16 AdvancedConnector::Proxy::port() const { return v_port; }

QUrl AdvancedConnector::Proxy::url() const { return v_url; }

QString AdvancedConnector::Proxy::user() const { return v_user; }

QString AdvancedConnector::Proxy::pass() const { return v_pass; }

int AdvancedConnector::Proxy::pollInterval() const { return v_poll; }

void AdvancedConnector::Proxy::setHttpConnect(const QString &host, quint16 port)
{
    t      = HttpConnect;
    v_host = host;
    v_port = port;
}

void AdvancedConnector::Proxy::setHttpPoll(const QString &host, quint16 port, const QUrl &url)
{
    t      = HttpPoll;
    v_host = host;
    v_port = port;
    v_url  = url;
}

void AdvancedConnector::Proxy::setSocks(const QString &host, quint16 port)
{
    t      = Socks;
    v_host = host;
    v_port = port;
}

void AdvancedConnector::Proxy::setUserPass(const QString &user, const QString &pass)
{
    v_user = user;
    v_pass = pass;
}

void AdvancedConnector::Proxy::setPollInterval(int secs) { v_poll = secs; }

AdvancedConnector::Proxy::operator QNetworkProxy()
{
    return QNetworkProxy(t == Socks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy, v_host, v_port, v_user,
                         v_pass);
}

//----------------------------------------------------------------------------
// AdvancedConnector
//----------------------------------------------------------------------------
typedef enum { Idle, Connecting, Connected } Mode;
typedef enum : char { Force, Never } DirectTLS;

class AdvancedConnector::Private {
public:
    ByteStream *bs = nullptr; //!< Socket to use

    /* configuration values / "options" */
    QString opt_host;              //!< explicit host from config
    quint16 opt_port;              //!< explicit port from config
    bool    opt_directtls = false; //!< Whether to use direct TLS support
    bool    opt_srvtls    = true;  //!< Whether to lookup tls port from SRV
    Proxy   proxy;                 //!< Proxy configuration
    QUrl    remoteDns;             //!< DNS server queried through the proxy, see setRemoteDns()

    /* Server discovery through the proxy, see resolveThroughProxy() */
    RemoteNameProvider *dnsProvider = nullptr;
    ServiceResolver    *resolver    = nullptr;
    QString             service;              //!< SRV service of the address being tried
    QString             targetHost;           //!< SRV target (or the domain) of that address
    int                 lastTargetError = -1; //!< Proxy error of the last address that failed

    /* State tracking values */
    Mode    mode;          //!< Idle, Connecting, Connected
    QString host;          //!< Host we currently try to connect to, set from connectToServer()
    int     port;          //!< Port we currently try to connect to, set from connectToServer() and bs_error()
    int     errorCode = 0; //!< Current error, if any
};

AdvancedConnector::AdvancedConnector(QObject *parent) : Connector(parent), d(new Private)
{
    cleanup();
    d->errorCode = 0;
}

AdvancedConnector::~AdvancedConnector() { cleanup(); }

void AdvancedConnector::cleanup()
{
    d->mode = Idle;
    releaseResolver();

    // destroy the bytestream, if there is one
    delete d->bs;
    d->bs = nullptr;

    setUseSSL(false);
    setPeerAddressNone();
}

void AdvancedConnector::setProxy(const Proxy &proxy)
{
    if (d->mode != Idle)
        return;
    d->proxy = proxy;
}

void AdvancedConnector::setRemoteDns(const QUrl &server)
{
    if (d->mode != Idle)
        return;
    d->remoteDns = server;
}

void AdvancedConnector::releaseResolver()
{
    // Called from resolver and stream signals: defer the deletion. Stopping
    // the resolver first ensures no lookup outlives its name provider.
    if (d->resolver) {
        d->resolver->stop();
        d->resolver->disconnect(this);
        d->resolver->deleteLater();
        d->resolver = nullptr;
    }
    if (d->dnsProvider) {
        d->dnsProvider->deleteLater(); // closes the connection to the DNS server
        d->dnsProvider = nullptr;
    }
    d->service.clear();
    d->targetHost.clear();
    d->lastTargetError = -1;
}

/*
   Discover the server like a direct connection does - SRV records, their
   targets, finally the domain itself - and connect through the proxy to every
   candidate in turn. By default only the SRV records are looked up here, and
   the proxy resolves the targets. With remote DNS a DNS server reached through
   the proxy looks up the SRV records and the target addresses; should that
   find nothing, the proxy is given the domain. Other proxy users of netnames
   get the same behaviour from ServiceResolver::setNameProvider() and
   ServiceResolver::setResolveTargets().
*/
void AdvancedConnector::resolveThroughProxy()
{
    QStringList services;
    if (!d->opt_directtls && d->opt_srvtls)
        services << XMPP_CLIENT_TLS_SRV;
    services << XMPP_CLIENT_SRV;
    if (d->opt_directtls)
        d->port = XMPP_LEGACY_PORT;

    d->resolver = new ServiceResolver(this);
    if (!d->remoteDns.isEmpty() && RemoteNameProvider::isSupported()) {
        d->dnsProvider = new RemoteNameProvider(d->remoteDns, d->proxy, this);
        d->resolver->setNameProvider(d->dnsProvider);
        // The proxy makes the connections, so the local network's IPv6 support is
        // irrelevant. IPv4 is what proxies reach most reliably; IPv6 comes next.
        d->resolver->setProtocol(ServiceResolver::IPv4_IPv6);
    } else {
        d->resolver->setResolveTargets(false);
    }

    connect(d->resolver, &ServiceResolver::resultReady, this,
            [this](const QHostAddress &address, quint16 port, const QString &hostname, const QString &service) {
                d->service    = service;
                d->targetHost = hostname;
                connectThroughProxy(address.isNull() ? hostname : address.toString(), port);
            });
    connect(d->resolver, &ServiceResolver::error, this, [this](ServiceResolver::Error) {
        if (d->dnsProvider) {
            const int dnsError = d->dnsProvider->connectionError();
            if (dnsError != SocksClient::ErrProxyConnect && dnsError != SocksClient::ErrProxyAuth) {
                // Nothing the DNS server found was reachable: as a last resort
                // the proxy resolves the domain, as it does without SRV.
                releaseResolver();
                connectThroughProxy(d->host, quint16(d->port));
                return;
            }
        }
        // Report why the last address failed through the proxy. Without any,
        // the DNS server was not reachable, possibly because of the proxy.
        int err = ErrHostNotFound;
        if (d->lastTargetError >= 0) {
            err = d->lastTargetError;
        } else {
            switch (d->dnsProvider ? d->dnsProvider->connectionError() : ByteStream::ErrOk) {
            case SocksClient::ErrProxyConnect:
                err = ErrProxyConnect;
                break;
            case SocksClient::ErrProxyNeg:
                err = ErrProxyNeg;
                break;
            case SocksClient::ErrProxyAuth:
                err = ErrProxyAuth;
                break;
            default:
                break;
            }
        }
        cleanup();
        d->errorCode = err;
        emit error();
    });
    d->resolver->start(services, XMPP_CLIENT_TRANSPORT, d->host, quint16(d->port));
}

void AdvancedConnector::connectThroughProxy(const QString &host, quint16 port)
{
    if (d->proxy.type() == Proxy::HttpConnect) {
        HttpConnect *s = new HttpConnect;
        d->bs          = s;

        connect(s, SIGNAL(connected()), SLOT(bs_connected()));
        connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

        if (!d->proxy.user().isEmpty())
            s->setAuth(d->proxy.user(), d->proxy.pass());

        s->connectToHost(d->proxy.host(), d->proxy.port(), host, port);
    } else {
        SocksClient *s = new SocksClient;
        d->bs          = s;

        connect(s, SIGNAL(connected()), SLOT(bs_connected()));
        connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

        if (!d->proxy.user().isEmpty())
            s->setAuth(d->proxy.user(), d->proxy.pass());

        s->connectToHost(d->proxy.host(), d->proxy.port(), host, port);
    }
}

void AdvancedConnector::setOptHostPort(const QString &_host, quint16 _port)
{
#ifdef XMPP_DEBUG
    XDEBUG << "h:" << _host << "p:" << _port;
#endif

    if (d->mode != Idle)
        return;

    // empty host means disable explicit host support
    if (_host.isEmpty()) {
        d->opt_host.clear();
        return;
    }
    d->opt_host = _host;
    d->opt_port = _port;
}

void AdvancedConnector::setOptSSL(bool b)
{
#ifdef XMPP_DEBUG
    XDEBUG << "b:" << b;
#endif

    if (d->mode != Idle)
        return;
    d->opt_directtls = b;
}

void AdvancedConnector::setOptTlsSrv(bool value)
{
#ifdef XMPP_DEBUG
    XDEBUG << "b:" << b;
#endif
    if (d->mode != Idle)
        return;
    d->opt_srvtls = value;
}

void AdvancedConnector::connectToServer(const QString &server)
{
#ifdef XMPP_DEBUG
    XDEBUG << "s:" << server;
#endif

    if (d->mode != Idle)
        return;
    if (server.isEmpty())
        return;

    d->errorCode = 0;
    d->mode      = Connecting;

    // Encode the servername
    d->host = QUrl::toAce(server);
    if (d->host == QByteArray()) {
        /* server contains invalid characters for DNS name, but maybe valid characters for connecting, like "::1" */
        d->host = server;
    }
    d->port = XMPP_DEFAULT_PORT;

    if (d->proxy.type() == Proxy::HttpPoll) {
        HttpPoll *s = new HttpPoll;
        d->bs       = s;

        connect(s, SIGNAL(connected()), SLOT(bs_connected()));
        connect(s, SIGNAL(syncStarted()), SLOT(http_syncStarted()));
        connect(s, SIGNAL(syncFinished()), SLOT(http_syncFinished()));
        connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

        if (!d->proxy.user().isEmpty())
            s->setAuth(d->proxy.user(), d->proxy.pass());
        s->setPollInterval(d->proxy.pollInterval());

        if (d->proxy.host().isEmpty())
            s->connectToUrl(d->proxy.url());
        else
            s->connectToHost(d->proxy.host(), d->proxy.port(), d->proxy.url());
    } else if (d->proxy.type() == Proxy::HttpConnect || d->proxy.type() == Proxy::Socks) {
        if (!d->opt_host.isEmpty()) {
            // an explicit host is handed to the proxy, which resolves it
            d->host = d->opt_host;
            d->port = d->opt_port;
        } else if (QHostAddress(d->host).isNull()) {
            resolveThroughProxy();
            return;
        }
        // an explicit host or an address: no SRV lookup
        connectThroughProxy(d->host, quint16(d->port));
    } else {
        BSocket *s = new BSocket;
        d->bs      = s;
#ifdef XMPP_DEBUG
        XDEBUG << "Adding socket:" << s;
#endif

        connect(s, &BSocket::connected, this, [this, s]() {
            if (!useSSL()) {
                setUseSSL(s->service() == QLatin1String(XMPP_CLIENT_TLS_SRV));
            }
            bs_connected();
        });
        connect(s, SIGNAL(error(int)), SLOT(bs_error(int)));

        if (!d->opt_host.isEmpty()) { /* if custom host:port */
            d->host = d->opt_host;
            d->port = d->opt_port;
            s->connectToHost(d->host, quint16(d->port));
            return;
        }

        QStringList services;
        if (!d->opt_directtls && d->opt_srvtls) {
            services << XMPP_CLIENT_TLS_SRV;
        }
        services << XMPP_CLIENT_SRV;
        if (d->opt_directtls) {
            d->port = XMPP_LEGACY_PORT;
        }
        s->connectToHost(services, XMPP_CLIENT_TRANSPORT, d->host, quint16(d->port));
    }
}

void AdvancedConnector::changePollInterval(int secs)
{
    if (d->bs && (d->bs->inherits("XMPP::HttpPoll") || d->bs->inherits("HttpPoll"))) {
        HttpPoll *s = static_cast<HttpPoll *>(d->bs);
        s->setPollInterval(secs);
    }
}

ByteStream *AdvancedConnector::stream() const
{
    if (d->mode == Connected)
        return d->bs;
    else
        return nullptr;
}

void AdvancedConnector::done() { cleanup(); }

int AdvancedConnector::errorCode() const { return d->errorCode; }

void AdvancedConnector::bs_connected()
{
#ifdef XMPP_DEBUG
    XDEBUG;
#endif
    if (d->proxy.type() == Proxy::None) {
        QHostAddress h = (static_cast<BSocket *>(d->bs))->peerAddress();
        quint16      p = (static_cast<BSocket *>(d->bs))->peerPort();
        setPeerAddress(h, p);
    }

    // We won't use ssl with HttpPoll since it has own tls handler enabled for https.
    // The only variant for ssl is legacy port in probing or forced mde.
    if (d->proxy.type() != Proxy::HttpPoll && (d->opt_directtls || peerPort() == XMPP_LEGACY_PORT)) {
        setUseSSL(true);
    }

    if (auto bs = qobject_cast<BSocket *>(d->bs); bs && !bs->host().isEmpty()) {
        d->host = bs->host();
    }
    if (d->resolver) {
        // connected to an address found through the proxy
        if (!useSSL())
            setUseSSL(d->service == QLatin1String(XMPP_CLIENT_TLS_SRV));
        if (!d->targetHost.isEmpty())
            d->host = d->targetHost;
        releaseResolver();
    }
    d->mode = Connected;
    emit connected();
}

void AdvancedConnector::bs_error(int x)
{
#ifdef XMPP_DEBUG
    XDEBUG << "e:" << x;
#endif

    if (d->mode == Connected) {
        d->errorCode = ErrStream;
        emit error();
        return;
    }

    bool proxyError = false;
    int  err        = ErrConnectionRefused;
    int  t          = d->proxy.type();

    // An address found through the proxy's DNS failed: unless the proxy itself
    // is unreachable or rejects us, try the next address.
    if (d->resolver && x != SocksClient::ErrProxyConnect && x != SocksClient::ErrProxyAuth) {
        if (x == SocksClient::ErrConnectionRefused)
            d->lastTargetError = ErrConnectionRefused;
        else if (x == SocksClient::ErrHostNotFound)
            d->lastTargetError = ErrHostNotFound;
        else
            d->lastTargetError = ErrProxyNeg;
        d->bs->disconnect(this);
        d->bs->deleteLater(); // we are in its error signal
        d->bs = nullptr;
        d->resolver->tryNext();
        return;
    }

#ifdef XMPP_DEBUG
    qDebug("bse1");
#endif

    // figure out the error
    if (t == Proxy::None) {
        if (x == BSocket::ErrHostNotFound)
            err = ErrHostNotFound;
        else
            err = ErrConnectionRefused;
    } else if (t == Proxy::HttpConnect) {
        if (x == HttpConnect::ErrConnectionRefused)
            err = ErrConnectionRefused;
        else if (x == HttpConnect::ErrHostNotFound)
            err = ErrHostNotFound;
        else {
            proxyError = true;
            if (x == HttpConnect::ErrProxyAuth)
                err = ErrProxyAuth;
            else if (x == HttpConnect::ErrProxyNeg)
                err = ErrProxyNeg;
            else
                err = ErrProxyConnect;
        }
    } else if (t == Proxy::HttpPoll) {
        if (x == HttpPoll::ErrConnectionRefused)
            err = ErrConnectionRefused;
        else if (x == HttpPoll::ErrHostNotFound)
            err = ErrHostNotFound;
        else {
            proxyError = true;
            if (x == HttpPoll::ErrProxyAuth)
                err = ErrProxyAuth;
            else if (x == HttpPoll::ErrProxyNeg)
                err = ErrProxyNeg;
            else
                err = ErrProxyConnect;
        }
    } else if (t == Proxy::Socks) {
        if (x == SocksClient::ErrConnectionRefused)
            err = ErrConnectionRefused;
        else if (x == SocksClient::ErrHostNotFound)
            err = ErrHostNotFound;
        else {
            proxyError = true;
            if (x == SocksClient::ErrProxyAuth)
                err = ErrProxyAuth;
            else if (x == SocksClient::ErrProxyNeg)
                err = ErrProxyNeg;
            else
                err = ErrProxyConnect;
        }
    }

    // no-multi or proxy error means we quit
    if (proxyError) {
        cleanup();
        d->errorCode = err;
        emit error();
        return;
    }

#ifdef XMPP_DEBUG
    qDebug("bse1.3");
#endif
    cleanup();
    d->errorCode = ErrConnectionRefused;
    emit error();
}

void AdvancedConnector::http_syncStarted() { emit httpSyncStarted(); }

void AdvancedConnector::http_syncFinished() { emit httpSyncFinished(); }

void AdvancedConnector::t_timeout()
{
    // bs_error(-1);
}

QString AdvancedConnector::host() const { return d->host; }
