/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef STUNUTIL_H
#define STUNUTIL_H

#include <QtCrypto>

namespace XMPP {

namespace StunUtil {

quint16 read16(const quint8 *in);
quint32 read32(const quint8 *in);
quint64 read64(const quint8 *in);

void write16(quint8 *out, quint16 i);
void write32(quint8 *out, quint32 i);
void write64(quint8 *out, quint64 i);

QCA::SecureArray saslPrep(const QCA::SecureArray &in);

}

}

#endif
