/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IRISNET_JDNSRECORD_P_H
#define IRISNET_JDNSRECORD_P_H

#include "netnames.h"

#include <qjdns.h>

namespace XMPP {
// Converts a record decoded by JDNS; returns a null record for types NameRecord cannot hold.
NameRecord importJDNSRecord(const QJDns::Record &in);
} // namespace XMPP

#endif // IRISNET_JDNSRECORD_P_H
