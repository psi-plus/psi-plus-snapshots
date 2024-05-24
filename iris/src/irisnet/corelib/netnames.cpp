/*
 * Copyright (C) 2006  Justin Karneges
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "netnames.h"

// #include "addressresolver.h"
#include "corelib/irisnetglobal_p.h"
#include "irisnetplugin.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <limits>

// #define NETNAMES_DEBUG
#ifdef NETNAMES_DEBUG
#define NNDEBUG (qDebug() << this << "#" << __FUNCTION__ << ":")
#endif

namespace XMPP {
//----------------------------------------------------------------------------
// NameRecord
//----------------------------------------------------------------------------
class NameRecord::Private : public QSharedData {
public:
    QString          owner; // de-ACE-ed version of domain name from dns reply
    NameRecord::Type type;
    int              ttl;

    QHostAddress      address;
    QByteArray        name;
    int               priority, weight, port;
    QList<QByteArray> texts;
    QByteArray        cpu, os;
    QByteArray        rawData;
};

#define ENSURE_D                                                                                                       \
    {                                                                                                                  \
        if (!d)                                                                                                        \
            d = new Private;                                                                                           \
    }

NameRecord::NameRecord() : d(nullptr) { }

NameRecord::NameRecord(const QString &owner, int ttl) : d(nullptr)
{
    setOwner(owner);
    setTtl(ttl);
}

NameRecord::NameRecord(const NameRecord &from) : d(nullptr) { *this = from; }

NameRecord::~NameRecord() { }

NameRecord &NameRecord::operator=(const NameRecord &from)
{
    d = from.d;
    return *this;
}

bool NameRecord::operator==(const NameRecord &o) const
{
    if (isNull() != o.isNull() || owner() != o.owner() || ttl() != o.ttl() || type() != o.type()) {
        return false;
    }

    switch (type()) {
    case XMPP::NameRecord::A:
    case XMPP::NameRecord::Aaaa:
        return address() == o.address();
    case XMPP::NameRecord::Mx:
        return name() == o.name() && priority() == o.priority();
    case XMPP::NameRecord::Srv:
        return name() == o.name() && port() == o.port() && priority() == o.priority() && weight() == o.weight();
    case XMPP::NameRecord::Cname:
    case XMPP::NameRecord::Ptr:
    case XMPP::NameRecord::Ns:
        return name() == o.name();
    case XMPP::NameRecord::Txt:
        return texts() == o.texts();
    case XMPP::NameRecord::Hinfo:
        return cpu() == o.cpu() && os() == o.os();
    case XMPP::NameRecord::Null:
        return rawData() == o.rawData();
    case XMPP::NameRecord::Any:
        return false;
    }

    return false;
}

bool NameRecord::isNull() const { return (d ? false : true); }

QString NameRecord::owner() const
{
    Q_ASSERT(d);
    return d->owner;
}

int NameRecord::ttl() const
{
    Q_ASSERT(d);
    return d->ttl;
}

NameRecord::Type NameRecord::type() const
{
    Q_ASSERT(d);
    return d->type;
}

QHostAddress NameRecord::address() const
{
    Q_ASSERT(d);
    return d->address;
}

QByteArray NameRecord::name() const
{
    Q_ASSERT(d);
    return d->name;
}

int NameRecord::priority() const
{
    Q_ASSERT(d);
    return d->priority;
}

int NameRecord::weight() const
{
    Q_ASSERT(d);
    return d->weight;
}

int NameRecord::port() const
{
    Q_ASSERT(d);
    return d->port;
}

QList<QByteArray> NameRecord::texts() const
{
    Q_ASSERT(d);
    return d->texts;
}

QByteArray NameRecord::cpu() const
{
    Q_ASSERT(d);
    return d->cpu;
}

QByteArray NameRecord::os() const
{
    Q_ASSERT(d);
    return d->os;
}

QByteArray NameRecord::rawData() const
{
    Q_ASSERT(d);
    return d->rawData;
}

void NameRecord::setOwner(const QString &name)
{
    ENSURE_D
    d->owner = name;
}

void NameRecord::setTtl(int seconds)
{
    ENSURE_D
    d->ttl = seconds;
}

void NameRecord::setAddress(const QHostAddress &a)
{
    ENSURE_D
    if (a.protocol() == QAbstractSocket::IPv6Protocol)
        d->type = NameRecord::Aaaa;
    else
        d->type = NameRecord::A;
    d->address = a;
}

void NameRecord::setMx(const QByteArray &name, int priority)
{
    ENSURE_D
    d->type     = NameRecord::Mx;
    d->name     = name;
    d->priority = priority;
}

void NameRecord::setSrv(const QByteArray &name, int port, int priority, int weight)
{
    ENSURE_D
    d->type     = NameRecord::Srv;
    d->name     = name;
    d->port     = port;
    d->priority = priority;
    d->weight   = weight;
}

void NameRecord::setCname(const QByteArray &name)
{
    ENSURE_D
    d->type = NameRecord::Cname;
    d->name = name;
}

void NameRecord::setPtr(const QByteArray &name)
{
    ENSURE_D
    d->type = NameRecord::Ptr;
    d->name = name;
}

void NameRecord::setTxt(const QList<QByteArray> &texts)
{
    ENSURE_D
    d->type  = NameRecord::Txt;
    d->texts = texts;
}

void NameRecord::setHinfo(const QByteArray &cpu, const QByteArray &os)
{
    ENSURE_D
    d->type = NameRecord::Hinfo;
    d->cpu  = cpu;
    d->os   = os;
}

void NameRecord::setNs(const QByteArray &name)
{
    ENSURE_D
    d->type = NameRecord::Ns;
    d->name = name;
}

void NameRecord::setNull(const QByteArray &rawData)
{
    ENSURE_D
    d->type    = NameRecord::Null;
    d->rawData = rawData;
}

QDebug operator<<(QDebug dbg, XMPP::NameRecord::Type type)
{
    dbg.nospace() << "XMPP::NameRecord::";

    switch (type) {
    case XMPP::NameRecord::A:
        dbg.nospace() << "A";
        break;
    case XMPP::NameRecord::Aaaa:
        dbg.nospace() << "Aaaa";
        break;
    case XMPP::NameRecord::Mx:
        dbg.nospace() << "Mx";
        break;
    case XMPP::NameRecord::Srv:
        dbg.nospace() << "Srv";
        break;
    case XMPP::NameRecord::Cname:
        dbg.nospace() << "Cname";
        break;
    case XMPP::NameRecord::Ptr:
        dbg.nospace() << "Ptr";
        break;
    case XMPP::NameRecord::Txt:
        dbg.nospace() << "Txt";
        break;
    case XMPP::NameRecord::Hinfo:
        dbg.nospace() << "Hinfo";
        break;
    case XMPP::NameRecord::Ns:
        dbg.nospace() << "Ns";
        break;
    case XMPP::NameRecord::Null:
        dbg.nospace() << "Null";
        break;
    case XMPP::NameRecord::Any:
        dbg.nospace() << "Any";
        break;
    }

    return dbg;
}

QDebug operator<<(QDebug dbg, const XMPP::NameRecord &record)
{
    dbg.nospace() << "XMPP::NameRecord("
                  << "owner=" << record.owner() << ", ttl=" << record.ttl() << ", type=" << record.type();

    switch (record.type()) {
    case XMPP::NameRecord::A:
    case XMPP::NameRecord::Aaaa:
        dbg.nospace() << ", address=" << record.address();
        break;
    case XMPP::NameRecord::Mx:
        dbg.nospace() << ", name=" << record.name() << ", priority=" << record.priority();
        break;
    case XMPP::NameRecord::Srv:
        dbg.nospace() << ", name=" << record.name() << ", port=" << record.port() << ", priority=" << record.priority()
                      << ", weight=" << record.weight();
        break;
    case XMPP::NameRecord::Cname:
    case XMPP::NameRecord::Ptr:
    case XMPP::NameRecord::Ns:
        dbg.nospace() << ", name=" << record.name();
        break;
    case XMPP::NameRecord::Txt:
        dbg.nospace() << ", texts={" << record.texts() << "}";
        break;
    case XMPP::NameRecord::Hinfo:
        dbg.nospace() << ", cpu=" << record.cpu() << ", os=" << record.os();
        break;
    case XMPP::NameRecord::Null:
        dbg.nospace() << ", size=" << record.rawData().size();
        break;
    case XMPP::NameRecord::Any:
        dbg.nospace() << ", <unknown>";
        // should not happen
        Q_ASSERT(false);
        break;
    }

    dbg.nospace() << ")";

    return dbg;
}

//----------------------------------------------------------------------------
// ServiceInstance
//----------------------------------------------------------------------------
class ServiceInstance::Private : public QSharedData {
public:
    QString                   instance, type, domain;
    QMap<QString, QByteArray> attribs;
    QByteArray                name;
};

ServiceInstance::ServiceInstance() : d(new Private) { }

ServiceInstance::ServiceInstance(const QString &instance, const QString &type, const QString &domain,
                                 const QMap<QString, QByteArray> &attribs) : d(new Private)
{
    d->instance = instance;
    d->type     = type;
    d->domain   = domain;
    d->attribs  = attribs;

    // FIXME: escape the items
    d->name = instance.toLatin1() + '.' + type.toLatin1() + '.' + domain.toLatin1();
}

ServiceInstance::ServiceInstance(const ServiceInstance &from) : d(nullptr) { *this = from; }

ServiceInstance::~ServiceInstance() { }

ServiceInstance &ServiceInstance::operator=(const ServiceInstance &from)
{
    d = from.d;
    return *this;
}

QString ServiceInstance::instance() const { return d->instance; }

QString ServiceInstance::type() const { return d->type; }

QString ServiceInstance::domain() const { return d->domain; }

QMap<QString, QByteArray> ServiceInstance::attributes() const { return d->attribs; }

QByteArray ServiceInstance::name() const { return d->name; }

//----------------------------------------------------------------------------
// NameManager
//----------------------------------------------------------------------------
class NameManager;

Q_GLOBAL_STATIC(QMutex, nman_mutex)
static NameManager *g_nman = nullptr;

class NameResolver::Private {
public:
    NameResolver *q;

    int  type;
    bool longLived;
    int  id;

    Private(NameResolver *_q) : q(_q) { }
};

class ServiceBrowser::Private {
public:
    ServiceBrowser *q;

    int id;

    Private(ServiceBrowser *_q) : q(_q) { }
};

class ServiceResolver::Private : public QObject {
    Q_OBJECT
public:
    Private(ServiceResolver *parent) :
        q(parent), dns_sd_resolve_id(0), requestedProtocol(IPv6_IPv4), port(0), protocol(QAbstractSocket::IPv6Protocol)
    {
    }

    /* DNS-SD interaction with NameManager */
    ServiceResolver *q;                 //!< Pointing upwards, so NameManager can call its signals
    int              dns_sd_resolve_id; //!< DNS-SD lookup id, set by NameManager

    /* configuration */
    Protocol requestedProtocol; //!< IP protocol requested by user

    /* state trackers */
    QString                               domain;   //!< Domain we are currently looking up
    QString                               host;     //!< Hostname we are currently looking up
    QHostAddress                          address;  //!< IP address we are currently looking up
    quint16                               port;     //!< Port we are currently looking up
    QAbstractSocket::NetworkLayerProtocol protocol; //!< IP protocol we are currently looking up

    XMPP::WeightedNameRecordList srvList;      //!< List of resolved SRV names
    QList<ServiceBoundRecord>    hostList;     //!< List or resolved hostnames for current SRV name
    QList<XMPP::NameResolver *>  resolverList; //!< NameResolvers currently in use, needed for cleanup
};

