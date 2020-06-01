/*
 * tcpportreserver.cpp - a utility to bind local tcp server sockets
 * Copyright (C) 2019  Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "tcpportreserver.h"

#include "ice176.h"

#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>

namespace XMPP {
TcpPortDiscoverer::TcpPortDiscoverer(TcpPortScope *scope) : QObject(scope), scope(scope) { }

bool TcpPortDiscoverer::setExternalHost(const QString &extHost, quint16 extPort, const QHostAddress &localAddr,
                                        quint16 localPort)
{
    if (!(typeMask & TcpPortServer::NatAssited)) {
        return false; // seems like we don't need nat-assited
    }
    auto server = scope->bind(localAddr, localPort);
    if (!server) {
        return false;
    }
    TcpPortServer::Port p;
    p.portType    = TcpPortServer::NatAssited;
    p.publishHost = extHost;
    p.publishPort = extPort;
    server->setPortInfo(p);
    servers.append(server);
    emit portAvailable();
    return true;
}

TcpPortServer::PortTypes TcpPortDiscoverer::inProgressPortTypes() const
{
    return {}; // same as for stop()
}

bool TcpPortDiscoverer::isDepleted() const
{
    return servers.size() == 0; // TODO and no active subdiscoveries
}

TcpPortServer::PortTypes TcpPortDiscoverer::setTypeMask(TcpPortServer::PortTypes mask)
{
    this->typeMask = mask;
    // drop ready ports if any
    auto it = std::remove_if(servers.begin(), servers.end(), [mask](auto &s) { return !(s->portType() & mask); });
    servers.erase(it, servers.end());

    TcpPortServer::PortTypes pendingTypes;
    for (auto &s : servers)
        pendingTypes |= s->portType();

    // TODO drop pending subdiscoveries too and update pendingType when implemented
    return pendingTypes;
}

void TcpPortDiscoverer::start()
{
    QList<QHostAddress> listenAddrs;
    auto const          interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &ni : interfaces) {
        if (!(ni.flags() & (QNetworkInterface::IsUp | QNetworkInterface::IsRunning))) {
            continue;
        }
        if (ni.flags() & QNetworkInterface::IsLoopBack) {
            continue;
        }
        QList<QNetworkAddressEntry> entries = ni.addressEntries();
        for (const QNetworkAddressEntry &na : entries) {
            QHostAddress h = na.ip();
            if (h.isLoopback()) {
                continue;
            }

            // don't put the same address in twice.
            //   this also means that if there are
            //   two link-local ipv6 interfaces
            //   with the exact same address, we
            //   only use the first one
            if (listenAddrs.contains(h))
                continue;
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
            if (h.protocol() == QAbstractSocket::IPv6Protocol && h.isLinkLocal())
#else
            if (h.protocol() == QAbstractSocket::IPv6Protocol && XMPP::Ice176::isIPv6LinkLocalAddress(h))
#endif
                h.setScopeId(ni.name());
            listenAddrs += h;
        }
    }

    for (auto &h : listenAddrs) {
        auto server = scope->bind(h, 0);
        if (!server) {
            continue;
        }
        TcpPortServer::Port p;
        p.portType        = TcpPortServer::Direct;
        QHostAddress addr = server->serverAddress();
        addr.setScopeId(QString());
        p.publishHost = addr.toString();
        p.publishPort = server->serverPort();
        server->setPortInfo(p);
        servers.append(server);
    }

    if (listenAddrs.size()) {
        emit portAvailable();
    }
}

void TcpPortDiscoverer::stop()
{
    // nothing really to do here. but if we invent extension interface it can call stop on subdisco
}

QList<TcpPortServer::Ptr> TcpPortDiscoverer::takeServers()
{
    auto ret = servers;
    servers.clear();
    for (auto &p : ret) {
        p->disconnect(this);
    }
    return ret;
}

// --------------------------------------------------------------------------
// TcpPortScope
// --------------------------------------------------------------------------
struct TcpPortScope::Private {
    QHash<QPair<QHostAddress, quint16>, QWeakPointer<TcpPortServer>> servers;
};

TcpPortScope::TcpPortScope() : d(new Private) { }

TcpPortScope::~TcpPortScope() { }

TcpPortDiscoverer *TcpPortScope::disco()
{
    auto discoverer = new TcpPortDiscoverer(this);
    QMetaObject::invokeMethod(parent(), "newDiscoverer", Q_ARG(TcpPortDiscoverer *, discoverer));
    QMetaObject::invokeMethod(discoverer, "start", Qt::QueuedConnection);
    return discoverer;
}

QList<TcpPortServer::Ptr> TcpPortScope::allServers() const
{
    QList<TcpPortServer::Ptr> ret;
    for (auto &s : d->servers) {
        auto sl = s.lock();
        if (sl) {
            ret.append(sl);
        }
    }
    return ret;
}

void TcpPortScope::destroyServer(TcpPortServer *server) { delete server; }

TcpPortServer::Ptr TcpPortScope::bind(const QHostAddress &addr, quint16 port)
{
    if (port) {
        auto srv = d->servers.value(qMakePair(addr, port)).toStrongRef();
        if (srv) {
            return srv;
        }
    }
    auto socket = new QTcpServer(this);
    if (!socket->listen(addr, port)) {
        delete socket;
        return TcpPortServer::Ptr();
    }
    auto server = makeServer(socket);

    TcpPortServer::Ptr shared(server, [](TcpPortServer *s) {
        auto scope = qobject_cast<TcpPortScope *>(s->parent());
        if (scope) {
            scope->d->servers.remove(qMakePair(s->serverAddress(), s->serverPort()));
            scope->destroyServer(s);
        } else {
            delete s;
        }
    });
    d->servers.insert(qMakePair(socket->serverAddress(), socket->serverPort()), shared.toWeakRef());

    return shared;
}

// --------------------------------------------------------------------------
// TcpPortScope
// --------------------------------------------------------------------------
TcpPortReserver::TcpPortReserver(QObject *parent) : QObject(parent) { }

TcpPortReserver::~TcpPortReserver() { }

TcpPortScope *TcpPortReserver::scope(const QString &id)
{
    return findChild<TcpPortScope *>(id, Qt::FindDirectChildrenOnly);
}

void TcpPortReserver::registerScope(const QString &id, TcpPortScope *scope)
{
    scope->setObjectName(id);
    scope->setParent(this);
}

TcpPortScope *TcpPortReserver::unregisterScope(const QString &id)
{
    auto s = scope(id);
    if (s) {
        s->setParent(nullptr);
    }
    return s;
}
} // namespace XMPP
