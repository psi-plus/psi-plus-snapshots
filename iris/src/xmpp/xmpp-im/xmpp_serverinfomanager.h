/*
 * xmpp_serverinfomanager.h
 * Copyright (C) 2006  Remko Troncon
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef SERVERINFOMANAGER_H
#define SERVERINFOMANAGER_H

#include "xmpp_discoitem.h"
#include "xmpp_status.h"

#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QVariant>

#include <functional>
#include <list>

namespace XMPP {
class Client;
class Features;
class Jid;

class ServerInfoManager : public QObject {
    Q_OBJECT
public:
    enum SQOption {
        SQ_CheckAllOnNoMatch    = 1, // check all if matched by name services do not match or no matched by name
        SQ_FinishOnFirstMatch   = 2, // first callback is final
        SQ_CallbackOnAnyMatches = 4  // TODO don't wait while all services will be discovered. empty result list = final
    };
    Q_DECLARE_FLAGS(SQOptions, SQOption)

private:
    struct ServiceQuery {
        const QString                                           type;
        const QString                                           category;
        const QList<QSet<QString>>                              features;
        const QRegularExpression                                nameHint;
        const SQOptions                                         options;
        const std::function<void(const QList<DiscoItem> &item)> callback;
        std::list<QString>                                      servicesToQuery;
        std::list<QString> spareServicesToQuery; // usually a fallback when the above is not matched
        bool               servicesToQueryDefined = false;
        QList<DiscoItem>   result;

        ServiceQuery(const QString &type, const QString &category, const QList<QSet<QString>> &features,
                     const QRegularExpression &nameHint, const SQOptions &options,
                     const std::function<void(const QList<DiscoItem> &item)> &&callback) :
            type(type), category(category), features(features), nameHint(nameHint), options(options), callback(callback)
        {
        }
    };

    enum ServicesState { ST_NotQueried, ST_InProgress, ST_Ready, ST_Failed };

    struct ServiceInfo {
        ServicesState           state;
        DiscoItem               item;
        QMap<QString, QVariant> meta;
    };

public:
    ServerInfoManager(XMPP::Client *client);

    const QString                           &multicastService() const;
    inline bool                              hasPEP() const { return _hasPEP; }
    inline bool                              hasPersistentStorage() const { return _hasPersistentStorage; }
    inline const Features                   &serverFeatures() const { return _serverFeatures; }
    inline const Features                   &accountFeatures() const { return _accountFeatures; }
    bool                                     canMessageCarbons() const;
    inline const QMap<QString, QStringList> &extraServerInfo() const { return _extraServerInfo; }

    /*
     empty type/category/features/nameHint means it won't be checked.
     nameHint is a regular expression for service jid.
       empty regexp = ".*". if regexp is not empty but matches with empty string then
       first matched not empty name will be preferred, and if nothing nonempty matched then
       all services will be checked by other params. If regexp doesn't match with empty string then
       only exact matches will be checked.
       It means nameHint may work like a hint but not a requirement.
     features is a list of options groups. all options of any group must match

     Example:
       type = file
       category = store
       features = [("urn:xmpp:http:upload"),("urn:xmpp:http:upload:0")]
       nameHint = (http\..*|)  // search for service name like http.jabber.ru
     Result: disco info for upload.jabber.ru will be returned.
    */
    void     queryServiceInfo(const QString &category, const QString &type, const QList<QSet<QString>> &features,
                              const QRegularExpression &nameHint, SQOptions options,
                              std::function<void(const QList<DiscoItem> &items)> callback);
    void     setServiceMeta(const Jid &service, const QString &key, const QVariant &value);
    QVariant serviceMeta(const Jid &service, const QString &key);

signals:
    void featuresChanged();
    void servicesChanged();

private slots:
    void server_disco_finished();
    void account_disco_finished();
    void initialize();
    void deinitialize();
    void reset();

private:
    void queryServicesList();
    void checkPendingServiceQueries();
    void appendQuery(const ServiceQuery &q);

private:
    XMPP::Client              *_client = nullptr;
    CapsSpec                   _caps;
    Features                   _serverFeatures;
    Features                   _accountFeatures;
    QString                    _multicastService;
    QMap<QString, QStringList> _extraServerInfo; // XEP-0128, XEP-0157

    std::list<ServiceQuery> _serviceQueries; // a storage of pending requests as result of `queryService` call
    ServicesState           _servicesListState = ST_NotQueried;
    QMap<QString, ServiceInfo>
        _servicesInfo; // all the diso#info requests for services of this server jid=>(state,info)

    bool _featuresRequested    = false;
    bool _hasPEP               = false;
    bool _hasPersistentStorage = false;
    bool _canMessageCarbons    = false;
};
} // namespace XMPP

#endif // SERVERINFOMANAGER_H