WeightedNameRecordList::WeightedNameRecordList() : currentPriorityGroup(priorityGroups.end()) /* void current state */
{
}

WeightedNameRecordList::WeightedNameRecordList(const QList<ServiceBoundRecord> &list) { append(list); }

WeightedNameRecordList::WeightedNameRecordList(const WeightedNameRecordList &other) { *this = other; }

WeightedNameRecordList &WeightedNameRecordList::operator=(const WeightedNameRecordList &other)
{
    priorityGroups = other.priorityGroups;
    if (other.currentPriorityGroup != other.priorityGroups.end()) {
        currentPriorityGroup = priorityGroups.find(other.currentPriorityGroup->first);
    } else {
        currentPriorityGroup = priorityGroups.end();
    }
    return *this;
}

WeightedNameRecordList::~WeightedNameRecordList() { }

bool WeightedNameRecordList::isEmpty() const
{
    return currentPriorityGroup == const_cast<WeightedNameRecordList *>(this)->priorityGroups.end();
}

ServiceBoundRecord WeightedNameRecordList::takeNext()
{
    /* Find the next useful priority group */
    while (currentPriorityGroup != priorityGroups.end() && currentPriorityGroup->second.empty()) {
        ++currentPriorityGroup;
    }
    /* There are no priority groups left, return failure */
    if (currentPriorityGroup == priorityGroups.end()) {
#ifdef NETNAMES_DEBUG
        NNDEBUG << "No more SRV records left";
#endif
        return {};
    }

    /* Find the new total weight of this priority group */
    int totalWeight = 0;
    for (const auto &record : std::as_const(currentPriorityGroup->second)) {
        totalWeight += record.record.weight();
    }

#ifdef NETNAMES_DEBUG
    NNDEBUG << "Total weight:" << totalWeight;
#endif

    /* Pick a random entry */
    int randomWeight = totalWeight ? QRandomGenerator::global()->bounded(totalWeight) : 0;

#ifdef NETNAMES_DEBUG
    NNDEBUG << "Picked weight:" << randomWeight;
#endif

    /* Iterate through the priority group until we found the randomly selected entry */
    WeightedNameRecordPriorityGroup::iterator it(currentPriorityGroup->second.begin());
    for (int currentWeight = it->record.weight(); currentWeight < randomWeight;
         currentWeight += (++it)->record.weight()) { }
    Q_ASSERT(it != currentPriorityGroup->second.end());

    /* We are going to delete the entry in the list, so save it */
    auto result { *it };

#ifdef NETNAMES_DEBUG
    NNDEBUG << "Picked record:" << result;
#endif

    /* Delete the entry from list, to prevent it from being tried multiple times */
    currentPriorityGroup->second.remove(it->record.weight(), *it);
    if (currentPriorityGroup->second.isEmpty()) {
        currentPriorityGroup = priorityGroups.erase(currentPriorityGroup);
    }

    return result;
}

