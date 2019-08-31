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

#ifndef TCPPORTRESERVER_H
#define TCPPORTRESERVER_H

#include <QObject>
#include <QSharedPointer>
#include <QTcpServer>
#include <QVariant>

namespace XMPP {
class TcpPortServer : public QObject
{
    Q_OBJECT
public:
    using Ptr = QSharedPointer<TcpPortServer>;

    enum PortType {
        NoType     = 0x0,
        Direct     = 0x1,
        NatAssited = 0x2,
        Tunneled   = 0x4
    };
    Q_DECLARE_FLAGS(PortTypes, PortType)

    struct Port
    {
        PortType portType = NoType;
        QString  publishHost;
        quint16  publishPort = 0;
        QVariant meta;
    };

    inline TcpPortServer(QTcpServer *serverSocket) : serverSocket(serverSocket) {}
    inline void            setPortInfo(const Port &port) { this->port = port; }
    inline QHostAddress    serverAddress() const { return serverSocket->serverAddress(); }
    inline quint16         serverPort() const { return serverSocket->serverPort(); }
    inline const QString  &publishHost() const { return port.publishHost; }
    inline quint16         publishPort() const { return port.publishPort; }
    inline PortType        portType() const { return port.portType; }
    inline const QVariant &meta() const { return port.meta; }

protected:
    QTcpServer *serverSocket = nullptr;
    Port port;
};

class TcpPortScope;
/**
 * @brief The TcpPortDiscoverer class
 *
 * Discovers / starts listening on a set of unique tcp ports.
 */
class TcpPortDiscoverer : public QObject
{
    Q_OBJECT
public:

    TcpPortDiscoverer(TcpPortScope *scope);
    bool setExternalHost(const QString &extHost, quint16 extPort, const QHostAddress &localIp, quint16 localPort);

    TcpPortServer::PortTypes inProgressPortTypes() const;
    bool isDepleted() const;

    /**
     * @brief setTypeMask sets expected port types mask and frees unnecessary resources
     * @param mask
     * @return remaining port types
     */
    XMPP::TcpPortServer::PortTypes setTypeMask(TcpPortServer::PortTypes mask);

    /**
     * @brief takeServers takes all discovered servers
     * @return
     */
    QList<TcpPortServer::Ptr> takeServers();
public slots:
    void start(); // it's autocalled after outside world is notified about this new discoverer
    void stop();
signals:
    void portAvailable();
private:
    TcpPortServer::PortTypes typeMask = TcpPortServer::PortTypes(TcpPortServer::Direct | TcpPortServer::NatAssited | TcpPortServer::Tunneled);
    TcpPortScope *scope = nullptr;
    QList<TcpPortServer::Ptr> servers;
};

class TcpPortReserver;
/**
 * @brief The TcpPortScope class
 *
 * Handles scopes of ports. For example just S5B dedicated ports.
 * There only on scope instance per scope id
 */
class TcpPortScope: public QObject
{
    Q_OBJECT
public:
    TcpPortScope();
    ~TcpPortScope();
    TcpPortDiscoverer* disco();
    QList<TcpPortServer::Ptr> allServers() const;
protected:
    virtual TcpPortServer* makeServer(QTcpServer *socket) = 0;
    virtual void destroyServer(TcpPortServer *server);

private:
    friend class TcpPortDiscoverer;
    TcpPortServer::Ptr bind(const QHostAddress &addr, quint16 port);

private:
    class Private;
    QScopedPointer<Private> d;
};

/**
 * @brief The TcpPortReserver class
 * This class should have the only instance per application
 */
class TcpPortReserver : public QObject
{
    Q_OBJECT
public:
    explicit TcpPortReserver(QObject *parent = nullptr);
    ~TcpPortReserver();

    /**
     * @brief scope returns a registered scope corresponding to scope id
     * @param id
     * @return scope
     * @note Do not reparent the object
     */
    TcpPortScope *scope(const QString &id);

    void registerScope(const QString &id, TcpPortScope *scope);
    TcpPortScope *unregisterScope(const QString &id);
signals:
    void newDiscoverer(TcpPortDiscoverer *discoverer);

public slots:
};
} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::TcpPortServer::PortTypes)

#endif // TCPPORTRESERVER_H
