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
    credHost_.clear(); // to indicate it's services request, not creds
}

void JT_ExternalServiceDiscovery::getCredentials(const QString &host, const QString &type, uint16_t port)
{
    Q_ASSERT(!host.isEmpty());
    Q_ASSERT(!type.isEmpty());
    credHost_ = host;
    type_     = type;
    credPort_ = port;
}

void JT_ExternalServiceDiscovery::onGo()
{
    QDomElement iq    = createIQ(doc(), "get", client()->jid().domain(), id());
    QDomElement query = doc()->createElementNS(QLatin1String("urn:xmpp:extdisco:2"),
                                               QLatin1String(credHost_.isEmpty() ? "services" : "credentials"));
    if (credHost_.isEmpty()) {
        if (!type_.isEmpty()) {
            query.setAttribute(QLatin1String("type"), type_);
        }
    } else {
        QDomElement service = doc()->createElement(QLatin1String("service"));
        service.setAttribute(QLatin1String("host"), credHost_);
        service.setAttribute(QLatin1String("type"), type_);
        query.appendChild(service);
    }
    iq.appendChild(query);
    send(iq);
}

bool JT_ExternalServiceDiscovery::take(const QDomElement &x)
{
    if (!iqVerify(x, Jid(QString(), client()->jid().domain()), id()))
        return false;

    if (x.attribute("type") == "result") {
        auto query = x.firstChildElement(QLatin1String(credHost_.isEmpty() ? "services" : "credentials"));
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
        expires = QDateTime::fromString(expiresOpt.left(19), Qt::ISODate);
        if (!expires.isValid())
            return false;
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

ExternalServiceDiscovery::ExternalServiceDiscovery(Client *client) : client_(client) { }

bool ExternalServiceDiscovery::isSupported() const
{
    return client_->serverInfoManager()->features().test("urn:xmpp:extdisco:2");
}

void ExternalServiceDiscovery::services(QObject *ctx, ServicesCallback &&callback, const QString &type)
{
    if (!isSupported()) {
        callback({});
        return;
    }

    // TODO optimize me (get cache, check action, etc)
    auto task = new JT_ExternalServiceDiscovery(client_->rootTask());
    connect(task, &Task::finished, ctx, [this, task, cb = std::move(callback)]() {
        services_ = task->services();
        cb(services_);
    });
    task->getServices(type);
    task->go(true);
}

} // namespace XMPP