void WeightedNameRecordList::clear()
{
    priorityGroups.clear();

    /* void current state */
    currentPriorityGroup = priorityGroups.end();
}

void WeightedNameRecordList::append(const XMPP::WeightedNameRecordList &list)
{
    /* Copy over all records from all groups */
    for (const auto &group : list.priorityGroups) {
        for (const auto &record : group.second)
            append(record);
    }

    /* Reset to beginning */
    currentPriorityGroup = priorityGroups.begin();
}

void WeightedNameRecordList::append(const QList<ServiceBoundRecord> &list)
{
    for (const auto &record : list)
        if (record.record.type() == XMPP::NameRecord::Srv)
            append(record);

    /* Reset to beginning */
    currentPriorityGroup = priorityGroups.begin();
}

void WeightedNameRecordList::append(const ServiceBoundRecord &record)
{
    Q_ASSERT(record.record.type() == XMPP::NameRecord::Srv);
    auto [it, _] = priorityGroups.try_emplace(record.record.priority(), WeightedNameRecordPriorityGroup {});
    it->second.insert(record.record.weight(), record);

    /* Reset to beginning */
    currentPriorityGroup = priorityGroups.begin();
}

void WeightedNameRecordList::append(const QString &hostname, quint16 port)
{
    NameRecord record(hostname.toLocal8Bit(), std::numeric_limits<int>::max());
    record.setSrv(hostname.toLocal8Bit(), port, std::numeric_limits<int>::max(), 0);

    append(ServiceBoundRecord { {}, record });

    /* Reset to beginning */
    currentPriorityGroup = priorityGroups.begin();
}

XMPP::WeightedNameRecordList &WeightedNameRecordList::operator<<(const XMPP::WeightedNameRecordList &list)
{
    append(list);
    return *this;
}

WeightedNameRecordList &WeightedNameRecordList::operator<<(const QList<ServiceBoundRecord> &list)
{
    append(list);
    return *this;
}

XMPP::WeightedNameRecordList &WeightedNameRecordList::operator<<(const ServiceBoundRecord &record)
{
    append(record);
    return *this;
}

QDebug operator<<(QDebug dbg, const ServiceBoundRecord &r)
{
    dbg.nospace() << "XMPP::ServiceBoundRecor(\n";
    dbg.nospace() << "service=" << r.service
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                  << Qt::endl;
#else
                  << endl;
#endif
    dbg.nospace() << "record=" << r.record
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                  << Qt::endl;
#else
                  << endl;
#endif
    dbg.nospace() << "})";
    return dbg;
}

