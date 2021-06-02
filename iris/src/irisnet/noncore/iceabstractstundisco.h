#ifndef XMPP_ABSTRACTSTUNDISCO_H
#define XMPP_ABSTRACTSTUNDISCO_H

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>

#include <functional>

namespace XMPP {

/**
 * Monitors if new STUN services are available, changed or not available anymore.
 */
class AbstractStunDiscoMonitor : public QObject {
    Q_OBJECT
public:
    enum Transport : std::uint8_t { Tcp, Udp };
    enum Flag : std::uint8_t { Relay = 0x01, Tls = 0x02, Restricted = 0x04 };
    Q_DECLARE_FLAGS(Flags, Flag)

    struct Service {
        using Ptr = std::shared_ptr<Service>;
        QString             name;
        QString             username;
        QString             password;
        QString             host;
        QList<QHostAddress> addresses;
        std::uint16_t       port = 0;
        Transport           transport;
        Flags               flags;
        QDeadlineTimer      expires;
    };

    using QObject::QObject;

    /**
     * Check where initial discovery is still in progress and therefore it's worth waiting for completion
     */
    virtual bool isDiscoInProgress() const = 0;

Q_SIGNALS:
    void discoFinished(); // if impl did rediscovery, it will signal when finished. required for initial start()
    void serviceAdded(Service::Ptr);
    void serviceRemoved(Service::Ptr);
    void serviceModified(Service::Ptr);
};

} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::AbstractStunDiscoMonitor::Flags)

#endif // XMPP_ABSTRACTSTUNDISCO_H
