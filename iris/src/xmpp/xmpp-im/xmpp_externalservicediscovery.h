#ifndef XMPP_EXTERNALSERVICEDISCOVERY_H
#define XMPP_EXTERNALSERVICEDISCOVERY_H

#include "xmpp_task.h"

#include <QDateTime>
#include <QObject>
#include <memory>

namespace XMPP {

struct ExternalService {
    using Ptr = std::shared_ptr<ExternalService>;
    enum Action { Add, Delete, Modify };

    Action        action = Action::Add;
    QDateTime     expires;            // optional
    QString       host;               // required
    QString       name;               // optional
    QString       password;           // optional
    std::uint16_t port;               // required
    bool          restricted = false; // optional
    QString       transport;          // optional
    QString       type;               // required
    QString       username;           // optional

    bool parse(QDomElement &el);
};

using ExternalServiceList = QVector<ExternalService::Ptr>;

// XEP-0215 0.7
class JT_ExternalServiceDiscovery : public Task {
    Q_OBJECT
public:
    explicit JT_ExternalServiceDiscovery(Task *parent = nullptr);

    void                              getServices(const QString &type = QString());
    void                              getCredentials(const QString &host, const QString &type, std::uint16_t port = 0);
    inline const ExternalServiceList &services() const { return services_; }

    void onGo();
    bool take(const QDomElement &);

private:
    std::uint16_t credPort_ = 0;
    QString       credHost_;
    QString       type_;

    ExternalServiceList services_; // result
};

class ExternalServiceDiscovery : public QObject {
    Q_OBJECT
public:
    using ServicesCallback = std::function<void(const ExternalServiceList &)>;

    ExternalServiceDiscovery(Client *client);

    bool isSupported() const;
    void services(QObject *ctx, ServicesCallback &&callback, const QString &type = QString());
signals:
    // server push signals only
    void serviceAdded(ExternalService::Ptr);
    void serviceDeleted(ExternalService::Ptr);
    void serviceModified(ExternalService::Ptr);

private:
    Client *            client_;
    ExternalServiceList services_;
};

} // namespace XMPP

#endif // XMPP_EXTERNALSERVICEDISCOVERY_H
