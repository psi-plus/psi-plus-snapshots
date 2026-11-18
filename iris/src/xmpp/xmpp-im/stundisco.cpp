/*
 * stundisco.cpp - STUN/TURN service discoverer
 * Copyright (C) 2021  Sergey Ilinykh
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

#include "stundisco.h"

#include "netnames.h"
#include "stunserviceaddress_p.h"
#include "xmpp_client.h"
#include "xmpp_externalservicediscovery.h"

#include <QPointer>

#include <chrono>

namespace XMPP {

class StunDiscoMonitor : public AbstractStunDisco {
    Q_OBJECT
public:
    using StunList = QList<AbstractStunDisco::Service::Ptr>;

    StunDiscoMonitor(StunDiscoManager *manager) : AbstractStunDisco(manager), manager_(manager)
    {
        auto extdisco = manager->client()->externalServiceDiscovery();
        connect(extdisco, &ExternalServiceDiscovery::serviceAdded, this,
                [this](const ExternalServiceList &l) { addNewServices(l); });
        connect(extdisco, &ExternalServiceDiscovery::serviceModified, this,
                [this](const ExternalServiceList &l) { modifyServices(l); });
        connect(extdisco, &ExternalServiceDiscovery::serviceDeleted, this,
                [this](const ExternalServiceList &l) { deleteServices(l); });

        QTimer::singleShot(0, this, &StunDiscoMonitor::disco);
    }

    bool isDiscoInProgress() const { return inProgress_; }

private:
    static QStringList supportedServiceTypes()
    {
        return QStringList(
            { QStringLiteral("stun"), QStringLiteral("stuns"), QStringLiteral("turn"), QStringLiteral("turns") });
    }

    void disco()
    {
        // ICE takes ownership of the monitor, so it can outlive the Client
        // and the manager that created it.
        if (!manager_)
            return;
        using namespace std::chrono_literals;
        manager_->client()->externalServiceDiscovery()->services(
            this, [this](auto const &list) { addNewServices(list); }, 5min, supportedServiceTypes());
    }

    void addNewServices(const ExternalServiceList &list)
    {
        StunList needCreds;
        StunList needResolve;
        for (auto const &l : list) {
            auto s = std::make_shared<AbstractStunDisco::Service>();
            if (l->type.startsWith(QLatin1String("turn")))
                s->flags |= AbstractStunDisco::Relay;
            else if (!l->type.startsWith(QLatin1String("stun")))
                continue;
            if (l->type.endsWith(QLatin1Char('s')))
                s->flags |= AbstractStunDisco::Tls;
            if (l->restricted)
                s->flags |= AbstractStunDisco::Restricted;
            s->transport = l->transport == QLatin1String("tcp") ? AbstractStunDisco::Tcp : AbstractStunDisco::Udp;
            s->expires   = l->expires; // it's definitely not expired. no need to check
            s->port      = l->port;
            s->name      = l->name;
            s->username  = l->username;
            s->password  = l->password;
            s->host      = l->host;
            QHostAddress addr(l->host);
            if (addr.isNull())
                needResolve.append(s);
            else if (isUsableStunServiceAddress(addr))
                s->addresses.append(addr);
            else {
                qWarning("stundisco: ignoring unusable STUN/TURN address %s", qPrintable(l->host));
                continue;
            }
            if (l->needsNewCreds()) {
                needCreds.append(s);
            } else if (!s->addresses.isEmpty()) {
                emit serviceAdded(s);
            } else
                pendingWork_.append(s);
        }
        bool final = needResolve.isEmpty() && needCreds.isEmpty();
        if (!needResolve.isEmpty())
            resolve(needResolve);
        if (!needCreds.isEmpty())
            getCreds(needCreds);
        if (final) {
            emit discoFinished();
            deleteLater();
        }
    }

    void modifyServices(const ExternalServiceList &list)
    {
        Q_UNUSED(list);
        // TODO
    }
    void deleteServices(const ExternalServiceList &list)
    {
        Q_UNUSED(list);
        // TODO
    }

    static QString extType(AbstractStunDisco::Service::Ptr s)
    {
        bool isTls = s->flags & AbstractStunDisco::Tls;
        return QLatin1String(s->flags & AbstractStunDisco::Relay ? (isTls ? "turns" : "turn")
                                                                 : (isTls ? "stuns" : "stun"));
    }

    void resolve(const StunList &services)
    {
        QSet<QString> names;
        for (auto const &s : services)
            names.insert(s->host);

        for (auto const &name : names) {
            auto *dns = new NameResolver(this);

            connect(dns, &NameResolver::resultsReady, this, [this, name](const QList<XMPP::NameRecord> &records) {
                QList<QHostAddress> addresses;
                for (const auto &r : records) {
                    if (isUsableStunServiceAddress(r.address()))
                        addresses.append(r.address());
                }
                if (addresses.isEmpty())
                    qWarning("stundisco: no usable DNS addresses for STUN/TURN host %s", qPrintable(name));
                setAddresses(name, addresses);
            });
            connect(dns, &NameResolver::error, this,
                    [this, name](XMPP::NameResolver::Error) { setAddresses(name, {}); });

            dns->start(name.toLatin1());
        }
    }

    void setAddresses(const QString &host, const QList<QHostAddress> &addresses)
    {
        for (auto const &s : std::as_const(pendingWork_)) {
            if (s->host == host && s->addresses.isEmpty()) {
                if (addresses.isEmpty())
                    s->expires = QDeadlineTimer(); // expired
                else
                    s->addresses = addresses;
            }
        }
        tryFinish();
    }

    void getCreds(const StunList &services)
    {
        if (!manager_)
            return;
        QSet<ExternalServiceId> ids;
        for (auto const &s : services) {
            ExternalServiceId id;
            id.host = s->host;
            id.port = s->port;
            id.type = extType(s);
            ids.insert(id);
        }
        manager_->client()->externalServiceDiscovery()->credentials(
            this,
            [services, this](const ExternalServiceList &resolved) {
                if (pendingWork_.isEmpty())
                    return; // we are finsihed already
                for (auto const &s : services) {
                    if (s->expires.hasExpired())
                        continue; // ditch it. either really expired or failed on dns
                    QString etype = extType(s);
                    auto    it
                        = std::find_if(resolved.begin(), resolved.end(), [&s, &etype](ExternalService::Ptr const &r) {
                              return s->host == r->host && etype == r->type && (r->port == 0 || s->port == r->port);
                          });
                    if (it == resolved.end()) {
                        s->expires = QDeadlineTimer(); // expired timer
                        qDebug("no creds from server for %s:%hu %s", qPrintable(s->host), s->port, qPrintable(etype));
                        continue; // failed to get creds? weird
                    }
                    s->expires  = (*it)->expires;
                    s->username = (*it)->username;
                    s->password = (*it)->password;
                }
                tryFinish();
            },
            ids);
    }

    void tryFinish()
    {
        Q_ASSERT(!pendingWork_.isEmpty());
        auto it = pendingWork_.begin();
        while (it != pendingWork_.end()) {
            auto s = **it;
            if (s.expires.hasExpired()) { // was marked invalid or really expired
                it = pendingWork_.erase(it);
                continue;
            }
            if (s.addresses.isEmpty() || (s.flags & AbstractStunDisco::Restricted && s.password.isEmpty())) {
                ++it;
                continue; // in progress yet.
            }
            emit serviceAdded(*it);
            it = pendingWork_.erase(it);
        }
        if (pendingWork_.isEmpty()) {
            if (inProgress_)
                emit discoFinished();
        }
    }

    QPointer<StunDiscoManager>               manager_;
    bool                                   inProgress_ = false; // initial disco
    QList<AbstractStunDisco::Service::Ptr> pendingWork_;
};

StunDiscoManager::StunDiscoManager(Client *client) : QObject(client) { client_ = client; }

StunDiscoManager::~StunDiscoManager() { }

AbstractStunDisco *StunDiscoManager::createMonitor() { return new StunDiscoMonitor(this); }

} // namespace XMPP

#include "stundisco.moc"
