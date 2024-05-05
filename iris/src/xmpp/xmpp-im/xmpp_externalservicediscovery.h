/*
 * xmpp_externalservicedisco.h - Implementation of XEP-0215
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

#ifndef XMPP_EXTERNALSERVICEDISCOVERY_H
#define XMPP_EXTERNALSERVICEDISCOVERY_H

#include "xmpp_task.h"
#include "xmpp_xdata.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>

namespace XMPP {

struct ExternalService {
    using Ptr = std::shared_ptr<ExternalService>;
    enum Action { Add, Delete, Modify };

    Action         action  = Action::Add;             // required only for pushes
    QDeadlineTimer expires = QDeadlineTimer::Forever; // optional
    QString        host;                              // required
    QString        name;                              // optional
    QString        password;                          // optional
    std::uint16_t  port;                              // required
    bool           restricted = false;                // optional
    QString        transport;                         // optional
    QString        type;                              // required
    QString        username;                          // optional
    XData          form;                              // optional

    bool parse(QDomElement &el, bool isCreds, bool isPush);
    operator QString() const;

    bool needsNewCreds(std::chrono::minutes minTtl = std::chrono::minutes(1)) const;
};

struct ExternalServiceId {
    QString       host;
    QString       type;
    std::uint16_t port;

    bool operator==(const ExternalServiceId &other) const
    {
        return host == other.host && type == other.type && port == other.port;
    }
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
inline uint qHash(const ExternalServiceId &id, uint seed = 0)
#else
inline size_t qHash(const ExternalServiceId &id, size_t seed = 0)
#endif
{
    return ::qHash(id.host, seed) ^ ::qHash(id.type, seed) ^ ::qHash(id.port, seed);
}

using ExternalServiceList = QVector<ExternalService::Ptr>;

// XEP-0215 0.7
class JT_ExternalServiceDiscovery : public Task {
    Q_OBJECT
public:
    explicit JT_ExternalServiceDiscovery(Task *parent = nullptr);

    void                              getServices(const QString &type = QString());
    void                              getCredentials(const QSet<ExternalServiceId> &ids);
    inline const ExternalServiceList &services() const { return services_; }

    void onGo();
    bool take(const QDomElement &);

private:
    QSet<ExternalServiceId> creds_;
    QString                 type_;

    ExternalServiceList services_; // result
};

class ExternalServiceDiscovery : public QObject {
    Q_OBJECT
public:
    using ServicesCallback = std::function<void(const ExternalServiceList &)>;

    ExternalServiceDiscovery(Client *client);

    bool isSupported() const;

    /**
     * @brief Resuest services from server or use cached ones
     * @param ctx           - if ctx dies, the request will be aborted
     * @param callback      - callback to call when ready
     * @param minTtl        - if service expires in less than minTtl it will be re-requested
     * @param serviceTypes  - types of services to request. e.g "stun", "turn"
     */
    void services(QObject *ctx, ServicesCallback &&callback, std::chrono::minutes minTtl = std::chrono::minutes(1),
                  const QStringList &types = QStringList());
    ExternalServiceList cachedServices(const QStringList &type = QStringList());

    /**
     * @brief credentials resolves credentials for specific services
     * @param ctx           - if ctx dies, the request will be aborted
     * @param callback      - callback to call when ready
     * @param ids           - identifier of services
     * @param minTtl        - if service expires in less than minTtl it will be re-requested
     *
     * The credentials won't be cached since it's assumed if crdentials are returned with services request then
     * the creds are constant values until the service is expired.
     * Otherwise `restricted` flag has to be set and the credentials are requested when they are really needed.
     * Most likely with `restricted` flag those are going to be temporal credentials. Even so the caller may cache them
     * on its own risk.
     */
    void credentials(QObject *ctx, ServicesCallback &&callback, const QSet<ExternalServiceId> &ids,
                     std::chrono::minutes minTtl = std::chrono::minutes(1));
signals:
    // server push signals only
    void serviceAdded(const ExternalServiceList &);
    void serviceDeleted(ExternalServiceList &);
    void serviceModified(ExternalServiceList &);

private:
    ExternalServiceList::iterator findCachedService(const ExternalServiceId &id = {});

    Client                               *client_;
    QPointer<JT_ExternalServiceDiscovery> currentTask = nullptr; // for all services (no type)
    ExternalServiceList                   services_;
};

} // namespace XMPP

#endif // XMPP_EXTERNALSERVICEDISCOVERY_H