QDebug operator<<(QDebug dbg, const XMPP::WeightedNameRecordList &list)
{
    dbg.nospace() << "XMPP::WeightedNameRecordList(\n";

    /* operator(QDebug, QMap const&) has a bug which makes it crash when trying to print the dereferenced end() iterator
     */
    if (!list.isEmpty()) {
        dbg.nospace() << "current=" << *list.currentPriorityGroup
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                      << Qt::endl;
#else
                      << endl;
#endif
    }

    dbg.nospace() << "{";

    for (const auto &[priority, group] : list.priorityGroups) {
        dbg.nospace() << "\t" << priority << "->" << group
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                      << Qt::endl;
#else
                      << endl;
#endif
    }

    dbg.nospace() << "})";
    return dbg;
}

class ServiceLocalPublisher::Private {
public:
    ServiceLocalPublisher *q;

    int id;

    Private(ServiceLocalPublisher *_q) : q(_q) { }
};

class NameManager : public QObject {
    Q_OBJECT
public:
    NameProvider                       *p_net, *p_local;
    ServiceProvider                    *p_serv;
    QHash<int, NameResolver::Private *> res_instances;
    QHash<int, int>                     res_sub_instances;

    QHash<int, ServiceBrowser::Private *>        br_instances;
    QHash<int, ServiceResolver::Private *>       sres_instances;
    QHash<int, ServiceLocalPublisher::Private *> slp_instances;

    NameManager(QObject *parent = nullptr) : QObject(parent)
    {
        p_net   = nullptr;
        p_local = nullptr;
        p_serv  = 0;
    }

    ~NameManager()
    {
        delete p_net;
        delete p_local;
        delete p_serv;
    }

    static NameManager *instance()
    {
        QMutexLocker locker(nman_mutex());
        if (!g_nman) {
            g_nman = new NameManager;
            irisNetAddPostRoutine(NetNames::cleanup);
        }
        return g_nman;
    }

    static void cleanup()
    {
        delete g_nman;
        g_nman = nullptr;
    }

    void resolve_start(NameResolver::Private *np, const QByteArray &name, int qType, bool longLived)
    {
        QMutexLocker locker(nman_mutex());

        np->type      = qType;
        np->longLived = longLived;
        if (!p_net) {
            NameProvider            *c    = 0;
            QList<IrisNetProvider *> list = irisNetProviders();
            for (int n = 0; n < list.count(); ++n) {
                IrisNetProvider *p = list[n];
                c                  = p->createNameProviderInternet();
                if (c)
                    break;
            }
            Q_ASSERT(c); // we have built-in support, so this should never fail
            p_net = c;

            // use queued connections
            qRegisterMetaType<QList<XMPP::NameRecord>>("QList<XMPP::NameRecord>");
            qRegisterMetaType<XMPP::NameResolver::Error>("XMPP::NameResolver::Error");
            connect(p_net, &NameProvider::resolve_resultsReady, this,
                    [this](int id, const QList<XMPP::NameRecord> &results) {
                        NameResolver::Private *np = res_instances.value(id);
                        NameResolver          *q  = np->q; // resolve_cleanup deletes np
                        if (!np->longLived)
                            resolve_cleanup(np);
                        emit q->resultsReady(results);
                    });
            connect(p_net, SIGNAL(resolve_error(int, XMPP::NameResolver::Error)),
                    SLOT(provider_resolve_error(int, XMPP::NameResolver::Error)));
            connect(p_net, SIGNAL(resolve_useLocal(int, QByteArray)), SLOT(provider_resolve_useLocal(int, QByteArray)));
        }

        np->id = p_net->resolve_start(name, qType, longLived);

        // printf("assigning %d to %p\n", req_id, np);
        res_instances.insert(np->id, np);
    }

    void resolve_stop(NameResolver::Private *np)
    {
        // FIXME: stop sub instances?
        p_net->resolve_stop(np->id);
        resolve_cleanup(np);
    }

    void resolve_cleanup(NameResolver::Private *np)
    {
        // clean up any sub instances

        QList<int>              sub_instances_to_remove;
        QHashIterator<int, int> it(res_sub_instances);
        while (it.hasNext()) {
            it.next();
            if (it.value() == np->id)
                sub_instances_to_remove += it.key();
        }

        for (int res_sub_id : std::as_const(sub_instances_to_remove)) {
            res_sub_instances.remove(res_sub_id);
            p_local->resolve_stop(res_sub_id);
        }

        // clean up primary instance

        res_instances.remove(np->id);
        NameResolver *q = np->q;
        delete q->d;
        q->d = nullptr;
    }

    void browse_start(ServiceBrowser::Private *np, const QString &type, const QString &domain)
    {
        QMutexLocker locker(nman_mutex());

        if (!p_serv) {
            ServiceProvider         *c    = nullptr;
            QList<IrisNetProvider *> list = irisNetProviders();
            for (int n = 0; n < list.count(); ++n) {
                IrisNetProvider *p = list[n];
                c                  = p->createServiceProvider();
                if (c)
                    break;
            }
            Q_ASSERT(c); // we have built-in support, so this should never fail
            p_serv = c;

            // use queued connections
            qRegisterMetaType<XMPP::ServiceInstance>("XMPP::ServiceInstance");
            qRegisterMetaType<XMPP::ServiceBrowser::Error>("XMPP::ServiceBrowser::Error");

            connect(p_serv, SIGNAL(browse_instanceAvailable(int, XMPP::ServiceInstance)),
                    SLOT(provider_browse_instanceAvailable(int, XMPP::ServiceInstance)), Qt::QueuedConnection);
            connect(p_serv, SIGNAL(browse_instanceUnavailable(int, XMPP::ServiceInstance)),
                    SLOT(provider_browse_instanceUnavailable(int, XMPP::ServiceInstance)), Qt::QueuedConnection);
            connect(p_serv, SIGNAL(browse_error(int, XMPP::ServiceBrowser::Error)),
                    SLOT(provider_browse_error(int, XMPP::ServiceBrowser::Error)), Qt::QueuedConnection);
        }

        /*np->id = */

        np->id = p_serv->browse_start(type, domain);

        br_instances.insert(np->id, np);
    }

