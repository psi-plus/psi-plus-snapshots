/*
 * Copyright (C) 2005-2008  Justin Karneges
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "qjdns_p.h"

#include <time.h>
#include "qjdns_sock.h"
#include "qjdns_datagramtransport_p.h"
#include "qjdns_querytransport.h"
#include "qjdns_httptransport.h"
#include "qjdns_tcptransport.h"
#include "qjdnsshared.h"
#include <memory>

#ifdef Q_OS_ANDROID
QJDnsQueryTransport *qjdns_create_android_query_transport(QObject *parent);
#endif

#include <QUdpSocket>

// for fprintf
#include <stdio.h>

#include <cstring>

// safeobj stuff, from qca

static void releaseAndDeleteLater(QObject *owner, QObject *obj)
{
    obj->disconnect(owner);
    obj->setParent(0);
    obj->deleteLater();
}

SafeTimer::SafeTimer(QObject *parent)
    : QObject(parent)
{
    t = new QTimer(this);
    connect(t, SIGNAL(timeout()), SIGNAL(timeout()));
}

SafeTimer::~SafeTimer()
{
    releaseAndDeleteLater(this, t);
}

int SafeTimer::interval() const
{
    return t->interval();
}

bool SafeTimer::isActive() const
{
    return t->isActive();
}

bool SafeTimer::isSingleShot() const
{
    return t->isSingleShot();
}

void SafeTimer::setInterval(int msec)
{
    t->setInterval(msec);
}

void SafeTimer::setSingleShot(bool singleShot)
{
    t->setSingleShot(singleShot);
}

int SafeTimer::timerId() const
{
    return t->timerId();
}

void SafeTimer::start(int msec)
{
    t->start(msec);
}

void SafeTimer::start()

{
    t->start();
}

void SafeTimer::stop()
{
    t->stop();
}

static jdns_string_t *qt2str(const QByteArray &in)
{
    jdns_string_t *out = jdns_string_new();
    jdns_string_set(out, (const unsigned char *)in.data(), in.size());
    return out;
}

static QByteArray str2qt(const jdns_string_t *in)
{
    return QByteArray((const char *)in->data, in->size);
}

static void qt2addr_set(jdns_address_t *addr, const QHostAddress &host)
{
    if(host.protocol() == QAbstractSocket::IPv6Protocol)
        jdns_address_set_ipv6(addr, host.toIPv6Address().c);
    else
        jdns_address_set_ipv4(addr, host.toIPv4Address());
}

static jdns_address_t *qt2addr(const QHostAddress &host)
{
    jdns_address_t *addr = jdns_address_new();
    qt2addr_set(addr, host);
    return addr;
}

static QHostAddress addr2qt(const jdns_address_t *addr)
{
    if(addr->isIpv6)
        return QHostAddress(addr->addr.v6);
    else
        return QHostAddress(addr->addr.v4);
}

static QJDns::Record import_record(const jdns_rr_t *in)
{
    QJDns::Record out;

    out.owner = QByteArray((const char *)in->owner);
    out.ttl = in->ttl;
    out.type = in->type;
    out.rdata = QByteArray((const char *)in->rdata, in->rdlength);

    // known
    if(in->haveKnown)
    {
        int type = in->type;

        if(type == QJDns::A || type == QJDns::Aaaa)
        {
            out.haveKnown = true;
            out.address = addr2qt(in->data.address);
        }
        else if(type == QJDns::Mx)
        {
            out.haveKnown = true;
            out.name = QByteArray((const char *)in->data.server->name);
            out.priority = in->data.server->priority;
        }
        else if(type == QJDns::Srv)
        {
            out.haveKnown = true;
            out.name = QByteArray((const char *)in->data.server->name);
            out.priority = in->data.server->priority;
            out.weight = in->data.server->weight;
            out.port = in->data.server->port;
        }
        else if(type == QJDns::Cname || type == QJDns::Ptr || type == QJDns::Ns)
        {
            out.haveKnown = true;
            out.name = QByteArray((const char *)in->data.name);
        }
        else if(type == QJDns::Txt)
        {
            out.haveKnown = true;
            out.texts.clear();
            for(int n = 0; n < in->data.texts->count; ++n)
                out.texts += str2qt(in->data.texts->item[n]);
        }
        else if(type == QJDns::Hinfo)
        {
            out.haveKnown = true;
            out.cpu = str2qt(in->data.hinfo.cpu);
            out.os = str2qt(in->data.hinfo.os);
        }
    }

    return out;
}

static jdns_rr_t *export_record(const QJDns::Record &in)
{
    jdns_rr_t *out = jdns_rr_new();

    jdns_rr_set_owner(out, (const unsigned char *)in.owner.data());
    out->ttl = in.ttl;

    // if we have known, use that
    if(in.haveKnown)
    {
        int type = in.type;

        if(type == QJDns::A)
        {
            jdns_address_t *addr = qt2addr(in.address);
            jdns_rr_set_A(out, addr);
            jdns_address_delete(addr);
        }
        else if(type == QJDns::Aaaa)
        {
            jdns_address_t *addr = qt2addr(in.address);
            jdns_rr_set_AAAA(out, addr);
            jdns_address_delete(addr);
        }
        else if(type == QJDns::Mx)
        {
            jdns_rr_set_MX(out, (const unsigned char *)in.name.data(), in.priority);
        }
        else if(type == QJDns::Srv)
        {
            jdns_rr_set_SRV(out, (const unsigned char *)in.name.data(), in.port, in.priority, in.weight);
        }
        else if(type == QJDns::Cname)
        {
            jdns_rr_set_CNAME(out, (const unsigned char *)in.name.data());
        }
        else if(type == QJDns::Ptr)
        {
            jdns_rr_set_PTR(out, (const unsigned char *)in.name.data());
        }
        else if(type == QJDns::Txt)
        {
            jdns_stringlist_t *list = jdns_stringlist_new();
            for(int n = 0; n < in.texts.count(); ++n)
            {
                jdns_string_t *str = qt2str(in.texts[n]);
                jdns_stringlist_append(list, str);
                jdns_string_delete(str);
            }
            jdns_rr_set_TXT(out, list);
            jdns_stringlist_delete(list);
        }
        else if(type == QJDns::Hinfo)
        {
            jdns_string_t *cpu = qt2str(in.cpu);
            jdns_string_t *os = qt2str(in.os);
            jdns_rr_set_HINFO(out, cpu, os);
            jdns_string_delete(cpu);
            jdns_string_delete(os);
        }
        else if(type == QJDns::Ns)
        {
            jdns_rr_set_NS(out, (const unsigned char *)in.name.data());
        }
    }
    else
        jdns_rr_set_record(out, in.type, (const unsigned char *)in.rdata.data(), in.rdata.size());

    return out;
}

//----------------------------------------------------------------------------
// QJDns::NameServer
//----------------------------------------------------------------------------
QJDns::NameServer::NameServer()
{
    port = JDNS_UNICAST_PORT;
}

//----------------------------------------------------------------------------
// QJDns::Record
//----------------------------------------------------------------------------
QJDns::Record::Record()
{
    ttl = 0;
    type = -1;
    haveKnown = false;
}

bool QJDns::Record::verify() const
{
    jdns_rr_t *rr = export_record(*this);
    int ok = jdns_rr_verify(rr);
    jdns_rr_delete(rr);
    return (ok ? true : false);
}

//----------------------------------------------------------------------------
// QJDns
//----------------------------------------------------------------------------
static int my_srand_done = 0;

static void my_srand()
{
    if(my_srand_done)
        return;

    // lame attempt at randomizing without srand
    int count = ::time(NULL) % 128;
    for(int n = 0; n < count; ++n)
        rand();

    my_srand_done = 1;
}

QJDns::Private::Private(QJDns *_q)
    : QObject(_q)
    , q(_q)
    , stepTrigger(this)
    , debugTrigger(this)
    , stepTimeout(this)
    , pErrors(0)
    , pPublished(0)
    , pResponses(0)
{
    sess = 0;
    transport = 0;
    queryTransport = nullptr;
    shutting_down = false;
    new_debug_strings = false;
    pending = 0;

    connect(&stepTrigger, SIGNAL(timeout()), SLOT(doNextStepSlot()));
    stepTrigger.setSingleShot(true);

    connect(&debugTrigger, SIGNAL(timeout()), SLOT(doDebug()));
    debugTrigger.setSingleShot(true);

    connect(&stepTimeout, SIGNAL(timeout()), SLOT(st_timeout()));
    stepTimeout.setSingleShot(true);

    my_srand();

    clock.start();
}

QJDns::Private::~Private()
{
    cleanup();
}

void QJDns::Private::cleanup()
{
    if(sess)
    {
        jdns_session_delete(sess);
        sess = 0;
    }

    delete transport;
    transport = 0;
    delete queryTransport;
    queryTransport = nullptr;

    shutting_down = false;
    pending = 0;

    stepTrigger.stop();
    stepTimeout.stop();
    need_handle = 0;
}

bool QJDns::Private::init(QJDns::Mode _mode, const QHostAddress &address)
{
    // Never overwrite a live JDNS session or its transport callbacks.
    if(sess)
        return false;
    mode = _mode;
#ifdef Q_OS_ANDROID
    if(mode == Unicast && !queryTransport && !transport)
        queryTransport = qjdns_create_android_query_transport(this);
#endif

    if(queryTransport)
    {
        if(mode != Unicast)
            return false;
        connect(queryTransport, &QJDnsQueryTransport::responseReady, this,
                [this](int id, const QByteArray &packet) {
            if(sess && !shutting_down)
            {
                jdns_async_complete(sess, id,
                    reinterpret_cast<const unsigned char *>(packet.constData()), packet.size());
                process();
            }
        });
        connect(queryTransport, &QJDnsQueryTransport::failed, this,
                [this](int id, const QString &reason, QJDnsQueryTransport::Failure failure) {
            debug_strings += reason;
            processDebug();
            if(sess && !shutting_down)
            {
                jdns_async_fail(sess, id, failure == QJDnsQueryTransport::Timeout
                                              ? JDNS_STATUS_TIMEOUT : JDNS_STATUS_ERROR);
                process();
            }
        });
        connect(queryTransport, &QJDnsQueryTransport::debugLine, this,
                &QJDns::Private::transport_debugLine);
    }
    else
    {
        if(!transport)
            transport = qjdns_create_datagram_transport(this);
        connect(transport, &QJDnsDatagramTransport::readyRead, this, &QJDns::Private::transport_readyRead);
        connect(transport, &QJDnsDatagramTransport::packetWritten, this, &QJDns::Private::transport_packetWritten);
        connect(transport, &QJDnsDatagramTransport::debugLine, this, &QJDns::Private::transport_debugLine);
    }

    jdns_callbacks_t callbacks = {};
    callbacks.app = this;
    callbacks.time_now = cb_time_now;
    callbacks.rand_int = cb_rand_int;
    callbacks.debug_line = cb_debug_line;
    sess = jdns_session_new(&callbacks);
    jdns_set_hold_ids_enabled(sess, 1);
    need_handle = false;

    int ret;

    if(queryTransport)
    {
        jdns_async_callbacks_t asyncCallbacks = {};
        asyncCallbacks.send = cb_async_send;
        asyncCallbacks.cancel = cb_async_cancel;
        ret = jdns_init_async_unicast(sess, &asyncCallbacks);
    }
    else
    {
        jdns_datagram_callbacks_t datagramCallbacks = {};
        datagramCallbacks.transport_bind = cb_transport_bind;
        datagramCallbacks.transport_unbind = cb_transport_unbind;
        datagramCallbacks.transport_read = cb_transport_read;
        datagramCallbacks.transport_write = cb_transport_write;
        jdns_address_t *baddr = qt2addr(address);
        if(mode == Unicast)
            ret = jdns_init_unicast(sess, &datagramCallbacks, baddr, 0);
        else
        {
            jdns_address_t *maddr;
            if(address.protocol() == QAbstractSocket::IPv6Protocol)
                maddr = jdns_address_multicast6_new();
            else
                maddr = jdns_address_multicast4_new();
            ret = jdns_init_multicast(sess, &datagramCallbacks, baddr, JDNS_MULTICAST_PORT, maddr);
            jdns_address_delete(maddr);
        }
        jdns_address_delete(baddr);
    }

    if(!ret)
    {
        jdns_session_delete(sess);
        sess = 0;
        delete transport;
        transport = 0;
        delete queryTransport;
        queryTransport = nullptr;
        return false;
    }

    return true;
}

bool QJDns::Private::setNameServers(const QList<NameServer> &nslist)
{
    if(queryTransport || !sess)
        return false;

    jdns_nameserverlist_t *addrs = jdns_nameserverlist_new();
    for(int n = 0; n < nslist.count(); ++n)
    {
        jdns_address_t *addr = qt2addr(nslist[n].address);
        jdns_nameserverlist_append(addrs, addr, nslist[n].port);
        jdns_address_delete(addr);
    }
    const bool applied = jdns_set_nameservers(sess, addrs) != 0;
    jdns_nameserverlist_delete(addrs);
    return applied;
}

void QJDns::Private::process()
{
    if(!stepTrigger.isActive())
    {
        stepTimeout.stop();
        stepTrigger.start();
    }
}

void QJDns::Private::processDebug()
{
    new_debug_strings = true;
    if(!debugTrigger.isActive())
        debugTrigger.start();
}

void QJDns::Private::doNextStep()
{
    if(shutting_down && complete_shutdown)
    {
        cleanup();
        emit q->shutdownFinished();
        return;
    }

    QPointer<QObject> self = this;

    int ret = jdns_step(sess);

    QList<LateError> errors;
    QList<int> published;
    QList<LateResponse> responses;
    bool finish_shutdown = false;

    pErrors = &errors;
    pPublished = &published;
    pResponses = &responses;

    while(1)
    {
        jdns_event_t *e = jdns_next_event(sess);
        if(!e)
            break;

        if(e->type == JDNS_EVENT_SHUTDOWN)
        {
            finish_shutdown = true;
        }
        else if(e->type == JDNS_EVENT_PUBLISH)
        {
            if(e->status != JDNS_STATUS_SUCCESS)
            {
                QJDns::Error error;
                if(e->status == JDNS_STATUS_CONFLICT)
                    error = QJDns::ErrorConflict;
                else
                    error = QJDns::ErrorGeneric;
                LateError le;
                le.source_type = 1;
                le.id = e->id;
                le.error = error;
                errors += le;
            }
            else
            {
                published += e->id;
            }
        }
        else if(e->type == JDNS_EVENT_RESPONSE)
        {
            if(e->status != JDNS_STATUS_SUCCESS)
            {
                QJDns::Error error;
                if(e->status == JDNS_STATUS_NXDOMAIN)
                    error = QJDns::ErrorNXDomain;
                else if(e->status == JDNS_STATUS_TIMEOUT)
                    error = QJDns::ErrorTimeout;
                else
                    error = QJDns::ErrorGeneric;
                LateError le;
                le.source_type = 0;
                le.id = e->id;
                le.error = error;
                errors += le;
            }
            else
            {
                QJDns::Response out_response;
                for(int n = 0; n < e->response->answerCount; ++n)
                    out_response.answerRecords += import_record(e->response->answerRecords[n]);
                LateResponse lr;
                lr.id = e->id;
                lr.response = out_response;
                if(mode == Unicast)
                    lr.do_cancel = true;
                else
                    lr.do_cancel = false;
                responses += lr;
            }
        }

        jdns_event_delete(e);
    }

    if(ret & JDNS_STEP_TIMER)
        stepTimeout.start(jdns_next_timer(sess));
    else
        stepTimeout.stop();

    need_handle = (ret & JDNS_STEP_HANDLE);

    // read the lists safely enough so that items can be deleted
    //   behind our back

    while(!errors.isEmpty())
    {
        LateError i = errors.takeFirst();
        if(i.source_type == 0)
            jdns_cancel_query(sess, i.id);
        else
            jdns_cancel_publish(sess, i.id);
        emit q->error(i.id, i.error);
        if(!self)
            return;
    }

    while(!published.isEmpty())
    {
        int i = published.takeFirst();
        emit q->published(i);
        if(!self)
            return;
    }

    while(!responses.isEmpty())
    {
        LateResponse i = responses.takeFirst();
        if(i.do_cancel)
            jdns_cancel_query(sess, i.id);
        emit q->resultsReady(i.id, i.response);
        if(!self)
            return;
    }

    if(finish_shutdown)
    {
        // if we have pending packets to write, stick around
        if(pending > 0)
        {
            pending_wait = true;
        }
        else
        {
            complete_shutdown = true;
            process();
        }
    }

    pErrors = 0;
    pPublished = 0;
    pResponses = 0;
}

void QJDns::Private::removeCancelled(int id)
{
    if(pErrors)
    {
        for(int n = 0; n < pErrors->count(); ++n)
        {
            if(pErrors->at(n).id == id)
            {
                pErrors->removeAt(n);
                --n; // adjust position
            }
        }
    }

    if(pPublished)
    {
        for(int n = 0; n < pPublished->count(); ++n)
        {
            if(pPublished->at(n) == id)
            {
                pPublished->removeAt(n);
                --n; // adjust position
            }
        }
    }

    if(pResponses)
    {
        for(int n = 0; n < pResponses->count(); ++n)
        {
            if(pResponses->at(n).id == id)
            {
                pResponses->removeAt(n);
                --n; // adjust position
            }
        }
    }
}

void QJDns::Private::transport_readyRead(int handle)
{
    if(need_handle)
    {
        jdns_set_handle_readable(sess, handle);
        process();
    }
    else
    {
        // Eat a response that arrived while jdns was not interested in
        // readable handles, preserving the previous socket behaviour.
        QJDnsDatagramTransport::Packet packet;
        transport->takeResponse(handle, &packet);
    }
}

void QJDns::Private::transport_packetWritten()
{
    if(pending > 0)
    {
        --pending;
        if(shutting_down && pending_wait && pending == 0)
        {
            pending_wait = false;
            complete_shutdown = true;
            process();
        }
    }
}

void QJDns::Private::transport_debugLine(const QString &line)
{
    debug_strings += line;
    processDebug();
}

void QJDns::Private::st_timeout()
{
    doNextStep();
}

void QJDns::Private::doNextStepSlot()
{
    doNextStep();
}

void QJDns::Private::doDebug()
{
    if(new_debug_strings)
    {
        new_debug_strings = false;
        if(!debug_strings.isEmpty())
            emit q->debugLinesReady();
    }
}

// jdns callbacks
int QJDns::Private::cb_time_now(jdns_session_t *, void *app)
{
    QJDns::Private *self = (QJDns::Private *)app;

    return self->clock.elapsed();
}

int QJDns::Private::cb_rand_int(jdns_session_t *, void *)
{
    return rand() % 65536;
}

void QJDns::Private::cb_debug_line(jdns_session_t *, void *app, const char *str)
{
    QJDns::Private *self = (QJDns::Private *)app;

    self->debug_strings += QString::fromLatin1(str);
    self->processDebug();
}

void QJDns::Private::cb_async_send(jdns_session_t *, void *app, int id,
                                   const unsigned char *data, int size)
{
    auto *self = static_cast<QJDns::Private *>(app);
    self->queryTransport->startRequest(id, QByteArray(
        reinterpret_cast<const char *>(data), size));
}

void QJDns::Private::cb_async_cancel(jdns_session_t *, void *app, int id)
{
    auto *self = static_cast<QJDns::Private *>(app);
    self->queryTransport->cancelRequest(id);
}

int QJDns::Private::cb_transport_bind(jdns_session_t *, void *app, const jdns_address_t *addr, int port, const jdns_address_t *maddr)
{
    QJDns::Private *self = (QJDns::Private *)app;

    // we always pass non-null to jdns_init, so this should be a valid address
    QHostAddress host = addr2qt(addr);
    QHostAddress multicastAddress;
    if(maddr)
        multicastAddress = addr2qt(maddr);

    return self->transport->open(host, static_cast<quint16>(port), multicastAddress);
}

void QJDns::Private::cb_transport_unbind(jdns_session_t *, void *app, int handle)
{
    QJDns::Private *self = (QJDns::Private *)app;
    self->transport->close(handle);
}

int QJDns::Private::cb_transport_read(jdns_session_t *, void *app, int handle, jdns_address_t *addr, int *port, unsigned char *buf, int *bufsize)
{
    QJDns::Private *self = (QJDns::Private *)app;

    QJDnsDatagramTransport::Packet packet;
    if(!self->transport->takeResponse(handle, &packet))
        return 0;

    const int size = qMin(*bufsize, packet.data.size());
    if(size > 0)
        std::memcpy(buf, packet.data.constData(), static_cast<size_t>(size));

    if(!packet.sourceAddress.isNull())
        qt2addr_set(addr, packet.sourceAddress);
    *port = static_cast<int>(packet.sourcePort);
    *bufsize = size;
    return 1;
}

int QJDns::Private::cb_transport_write(jdns_session_t *, void *app, int handle, const jdns_address_t *addr, int port, unsigned char *buf, int bufsize)
{
    QJDns::Private *self = (QJDns::Private *)app;

    QHostAddress host;
    if(addr)
        host = addr2qt(addr);
    const QByteArray packet(reinterpret_cast<const char *>(buf), bufsize);
    const QJDnsDatagramTransport::SubmitResult result = self->transport->submit(handle, host, static_cast<quint16>(port), packet);
    if(result == QJDnsDatagramTransport::RetryLater)
        return 0;
    if(result == QJDnsDatagramTransport::Dropped)
        return 1;

    ++self->pending;
    return 1;
}

QJDns::QJDns(QObject *parent)
:QObject(parent)
{
    d = new Private(this);
}

QJDns::~QJDns()
{
    delete d;
}

QJDns *QJDns::create(QObject *parent)
{
    std::unique_ptr<QJDns> dns(new QJDns(parent));
    if(!dns->init(Unicast, QHostAddress::AnyIPv4))
        return nullptr;
    if(!dns->d->queryTransport && !dns->setNameServers(systemInfo().nameServers))
        return nullptr;
    return dns.release();
}

namespace {
bool validResolverUrl(const QUrl &url)
{
    if(!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
       || url.hasFragment() || url.port(53) <= 0 || url.port(53) > 65535)
        return false;
    if(url.scheme() == QLatin1String("https"))
        return true;
    return url.scheme() == QLatin1String("tcp") && url.path().isEmpty() && !url.hasQuery();
}
}

QJDns *QJDns::create(const QUrl &server, QObject *parent)
{
    return create(server, QNetworkProxy(QNetworkProxy::DefaultProxy), parent);
}

QJDns *QJDns::create(const QUrl &server, const QNetworkProxy &proxy, QObject *parent)
{
    if(!validResolverUrl(server))
        return nullptr;
    if(server.scheme() == QLatin1String("https"))
    {
        auto *transport = new QJDnsHttpTransport(server);
        transport->networkAccessManager()->setProxy(proxy);
        return create(transport, parent);
    }
    return create(new QJDnsTcpTransport(server.host(QUrl::FullyEncoded), quint16(server.port(53)), proxy), parent);
}

QJDns *QJDns::create(const QUrl &server, QNetworkAccessManager *manager, QObject *parent)
{
    if(!validResolverUrl(server) || server.scheme() != QLatin1String("https"))
        return nullptr;
    return create(new QJDnsHttpTransport(server, manager), parent);
}

QJDns *QJDns::create(QJDnsQueryTransport *transport, QObject *parent)
{
    if(!transport)
        return nullptr;
    Q_ASSERT(transport->thread() == QThread::currentThread());
    std::unique_ptr<QJDns> dns(new QJDns(parent));
    transport->setParent(dns->d);
    dns->d->queryTransport = transport;
    if(!dns->init(Unicast, QHostAddress::AnyIPv4))
        return nullptr;
    return dns.release();
}

bool QJDns::init(Mode mode, const QHostAddress &address)
{
    return d->init(mode, address);
}

void QJDns::shutdown()
{
    d->shutting_down = true;
    d->pending_wait = false;
    d->complete_shutdown = false;
    jdns_shutdown(d->sess);
    d->process();
}

QStringList QJDns::debugLines()
{
    QStringList tmp = d->debug_strings;
    d->debug_strings.clear();
    return tmp;
}

QJDns::SystemInfo QJDns::systemInfo()
{
    SystemInfo out;
    jdns_dnsparams_t *params = jdns_system_dnsparams();
    for(int n = 0; n < params->nameservers->count; ++n)
    {
        NameServer ns;
        ns.address = addr2qt(params->nameservers->item[n]->address);
        out.nameServers += ns;
    }
    for(int n = 0; n < params->domains->count; ++n)
        out.domains += str2qt(params->domains->item[n]);
    for(int n = 0; n < params->hosts->count; ++n)
    {
        DnsHost h;
        h.name = str2qt(params->hosts->item[n]->name);
        h.address = addr2qt(params->hosts->item[n]->address);
        out.hosts += h;
    }
    jdns_dnsparams_delete(params);
    return out;
}

#define PROBE_BASE  20000
#define PROBE_RANGE   100

QHostAddress QJDns::detectPrimaryMulticast(const QHostAddress &address)
{
    my_srand();

    QUdpSocket *sock = new QUdpSocket;
    QUdpSocket::BindMode mode;
    mode |= QUdpSocket::ShareAddress;
    mode |= QUdpSocket::ReuseAddressHint;
    int port = -1;
    for(int n = 0; n < PROBE_RANGE; ++n)
    {
        if(sock->bind(address, PROBE_BASE + n, mode))
        {
            port = PROBE_BASE + n;
            break;
        }
    }
    if(port == -1)
    {
        delete sock;
        return QHostAddress();
    }

    jdns_address_t *a;
    if(address.protocol() == QAbstractSocket::IPv6Protocol)
        a = jdns_address_multicast6_new();
    else
        a = jdns_address_multicast4_new();
    QHostAddress maddr = addr2qt(a);
    jdns_address_delete(a);

    if(address.protocol() == QAbstractSocket::IPv6Protocol)
    {
        int x;
        if(!qjdns_sock_setMulticast6(sock->socketDescriptor(), maddr.toIPv6Address().c, &x))
        {
            delete sock;
            return QHostAddress();
        }
        qjdns_sock_setTTL6(sock->socketDescriptor(), 0);
    }
    else
    {
        int x;
        if(!qjdns_sock_setMulticast4(sock->socketDescriptor(), maddr.toIPv4Address(), &x))
        {
            delete sock;
            return QHostAddress();
        }
        qjdns_sock_setTTL4(sock->socketDescriptor(), 0);
    }

    QHostAddress result;
    QByteArray out(128, 0);
    for(int n = 0; n < out.size(); ++n)
        out[n] = rand();
    if(sock->writeDatagram(out.data(), out.size(), maddr, port) == -1)
    {
        delete sock;
        return QHostAddress();
    }
    while(1)
    {
        if(!sock->waitForReadyRead(1000))
        {
            fprintf(stderr, "QJDns::detectPrimaryMulticast: timeout while checking %s\n", qPrintable(address.toString()));
            delete sock;
            return QHostAddress();
        }
        QByteArray in(128, 0);
        QHostAddress from_addr;
        quint16 from_port;
        int ret = sock->readDatagram(in.data(), in.size(), &from_addr, &from_port);
        if(ret == -1)
        {
            delete sock;
            return QHostAddress();
        }

        if(from_port != port)
            continue;
        in.resize(ret);
        if(in != out)
            continue;

        result = from_addr;
        break;
    }
    delete sock;

    return result;
}

bool QJDns::setNameServers(const QList<NameServer> &list)
{
    return d->setNameServers(list);
}

QJDnsSharedRequest *QJDns::query(const QByteArray &name, int type, QObject *parent)
{
    auto *request = new QJDnsSharedRequest(this, parent ? parent : this);
    request->query(name, d->sess && d->mode == Unicast ? type : 0);
    return request;
}

int QJDns::queryStart(const QByteArray &name, int type)
{
    if(!d->sess || d->shutting_down || name.isEmpty() || name.contains('\0') || type < 1 || type > 65535)
        return -1;
    int id = jdns_query(d->sess, (const unsigned char *)name.data(), type);
    d->process();
    return id;
}

void QJDns::queryCancel(int id)
{
    if(!d->sess || id < 0)
        return;
    jdns_cancel_query(d->sess, id);
    d->removeCancelled(id);
    d->process();
}

int QJDns::publishStart(PublishMode m, const Record &record)
{
    if(!d->sess || d->mode != Multicast || d->shutting_down || record.ttl <= 0 || !record.verify())
        return -1;
    jdns_rr_t *rr = export_record(record);

    int pubmode;
    if(m == QJDns::Unique)
        pubmode = JDNS_PUBLISH_UNIQUE;
    else
        pubmode = JDNS_PUBLISH_SHARED;

    int id = jdns_publish(d->sess, pubmode, rr);
    jdns_rr_delete(rr);
    d->process();
    return id;
}

void QJDns::publishUpdate(int id, const Record &record)
{
    if(!d->sess || d->mode != Multicast || d->shutting_down || id < 0 || record.ttl <= 0 || !record.verify())
        return;
    jdns_rr_t *rr = export_record(record);

    jdns_update_publish(d->sess, id, rr);
    jdns_rr_delete(rr);
    d->process();
}

void QJDns::publishCancel(int id)
{
    if(!d->sess || id < 0)
        return;
    jdns_cancel_publish(d->sess, id);
    d->removeCancelled(id);
    d->process();
}
