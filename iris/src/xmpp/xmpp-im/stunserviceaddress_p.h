// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef STUNSERVICEADDRESS_P_H
#define STUNSERVICEADDRESS_P_H

#include <QHostAddress>

namespace XMPP {

// An unspecified address can bind a local listener, but cannot identify a remote STUN/TURN server.
inline bool isUsableStunServiceAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    if (address.protocol() == QAbstractSocket::IPv4Protocol)
        return address.toIPv4Address() != 0;
    if (address.protocol() == QAbstractSocket::IPv6Protocol)
        return address != QHostAddress(QHostAddress::AnyIPv6);
    return false;
}

inline bool isUnusableStunServiceLiteral(const QString &host)
{
    QHostAddress address;
    return address.setAddress(host) && !isUsableStunServiceAddress(address);
}

} // namespace XMPP

#endif // STUNSERVICEADDRESS_P_H