    void resolve_instance_start(ServiceResolver::Private *np, const QByteArray &name)
    {
        QMutexLocker locker(nman_mutex());

        if (!p_serv) {
            ServiceProvider         *c    = nullptr;
            QList<IrisNetProvider *> list = irisNetProviders();
            for (int n = 0; n < list.count(); ++n) {
                IrisNetProvider *p = list[n];
                c                  = p->createServiceProvider();
                if (c)
                    break;
            }
            Q_ASSERT(c); // we have built-in support, so this should never fail
            p_serv = c;

            // use queued connections
            qRegisterMetaType<QList<XMPP::ServiceProvider::ResolveResult>>(
                "QList<XMPP::ServiceProvider::ResolveResult>");
            connect(
                p_serv, &ServiceProvider::resolve_resultsReady, this,
                [this](int id, const QList<XMPP::ServiceProvider::ResolveResult> &results) {
                    ServiceResolver::Private *np = sres_instances.value(id);
                    emit np->q->resultReady(results[0].address, quint16(results[0].port), results[0].hostName, {});
                },
                Qt::QueuedConnection);
        }

        /* store the id so we can stop it later */
        np->dns_sd_resolve_id = p_serv->resolve_start(name);

        sres_instances.insert(np->dns_sd_resolve_id, np);
    }

    void publish_start(ServiceLocalPublisher::Private *np, const QString &instance, const QString &type, int port,
                       const QMap<QString, QByteArray> &attribs)
    {
        QMutexLocker locker(nman_mutex());

        if (!p_serv) {
            ServiceProvider         *c    = nullptr;
            QList<IrisNetProvider *> list = irisNetProviders();
            for (int n = 0; n < list.count(); ++n) {
                IrisNetProvider *p = list[n];
                c                  = p->createServiceProvider();
                if (c)
                    break;
            }
            Q_ASSERT(c); // we have built-in support, so this should never fail
            p_serv = c;

            // use queued connections
            qRegisterMetaType<XMPP::ServiceLocalPublisher::Error>("XMPP::ServiceLocalPublisher::Error");
            connect(p_serv, SIGNAL(publish_published(int)), SLOT(provider_publish_published(int)),
                    Qt::QueuedConnection);
            connect(p_serv, SIGNAL(publish_extra_published(int)), SLOT(provider_publish_extra_published(int)),
                    Qt::QueuedConnection);
        }

        /*np->id = */

        np->id = p_serv->publish_start(instance, type, port, attribs);

        slp_instances.insert(np->id, np);
    }

    void publish_extra_start(ServiceLocalPublisher::Private *np, const NameRecord &rec)
    {
        np->id = p_serv->publish_extra_start(np->id, rec);
    }

private slots:

    void provider_resolve_error(int id, XMPP::NameResolver::Error e)
    {
        NameResolver::Private *np = res_instances.value(id);
        NameResolver          *q  = np->q; // resolve_cleanup deletes np
        resolve_cleanup(np);
        emit q->error(e);
    }

    void provider_local_resolve_resultsReady(int id, const QList<XMPP::NameRecord> &results)
    {
        int                    par_id = res_sub_instances.value(id);
        NameResolver::Private *np     = res_instances.value(par_id);
        if (!np->longLived)
            res_sub_instances.remove(id);
        p_net->resolve_localResultsReady(par_id, results);
    }

    void provider_local_resolve_error(int id, XMPP::NameResolver::Error e)
    {
        int par_id = res_sub_instances.value(id);
        res_sub_instances.remove(id);
        p_net->resolve_localError(par_id, e);
    }

    void provider_resolve_useLocal(int id, const QByteArray &name)
    {
        // transfer to local
        if (!p_local) {
            NameProvider            *c    = nullptr;
            QList<IrisNetProvider *> list = irisNetProviders();
            for (int n = 0; n < list.count(); ++n) {
                IrisNetProvider *p = list[n];
                c                  = p->createNameProviderLocal();
                if (c)
                    break;
            }
            Q_ASSERT(c); // we have built-in support, so this should never fail
            // FIXME: not true, binding can fail
            p_local = c;

            // use queued connections
            qRegisterMetaType<QList<XMPP::NameRecord>>("QList<XMPP::NameRecord>");
            qRegisterMetaType<XMPP::NameResolver::Error>("XMPP::NameResolver::Error");
            connect(p_local, SIGNAL(resolve_resultsReady(int, QList<XMPP::NameRecord>)),
                    SLOT(provider_local_resolve_resultsReady(int, QList<XMPP::NameRecord>)), Qt::QueuedConnection);
            connect(p_local, SIGNAL(resolve_error(int, XMPP::NameResolver::Error)),
                    SLOT(provider_local_resolve_error(int, XMPP::NameResolver::Error)), Qt::QueuedConnection);
        }

        NameResolver::Private *np = res_instances.value(id);

        /*// transfer to local only
        if(np->longLived)
        {
            res_instances.remove(np->id);

            np->id = p_local->resolve_start(name, np->type, true);
            res_instances.insert(np->id, np);
        }
        // sub request
        else
        {
            int req_id = p_local->resolve_start(name, np->type, false);

            res_sub_instances.insert(req_id, np->id);
        }*/

        int req_id = p_local->resolve_start(name, np->type, np->longLived);
        res_sub_instances.insert(req_id, np->id);
    }

