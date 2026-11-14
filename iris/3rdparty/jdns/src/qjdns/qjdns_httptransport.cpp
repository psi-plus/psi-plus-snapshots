/*
 * qjdns_httptransport.cpp - RFC 8484 DNS-over-HTTPS using Qt Network
 * Copyright (C) 2026 Psi contributors
 * Licensed under the MIT license (see COPYING).
 */
#include "qjdns_httptransport.h"
#include "qjdns_externaltransport.h"
#include "qjdns_wire_p.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

class QJDnsHttpTransport::Private
{
public:
    QJDnsExternalTransport *core = nullptr;
    QUrl endpoint;
    QPointer<QNetworkAccessManager> manager;
    QHash<int, QPointer<QNetworkReply>> replies;
};

QJDnsHttpTransport::QJDnsHttpTransport(const QUrl &endpoint,
                                     QNetworkAccessManager *manager, QObject *parent)
    : QJDnsQueryTransport(parent), d(new Private)
{
    d->endpoint = endpoint;
    // QNAM remains caller-owned when supplied. Otherwise QObject parenting
    // gives the transport full ownership of its private network manager.
    d->manager = manager ? manager : new QNetworkAccessManager(this);
    d->core = new QJDnsExternalTransport(this);
    connect(d->core, &QJDnsExternalTransport::queryRequested, this,
            [this](int id, const QByteArray &packet) { submitDns(id, packet); });
    connect(d->core, &QJDnsExternalTransport::requestCancelled, this,
            [this](int id) { cancelDns(id); });
    connect(d->core, &QJDnsQueryTransport::responseReady, this, &QJDnsQueryTransport::responseReady);
    connect(d->core, &QJDnsQueryTransport::failed, this, &QJDnsQueryTransport::failed);
    connect(d->core, &QJDnsQueryTransport::debugLine, this, &QJDnsQueryTransport::debugLine);
}

QJDnsHttpTransport::~QJDnsHttpTransport()
{
    d->core->disconnect(this);
    const auto ids = d->replies.keys();
    for(int id : ids)
        cancelDns(id);
    delete d;
}

QNetworkAccessManager *QJDnsHttpTransport::networkAccessManager() const
{
    return d->manager.data();
}

void QJDnsHttpTransport::setTimeout(int milliseconds)
{
    d->core->setTimeout(milliseconds);
}

void QJDnsHttpTransport::startRequest(int id, const QByteArray &packet)
{
    d->core->startRequest(id, packet);
}

void QJDnsHttpTransport::cancelRequest(int id)
{
    d->core->cancelRequest(id);
}

void QJDnsHttpTransport::cancelDns(int id)
{
    auto reply = d->replies.take(id);
    if(reply) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void QJDnsHttpTransport::finishDns(int id, const QByteArray &response, const QString &error)
{
    if(!d->replies.contains(id))
        return;
    cancelDns(id);
    if(!error.isEmpty())
        d->core->failRequest(id, error);
    else
        d->core->completeRequest(id, response);
}

void QJDnsHttpTransport::submitDns(int id, const QByteArray &packet)
{
    if(!d->manager || !d->endpoint.isValid()
       || d->endpoint.scheme() != QLatin1String("https")
       || d->endpoint.host().isEmpty() || !d->endpoint.userInfo().isEmpty()
       || !d->endpoint.fragment().isEmpty()) {
        d->core->failRequest(id, QStringLiteral("DoH requires an HTTPS endpoint and a valid network manager"));
        return;
    }
    if(packet.size() < 12 || packet.size() > QJDnsWire::MaxMessageSize) {
        d->core->failRequest(id, QStringLiteral("Invalid DNS request size"));
        return;
    }

    QNetworkRequest request(d->endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/dns-message"));
    request.setRawHeader("Accept", "application/dns-message");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = d->manager->post(request, packet);
    d->replies.insert(id, reply);
    connect(reply, &QNetworkReply::finished, this, [this, id, reply]() {
        if(!d->replies.contains(id))
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray mime = reply->rawHeader("Content-Type").split(';').value(0).trimmed().toLower();
        if(reply->error() != QNetworkReply::NoError || status != 200
           || mime != QByteArrayLiteral("application/dns-message")) {
            finishDns(id, {}, QStringLiteral("DoH request failed (HTTP %1): %2")
                                 .arg(status).arg(reply->errorString()));
            return;
        }
        if(reply->bytesAvailable() > QJDnsWire::MaxMessageSize) {
            finishDns(id, {}, QStringLiteral("DoH DNS response exceeds size limit"));
            return;
        }
        finishDns(id, reply->readAll(), {});
    });
}
