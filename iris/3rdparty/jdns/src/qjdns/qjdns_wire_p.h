/*
 * Framing helpers for TCP/HTTP DNS wire transports.
 * Copyright (C) 2026 Psi contributors
 * Licensed under the MIT license (see COPYING).
 */
#ifndef QJDNS_WIRE_P_H
#define QJDNS_WIRE_P_H

#include <QByteArray>
#include <QtGlobal>

namespace QJDnsWire {
constexpr int MaxMessageSize = 16384;

inline quint16 read16(const QByteArray &packet, int position)
{
    return (quint16(quint8(packet.at(position))) << 8) | quint8(packet.at(position + 1));
}

} // namespace QJDnsWire

#endif