    void provider_browse_instanceAvailable(int id, const XMPP::ServiceInstance &i)
    {
        ServiceBrowser::Private *np = br_instances.value(id);
        emit                     np->q->instanceAvailable(i);
    }

    void provider_browse_instanceUnavailable(int id, const XMPP::ServiceInstance &i)
    {
        ServiceBrowser::Private *np = br_instances.value(id);
        emit                     np->q->instanceUnavailable(i);
    }

    void provider_browse_error(int id, XMPP::ServiceBrowser::Error e)
    {
        Q_UNUSED(e);
        ServiceBrowser::Private *np = br_instances.value(id);
        // TODO
        emit np->q->error();
    }

    void provider_publish_published(int id)
    {
        ServiceLocalPublisher::Private *np = slp_instances.value(id);
        emit                            np->q->published();
    }

    void provider_publish_extra_published(int id)
    {
        Q_UNUSED(id);
        // ServiceLocalPublisher::Private *np = slp_instances.value(id);
        // emit np->q->published();
    }
};

//----------------------------------------------------------------------------
// NameResolver
//----------------------------------------------------------------------------

// copied from JDNS
#define JDNS_RTYPE_A 1
#define JDNS_RTYPE_AAAA 28
#define JDNS_RTYPE_MX 15
#define JDNS_RTYPE_SRV 33
#define JDNS_RTYPE_CNAME 5
#define JDNS_RTYPE_PTR 12
#define JDNS_RTYPE_TXT 16
#define JDNS_RTYPE_HINFO 13
#define JDNS_RTYPE_NS 2
#define JDNS_RTYPE_ANY 255

static int recordType2Rtype(NameRecord::Type type)
{
    switch (type) {
    case NameRecord::A:
        return JDNS_RTYPE_A;
    case NameRecord::Aaaa:
        return JDNS_RTYPE_AAAA;
    case NameRecord::Mx:
        return JDNS_RTYPE_MX;
    case NameRecord::Srv:
        return JDNS_RTYPE_SRV;
    case NameRecord::Cname:
        return JDNS_RTYPE_CNAME;
    case NameRecord::Ptr:
        return JDNS_RTYPE_PTR;
    case NameRecord::Txt:
        return JDNS_RTYPE_TXT;
    case NameRecord::Hinfo:
        return JDNS_RTYPE_HINFO;
    case NameRecord::Ns:
        return JDNS_RTYPE_NS;
    case NameRecord::Null:
        return 10;
    case NameRecord::Any:
        return JDNS_RTYPE_ANY;
    }
    return -1;
}

NameResolver::NameResolver(QObject *parent) : QObject(parent) { d = nullptr; }

NameResolver::~NameResolver() { stop(); }

void NameResolver::start(const QByteArray &name, NameRecord::Type type, Mode mode)
{
    stop();
    d         = new Private(this);
    int qType = recordType2Rtype(type);
    if (qType == -1)
        qType = JDNS_RTYPE_A;
    NameManager::instance()->resolve_start(d, name, qType, mode == NameResolver::LongLived);
}

void NameResolver::stop()
{
    if (d) {
        NameManager::instance()->resolve_stop(d);
        delete d;
        d = nullptr;
    }
}

QDebug operator<<(QDebug dbg, XMPP::NameResolver::Error e)
{
    dbg.nospace() << "XMPP::NameResolver::";

    switch (e) {
    case XMPP::NameResolver::ErrorGeneric:
        dbg.nospace() << "ErrorGeneric";
        break;
    case XMPP::NameResolver::ErrorNoName:
        dbg.nospace() << "ErrorNoName";
        break;
    case XMPP::NameResolver::ErrorTimeout:
        dbg.nospace() << "ErrorTimeout";
        break;
    case XMPP::NameResolver::ErrorNoLocal:
        dbg.nospace() << "ErrorNoLocal";
        break;
    case XMPP::NameResolver::ErrorNoLongLived:
        dbg.nospace() << "ErrorNoLongLived";
        break;
    }

    return dbg;
}

//----------------------------------------------------------------------------
// ServiceBrowser
//----------------------------------------------------------------------------
ServiceBrowser::ServiceBrowser(QObject *parent) : QObject(parent) { d = new Private(this); }

ServiceBrowser::~ServiceBrowser() { delete d; }

void ServiceBrowser::start(const QString &type, const QString &domain)
{
    NameManager::instance()->browse_start(d, type, domain);
}

void ServiceBrowser::stop() { }

//----------------------------------------------------------------------------
// ServiceResolver
//----------------------------------------------------------------------------
ServiceResolver::ServiceResolver(QObject *parent) : QObject(parent)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG;
#endif

    d = new Private(this);
}

ServiceResolver::~ServiceResolver() { delete d; }

void ServiceResolver::clear_resolvers()
{
#ifdef NETNAMES_DEBUG
    NNDEBUG;
#endif

    /* cleanup all resolvers */
    for (XMPP::NameResolver *resolver : std::as_const(d->resolverList)) {
        cleanup_resolver(resolver);
    }
}

void ServiceResolver::cleanup_resolver(XMPP::NameResolver *resolver)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "r:" << resolver;
#endif

    if (resolver) {
        /*
        do not just "delete", because we might have been called from a slot
        that was invoked by the resolver, and we do not want to create a mess
        there.
        */
        disconnect(resolver);
        resolver->stop();
        resolver->deleteLater();

        d->resolverList.removeAll(resolver);
    }
}

