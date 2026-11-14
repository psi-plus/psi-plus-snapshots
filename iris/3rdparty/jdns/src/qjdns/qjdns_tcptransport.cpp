/*
 * qjdns_tcptransport.cpp - RFC 7766 DNS-over-TCP transport
 * Copyright (C) 2026 Psi contributors
 * Licensed under the MIT license (see COPYING).
 */
#include "qjdns_tcptransport.h"
#include "qjdns_externaltransport.h"
#include "qjdns_wire_p.h"

#include <QAbstractSocket>
#include <QHash>
#include <QPointer>
#include <QTcpSocket>

namespace {
void append16(QByteArray *out, quint16 value)
{
    out->append(char(value >> 8));
    out->append(char(value & 0xff));
}
}

class QJDnsTcpTransport::Private
{
public:
    struct Request {
        QPointer<QTcpSocket> socket;
        QByteArray received;
        int expected = -1;
    };

    QJDnsExternalTransport *core = nullptr;
    QString server;
    quint16 port = 0;
    QNetworkProxy proxy;
    QHash<int, Request *> requests;
};

QJDnsTcpTransport::QJDnsTcpTransport(const QString &server, quint16 port,
                                     const QNetworkProxy &proxy, QObject *parent)
    : QJDnsQueryTransport(parent), d(new Private)
{
    d->server = server;
    d->port = port;
    d->proxy = proxy;
    d->core = new QJDnsExternalTransport(this);
    connect(d->core, &QJDnsExternalTransport::queryRequested, this,
            [this](int id, const QByteArray &packet) { submitDns(id, packet); });
    connect(d->core, &QJDnsExternalTransport::requestCancelled, this,
            [this](int id) { cancelDns(id); });
    connect(d->core, &QJDnsQueryTransport::responseReady, this, &QJDnsQueryTransport::responseReady);
    connect(d->core, &QJDnsQueryTransport::failed, this, &QJDnsQueryTransport::failed);
    connect(d->core, &QJDnsQueryTransport::debugLine, this, &QJDnsQueryTransport::debugLine);
}

QJDnsTcpTransport::~QJDnsTcpTransport()
{
    // Base destructor can emit requestCancelled(); disconnect our callbacks
    // before the derived state is destroyed.
    d->core->disconnect(this);
    const auto ids = d->requests.keys();
    for(int id : ids)
        cancelDns(id);
    delete d;
}

void QJDnsTcpTransport::setTimeout(int milliseconds)
{
    d->core->setTimeout(milliseconds);
}

void QJDnsTcpTransport::startRequest(int id, const QByteArray &packet)
{
    d->core->startRequest(id, packet);
}

void QJDnsTcpTransport::cancelRequest(int id)
{
    d->core->cancelRequest(id);
}

void QJDnsTcpTransport::cancelDns(int id)
{
    auto *request = d->requests.take(id);
    if(!request)
        return;
    if(request->socket) {
        request->socket->disconnect(this);
        request->socket->abort();
        request->socket->deleteLater();
    }
    delete request;
}

void QJDnsTcpTransport::finishDns(int id, const QByteArray &response, const QString &error)
{
    if(!d->requests.contains(id))
        return;
    cancelDns(id);
    if(!error.isEmpty())
        d->core->failRequest(id, error);
    else
        d->core->completeRequest(id, response);
}

void QJDnsTcpTransport::submitDns(int id, const QByteArray &packet)
{
    if(d->server.isEmpty() || !d->port) {
        d->core->failRequest(id, QStringLiteral("DNS-over-TCP server and port are required"));
        return;
    }
    if(packet.size() < 12 || packet.size() > QJDnsWire::MaxMessageSize) {
        d->core->failRequest(id, QStringLiteral("Invalid DNS request size"));
        return;
    }

    auto *request = new Private::Request;
    auto *socket = new QTcpSocket(this);
    request->socket = socket;
    d->requests.insert(id, request);
    socket->setProxy(d->proxy);

    connect(socket, &QTcpSocket::connected, this, [this, id, packet, socket]() {
        if(!d->requests.contains(id))
            return;
        QByteArray framed;
        append16(&framed, quint16(packet.size()));
        framed += packet;
        if(socket->write(framed) != framed.size())
            finishDns(id, {}, QStringLiteral("Could not queue complete DNS-over-TCP request"));
    });

    connect(socket, &QTcpSocket::readyRead, this, [this, id, socket]() {
        auto *req = d->requests.value(id);
        if(!req)
            return;
        req->received += socket->readAll();
        if(req->received.size() > QJDnsWire::MaxMessageSize + 2) {
            finishDns(id, {}, QStringLiteral("DNS-over-TCP response exceeds size limit"));
            return;
        }
        if(req->expected == -1 && req->received.size() >= 2) {
            req->expected = QJDnsWire::read16(req->received, 0);
            if(req->expected < 12 || req->expected > QJDnsWire::MaxMessageSize) {
                finishDns(id, {}, QStringLiteral("Invalid DNS-over-TCP frame length"));
                return;
            }
        }
        if(req->expected != -1 && req->received.size() >= req->expected + 2)
            finishDns(id, req->received.mid(2, req->expected), {});
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QTcpSocket::errorOccurred, this, [this, id, socket](QAbstractSocket::SocketError) {
        finishDns(id, {}, QStringLiteral("DNS-over-TCP socket: %1").arg(socket->errorString()));
    });
#else
    connect(socket, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
            this, [this, id, socket](QAbstractSocket::SocketError) {
        finishDns(id, {}, QStringLiteral("DNS-over-TCP socket: %1").arg(socket->errorString()));
    });
#endif
    connect(socket, &QTcpSocket::disconnected, this, [this, id]() {
        finishDns(id, {}, QStringLiteral("DNS-over-TCP connection closed before receiving a complete response"));
    });
    socket->connectToHost(d->server, d->port);
}
