/*
 * qjdns_externaltransport.cpp - asynchronous network-agnostic DNS transport
 * SPDX-License-Identifier: MIT
 */
#include "qjdns_externaltransport.h"
#include "qjdns_wire_p.h"

#include <QHash>
#include <QThread>
#include <QTimer>

struct QJDnsExternalTransport::Private
{
    struct Request {
        QByteArray query;
        QTimer *timer = nullptr;
        bool dispatched = false;
    };
    QHash<int, Request *> pending;
    int timeoutMs = 12000;
};

QJDnsExternalTransport::QJDnsExternalTransport(QObject *parent)
    : QJDnsQueryTransport(parent), d(new Private)
{
}

QJDnsExternalTransport::~QJDnsExternalTransport()
{
    const auto ids = d->pending.keys();
    for(int id : ids)
        forget(id, true);
    delete d;
}

void QJDnsExternalTransport::setTimeout(int milliseconds)
{
    if(milliseconds > 0)
        d->timeoutMs = milliseconds;
}

void QJDnsExternalTransport::forget(int id, bool notifyCancel)
{
    Private::Request *r = d->pending.take(id);
    if(!r)
        return;
    r->timer->stop();
    r->timer->deleteLater();
    const bool dispatched = r->dispatched;
    delete r;
    if(notifyCancel && dispatched)
        emit requestCancelled(id);
}

void QJDnsExternalTransport::startRequest(int id, const QByteArray &dnsPacket)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if(d->pending.contains(id))
        return;
    auto *r = new Private::Request;
    r->query = dnsPacket;
    r->timer = new QTimer(this);
    r->timer->setSingleShot(true);
    d->pending.insert(id, r);

    // Even a rejected packet is tracked until its deferred error is delivered:
    // cancelRequest() must be able to suppress that error before dispatch.
    if(dnsPacket.size() < 12 || dnsPacket.size() > QJDnsWire::MaxMessageSize)
    {
        QTimer::singleShot(0, this, [this, id]() {
            failRequest(id, QStringLiteral("Invalid DNS query length"), NetworkError);
        });
        return;
    }
    connect(r->timer, &QTimer::timeout, this, [this, id]() {
        if(!d->pending.contains(id))
            return;
        forget(id, true);
        emit failed(id, QStringLiteral("External DNS request timed out"), Timeout);
    });
    r->timer->start(d->timeoutMs);
    // C resolver callbacks may occur inside jdns_step(): never reenter it.
    QTimer::singleShot(0, this, [this, id]() {
        auto *request = d->pending.value(id);
        if(request)
        {
            request->dispatched = true;
            // A receiver may synchronously complete/cancel the request and
            // delete its storage before later receivers inspect the packet.
            const QByteArray packet = request->query;
            emit queryRequested(id, packet);
        }
    });
}

void QJDnsExternalTransport::cancelRequest(int id)
{
    Q_ASSERT(thread() == QThread::currentThread());
    forget(id, true);
}

void QJDnsExternalTransport::completeRequest(int id, const QByteArray &dnsResponse)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if(!d->pending.contains(id))
        return;
    forget(id, false);
    emit responseReady(id, dnsResponse);
}

void QJDnsExternalTransport::failRequest(int id, const QString &reason, Failure kind)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if(!d->pending.contains(id))
        return;
    forget(id, false);
    emit failed(id, reason, kind);
}