ServiceResolver::Protocol ServiceResolver::protocol() const { return d->requestedProtocol; }

void ServiceResolver::setProtocol(ServiceResolver::Protocol p) { d->requestedProtocol = p; }

/* DNS-SD lookup */
void ServiceResolver::start(const QByteArray &name) { NameManager::instance()->resolve_instance_start(d, name); }

/* normal host lookup */
void ServiceResolver::start(const QString &host, quint16 port, const QString &service)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "h:" << host << "p:" << port;
#endif

    /* clear host list */
    d->hostList.clear();

    d->protocol = (d->requestedProtocol == IPv6_IPv4 || d->requestedProtocol == IPv6 ? QAbstractSocket::IPv6Protocol
                                                                                     : QAbstractSocket::IPv4Protocol);
    d->host     = host;
    d->port     = port;

#ifdef NETNAMES_DEBUG
    NNDEBUG << "d->p:" << d->protocol;
#endif

    /* initiate the host lookup */
    XMPP::NameRecord::Type querytype
        = (d->protocol == QAbstractSocket::IPv6Protocol ? XMPP::NameRecord::Aaaa : XMPP::NameRecord::A);
    XMPP::NameResolver *resolver = new XMPP::NameResolver;
    connect(resolver, &XMPP::NameResolver::resultsReady, this,
            [this, service](const QList<XMPP::NameRecord> records) { handle_host_ready(service, records); });
    connect(resolver, SIGNAL(error(XMPP::NameResolver::Error)), this,
            SLOT(handle_host_error(XMPP::NameResolver::Error)));
    resolver->start(host.toLocal8Bit(), querytype);
    d->resolverList << resolver;
}

/* SRV lookup */
void ServiceResolver::start(const QStringList &services, const QString &transport, const QString &domain, int port)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "s:" << services << "t:" << transport << "d:" << domain << "p:" << port;
#endif
    /* clear SRV list */
    d->srvList.clear();
    d->domain = domain;

    /* after we tried all SRV hosts, we shall connect directly (if requested) */
    if (port < std::numeric_limits<quint16>::max()) {
        d->srvList.append(domain.toLocal8Bit(), quint16(port));
    } else {
        /* The only "valid" port above the valid port range is our specification of an invalid port */
        Q_ASSERT(port == std::numeric_limits<int>::max());
    }

    struct SrvStats {
        std::function<void(bool)> callback;
        int                       counter = 0;
        int                       success = 0;
        SrvStats(std::function<void(bool)> &&cb, int cnt) : callback(std::move(cb)), counter(cnt) { }
        void finishOne(bool success)
        {
            if (success)
                this->success++;
            if (--counter == 0)
                callback(this->success > 0);
        }
    };

    auto stats = std::make_shared<SrvStats>(
        [this](bool success) {
            if (success) {
                emit srvReady();
            } else {
                /* srvList already contains a failsafe host, try that */
                emit srvFailed();
            }
            if (d->requestedProtocol != HappyEyeballs) {
                try_next_srv();
            }
        },
        services.size());

    for (auto const &service : services) {
        QString srv_request("_" + service + "._" + transport + "." + domain + ".");

        /* initiate the SRV lookup */
        auto resolver = new XMPP::NameResolver;
        connect(resolver, &XMPP::NameResolver::resultsReady, this,
                [this, resolver, stats, service](const QList<XMPP::NameRecord> &r) {
#ifdef NETNAMES_DEBUG
                    NNDEBUG << "sl:" << r;
#endif
                    QList<ServiceBoundRecord> sbr;
                    std::transform(r.begin(), r.end(), std::back_inserter(sbr),
                                   [service](auto const &r) { return ServiceBoundRecord { service, r }; });
                    d->srvList << sbr;
                    stats->finishOne(true);
                    cleanup_resolver(resolver);
                });
        connect(resolver, &XMPP::NameResolver::error, this, [this, resolver, stats](XMPP::NameResolver::Error e) {
        /* failed the srv lookup, but we might have a fallback host in the srvList */
#ifdef NETNAMES_DEBUG
            NNDEBUG << "e:" << e;
#else
            Q_UNUSED(e)
#endif
            stats->finishOne(false);
            cleanup_resolver(resolver);
        });
        resolver->start(srv_request.toLocal8Bit(), XMPP::NameRecord::Srv);
        d->resolverList << resolver;
    }
}

/* hosts resolved, now try to connect to them */
void ServiceResolver::handle_host_ready(const QString &service, const QList<XMPP::NameRecord> &rl)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "hl:" << rl;
#endif

    /* cleanup resolver */
    cleanup_resolver(static_cast<XMPP::NameResolver *>(sender()));

    /* connect to host */
    for (auto const &r : rl) {
        d->hostList << ServiceBoundRecord { service, r };
    }
    try_next_host();
}

/* failed to lookup the primary record (A or AAAA, depending on user choice) */
void ServiceResolver::handle_host_error(XMPP::NameResolver::Error e)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "e:" << e;
#endif

    /* cleanup resolver */
    cleanup_resolver(static_cast<XMPP::NameResolver *>(sender()));

    /* try a fallback lookup if requested*/
    if (!lookup_host_fallback()) {
        /* no-fallback should behave the same as a failed fallback */
        handle_host_fallback_error(e);
    }
}

/* failed to lookup the fallback record (A or AAAA, depending on user choice) */
void ServiceResolver::handle_host_fallback_error(XMPP::NameResolver::Error e)
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "e:" << e;
#else
    Q_UNUSED(e)
#endif

    /* cleanup resolver */
    cleanup_resolver(static_cast<XMPP::NameResolver *>(sender()));

    /* lookup next SRV */
    try_next_srv();
}

