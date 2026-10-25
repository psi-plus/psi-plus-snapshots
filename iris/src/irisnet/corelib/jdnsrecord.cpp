/*
 * Copyright (C) 2005-2008  Justin Karneges
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

#include "jdnsrecord_p.h"

#include <QUrl>

namespace XMPP {
NameRecord importJDNSRecord(const QJDns::Record &in)
{
    NameRecord out;
    switch (in.type) {
    case QJDns::A:
        out.setAddress(in.address);
        break;
    case QJDns::Aaaa:
        out.setAddress(in.address);
        break;
    case QJDns::Mx:
        out.setMx(in.name, in.priority);
        break;
    case QJDns::Srv:
        out.setSrv(in.name, in.port, in.priority, in.weight);
        break;
    case QJDns::Cname:
        out.setCname(in.name);
        break;
    case QJDns::Ptr:
        out.setPtr(in.name);
        break;
    case QJDns::Txt:
        out.setTxt(in.texts);
        break;
    case QJDns::Hinfo:
        out.setHinfo(in.cpu, in.os);
        break;
    case QJDns::Ns:
        out.setNs(in.name);
        break;
    case 10:
        out.setNull(in.rdata);
        break;
    default:
        return out;
    }
    out.setOwner(QUrl::fromAce(in.owner));
    out.setTtl(in.ttl);
    return out;
}
} // namespace XMPP
