/*
 * xmpp_externalservicedisco.cpp - Implementation of XEP-0215
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

#include "xmpp_externalservicediscovery.h"

#include "xmpp_client.h"
#include "xmpp_jid.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {

JT_ExternalServiceDiscovery::JT_ExternalServiceDiscovery(Task *parent) : Task(parent) { }

void JT_ExternalServiceDiscovery::getServices(const QString &type)
{
    type_ = type;
    creds_.clear(); // to indicate it's services request, not creds
}

void JT_ExternalServiceDiscovery::getCredentials(const QSet<ExternalServiceId> &ids) { creds_ = ids; }

void JT_ExternalServiceDiscovery::onGo()
{
    QDomElement iq    = createIQ(doc(), "get", client()->jid().domain(), id());
    QDomElement query = doc()->createElementNS(QLatin1String("urn:xmpp:extdisco:2"),
                                               QLatin1String(creds_.isEmpty() ? "services" : "credentials"));
    if (creds_.isEmpty()) {
        if (!type_.isEmpty()) {
            query.setAttribute(QLatin1String("type"), type_);
        }
    } else {
        for (auto const &c : std::as_const(creds_)) {
            QDomElement service = doc()->createElement(QLatin1String("service"));
            service.setAttribute(QLatin1String("host"), c.host);
            service.setAttribute(QLatin1String("type"), c.type);
            if (c.port)
                service.setAttribute(QLatin1String("port"), c.port);
            query.appendChild(service);
        }
    }
    iq.appendChild(query);
    send(iq);
}

bool JT_ExternalServiceDiscovery::take(const QDomElement &x)
{
    if (!iqVerify(x, Jid(QString(), client()->jid().domain()), id()))
        return false;

    if (x.attribute("type") == "result") {
        auto query = x.firstChildElement(QLatin1String(creds_.isEmpty() ? "services" : "credentials"));
        if (query.namespaceURI() != QLatin1String("urn:xmpp:extdisco:2")) {
            setError(0, QLatin1String("invalid namespace"));
            return true;
        }
        QString serviceTag(QLatin1String("service"));
        for (auto el = query.firstChildElement(serviceTag); !el.isNull(); el = el.nextSiblingElement(serviceTag)) {
            // services_.append(ExternalService {});
            auto s = std::make_shared<ExternalService>();
            if (s->parse(el)) {
                services_.append(s);
            }
        }
        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

bool ExternalService::parse(QDomElement &el)
{
    QString actionOpt     = el.attribute(QLatin1String("action"));
    QString expiresOpt    = el.attribute(QLatin1String("expires"));
    name                  = el.attribute(QLatin1String("name"));
    password              = el.attribute(QLatin1String("password"));
    QString restrictedOpt = el.attribute(QLatin1String("restricted"));
    transport             = el.attribute(QLatin1String("transport"));
    username              = el.attribute(QLatin1String("username"));
    host                  = el.attribute(QLatin1String("host"));
    QString portReq       = el.attribute(QLatin1String("port"));
    type                  = el.attribute(QLatin1String("type"));

    bool ok;
    if (host.isEmpty() || portReq.isEmpty() || type.isEmpty())
        return false;

    port = portReq.toUShort(&ok);
    if (!ok)
        return false;

    if (!expiresOpt.isEmpty()) {
        auto date   = QDateTime::fromString(expiresOpt.left(19), Qt::ISODate);
        auto curUtc = QDateTime::currentDateTimeUtc();
        if (!date.isValid() || date < curUtc)
            return false;
        expires = QDeadlineTimer(curUtc.msecsTo(date));
        if (expires.hasExpired())
            qInfo("Server returned already expired service %s expired at %s UTC", qPrintable(*this),
                  qPrintable(expiresOpt));
    } else {
        expires = QDeadlineTimer(QDeadlineTimer::Forever);
    }

    if (actionOpt.isEmpty() || actionOpt == QLatin1String("add"))
        action = Action::Add;
    else if (actionOpt == QLatin1String("modify"))
        action = Action::Modify;
    else if (actionOpt == QLatin1String("delete"))
        action = Action::Delete;
    else
        return false;

    if (!restrictedOpt.isEmpty()) {
        if (restrictedOpt == QLatin1String("true") || restrictedOpt == QLatin1String("1"))
            restricted = true;
        else if (restrictedOpt != QLatin1String("false") && restrictedOpt != QLatin1String("0"))
            return false;
    }

    return true;
}

ExternalService::operator QString() const
{
    return QString(QLatin1String("ExternalService<name=%1 host=%2 port=%3 type=%4 transport=%5>"))
        .arg(name, host, QString::number(port), type, transport);
}

ExternalServiceDiscovery::ExternalServiceDiscovery(Client *client) : client_(client) { }

bool ExternalServiceDiscovery::isSupported() const
{
    return client_->serverInfoManager()->features().test("urn:xmpp:extdisco:2");
}

void ExternalServiceDiscovery::services(QObject *ctx, ServicesCallback &&callback, std::chrono::minutes minTtl,
                                        const QStringList &types)
{
    if (!isSupported()) {
        callback({});
        return;
    }

    // check if cache is valid (no expired or ready to expire items)
    ExternalServiceList ret;
    bool                cacheValid = true;
    for (auto const &s : std::as_const(services_)) {
        if (!(types.isEmpty() || types.contains(s->type)))
            continue; // not interesting for us
        if (!(s->expires.isForever() || s->expires.remainingTimeAsDuration() > minTtl)) {
            cacheValid = false;
            break;
        }
        ret += s;
    }

    if (cacheValid && !ret.isEmpty()) {
        callback(ret);
        return;
    }

    if (currentTask) {
        connect(currentTask, &Task::finished, ctx,
                [this, types, cb = std::move(callback)]() { cb(cachedServices(types)); });
    } else {
        if (types.isEmpty() || types.size() > 1) {
            currentTask = new JT_ExternalServiceDiscovery(client_->rootTask());
            connect(currentTask, &Task::finished, ctx, [this, types, cb = std::move(callback)]() {
                services_   = currentTask->services();
                currentTask = nullptr; // it will self-delete anyway
                cb(cachedServices(types));
            });
            currentTask->getServices();
            currentTask->go(true);
        } else {
            auto task = new JT_ExternalServiceDiscovery(client_->rootTask());
            auto type = types[0];
            connect(task, &Task::finished, ctx, [task, type, cb = std::move(callback)]() { cb(task->services()); });
            task->getServices(type);
            task->go(true);
            // in fact we can improve caching even more if start remembering specific repviously requested types,
            // even if the result was negative.
        }
    }
}

ExternalServiceList ExternalServiceDiscovery::cachedServices(const QStringList &types)
{
    if (types.isEmpty())
        return services_;

    ExternalServiceList ret;
    for (auto const &s : std::as_const(services_)) {
        if (types.contains(s->type))
            ret += s;
    }
    return ret;
}

void ExternalServiceDiscovery::credentials(QObject *ctx, ServicesCallback &&callback,
                                           const QSet<ExternalServiceId> &ids)
{
    auto task = new JT_ExternalServiceDiscovery(client_->rootTask());
    connect(task, &Task::finished, ctx ? ctx : this, [task, cb = std::move(callback)]() { cb(task->services()); });
    task->getCredentials(ids);
    task->go(true);
}

} // namespace XMPP