/* check whether a fallback is needed in the current situation */
bool ServiceResolver::check_protocol_fallback()
{
    return (d->requestedProtocol == IPv6_IPv4 && d->protocol == QAbstractSocket::IPv6Protocol)
        || (d->requestedProtocol == IPv4_IPv6 && d->protocol == QAbstractSocket::IPv4Protocol);
}

/* lookup the fallback host */
bool ServiceResolver::lookup_host_fallback()
{
#ifdef NETNAMES_DEBUG
    NNDEBUG;
#endif

    /* if a fallback is desired, otherwise we must fail immediately */
    if (!check_protocol_fallback()) {
        return false;
    }

    d->protocol = (d->protocol == QAbstractSocket::IPv6Protocol ? QAbstractSocket::IPv4Protocol
                                                                : QAbstractSocket::IPv6Protocol);

#ifdef NETNAMES_DEBUG
    NNDEBUG << "d->p:" << d->protocol;
#endif

    /* initiate the fallback host lookup */
    XMPP::NameRecord::Type querytype
        = (d->protocol == QAbstractSocket::IPv6Protocol ? XMPP::NameRecord::Aaaa : XMPP::NameRecord::A);
    XMPP::NameResolver *resolver = new XMPP::NameResolver;
    connect(resolver, SIGNAL(resultsReady(QList<XMPP::NameRecord>)), this,
            SLOT(handle_host_ready(QList<XMPP::NameRecord>)));
    connect(resolver, SIGNAL(error(XMPP::NameResolver::Error)), this,
            SLOT(handle_host_fallback_error(XMPP::NameResolver::Error)));
    resolver->start(d->host.toLocal8Bit(), querytype);
    d->resolverList << resolver;

    return true;
}

/* notify user about next host */
bool ServiceResolver::try_next_host()
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "hl:" << d->hostList;
#endif

    /* if there is a host left for current protocol (AAAA or A) */
    if (!d->hostList.empty()) {
        auto record { d->hostList.takeFirst() };
        /* emit found address and the port specified earlier */
        emit resultReady(record.record.address(), d->port, record.record.owner(), record.service);
        return true;
    }

    /* otherwise try the fallback protocol */
    return lookup_host_fallback();
}

/* lookup the next SRV record in line */
void ServiceResolver::try_next_srv()
{
#ifdef NETNAMES_DEBUG
    NNDEBUG << "sl:" << d->srvList;
#endif

    /* if there are still hosts we did not try */
    auto record = d->srvList.takeNext();
    if (!record.record.isNull()) {
        /* lookup host by name and specify port for later use */
        start(record.record.name(), quint16(record.record.port()), record.service);
    } else {
#ifdef NETNAMES_DEBUG
        NNDEBUG << "SRV list empty, failing";
#endif
        /* no more SRV hosts to try, fail */
        emit error(NoHostLeft);
    }
}

void ServiceResolver::tryNext()
{
    /* if the host list cannot help, try the SRV list */
    if (!try_next_host()) {
        try_next_srv();
    }
}

void ServiceResolver::stop() { clear_resolvers(); }

bool ServiceResolver::hasPendingSrv() const { return !d->srvList.isEmpty(); }

ServiceResolver::ProtoSplit ServiceResolver::happySplit()
{
    Q_ASSERT(d->requestedProtocol == HappyEyeballs);
    ProtoSplit s;
    s.ipv4 = new ServiceResolver(this);
    s.ipv4->setProtocol(IPv4);
    s.ipv4->d->srvList  = d->srvList;
    s.ipv4->d->hostList = d->hostList;
    s.ipv4->d->domain   = d->domain;
    s.ipv4->d->host     = d->host;
    s.ipv4->d->port     = d->port;
    s.ipv6              = new ServiceResolver(this);
    s.ipv6->setProtocol(IPv6);
    s.ipv6->d->srvList  = d->srvList;
    s.ipv6->d->hostList = d->hostList;
    s.ipv6->d->domain   = d->domain;
    s.ipv4->d->host     = d->host;
    s.ipv4->d->port     = d->port;
    return s;
}

//----------------------------------------------------------------------------
// ServiceLocalPublisher
//----------------------------------------------------------------------------
ServiceLocalPublisher::ServiceLocalPublisher(QObject *parent) : QObject(parent) { d = new Private(this); }

ServiceLocalPublisher::~ServiceLocalPublisher() { delete d; }

void ServiceLocalPublisher::publish(const QString &instance, const QString &type, int port,
                                    const QMap<QString, QByteArray> &attributes)
{
    NameManager::instance()->publish_start(d, instance, type, port, attributes);
}

void ServiceLocalPublisher::updateAttributes(const QMap<QString, QByteArray> &attributes) { Q_UNUSED(attributes); }

void ServiceLocalPublisher::addRecord(const NameRecord &rec) { NameManager::instance()->publish_extra_start(d, rec); }

void ServiceLocalPublisher::cancel() { }

//----------------------------------------------------------------------------
// NetNames
//----------------------------------------------------------------------------
void NetNames::cleanup() { NameManager::cleanup(); }

QString NetNames::diagnosticText()
{
    // TODO
    return QString();
}

QByteArray NetNames::idnaFromString(const QString &in)
{
    // TODO
    Q_UNUSED(in);
    return QByteArray();
}

QString NetNames::idnaToString(const QByteArray &in)
{
    // TODO
    Q_UNUSED(in);
    return QString();
}

QByteArray NetNames::escapeDomain(const QByteArray &in)
{
    // TODO
    Q_UNUSED(in);
    return QByteArray();
}

QByteArray NetNames::unescapeDomain(const QByteArray &in)
{
    // TODO
    Q_UNUSED(in);
    return QByteArray();
}
} // namespace XMPP

#include "netnames.moc"
