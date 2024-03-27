#ifndef TRANSPORTADDRESS_H
#define TRANSPORTADDRESS_H

#include <QHostAddress>

namespace XMPP {

class TransportAddress {
public:
    QHostAddress addr;
    quint16      port = 0;

    TransportAddress() = default;
    TransportAddress(const QHostAddress &_addr, quint16 _port) : addr(_addr), port(_port) { }

    bool isValid() const { return !addr.isNull(); }
    bool operator==(const TransportAddress &other) const { return addr == other.addr && port == other.port; }

    inline bool operator!=(const TransportAddress &other) const { return !operator==(other); }
    inline      operator QString() const
    {
        return QString(QLatin1String("%1:%2")).arg(addr.toString(), QString::number(port));
    }
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)

inline uint qHash(const TransportAddress &key, uint seed = 0)
#else
inline size_t qHash(const TransportAddress &key, size_t seed = 0)
#endif
{
    return ::qHash(key.addr, seed) ^ ::qHash(key.port, seed);
}

}

#endif // TRANSPORTADDRESS_H
