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

#include "stunutil.h"

namespace XMPP {

namespace StunUtil {

quint16 read16(const quint8 *in)
{
	quint16 out = in[0];
	out <<= 8;
	out += in[1];
	return out;
}

quint32 read32(const quint8 *in)
{
	quint32 out = in[0];
	out <<= 8;
	out += in[1];
	out <<= 8;
	out += in[2];
	out <<= 8;
	out += in[3];
	return out;
}

quint64 read64(const quint8 *in)
{
	quint64 out = in[0];
	out <<= 8;
	out += in[1];
	out <<= 8;
	out += in[2];
	out <<= 8;
	out += in[3];
	out <<= 8;
	out += in[4];
	out <<= 8;
	out += in[5];
	out <<= 8;
	out += in[6];
	out <<= 8;
	out += in[7];
	return out;
}

void write16(quint8 *out, quint16 i)
{
	out[0] = (i >> 8) & 0xff;
	out[1] = i & 0xff;
}

void write32(quint8 *out, quint32 i)
{
	out[0] = (i >> 24) & 0xff;
	out[1] = (i >> 16) & 0xff;
	out[2] = (i >> 8) & 0xff;
	out[3] = i & 0xff;
}

void write64(quint8 *out, quint64 i)
{
	out[0] = (i >> 56) & 0xff;
	out[1] = (i >> 48) & 0xff;
	out[2] = (i >> 40) & 0xff;
	out[3] = (i >> 32) & 0xff;
	out[4] = (i >> 24) & 0xff;
	out[5] = (i >> 16) & 0xff;
	out[6] = (i >> 8) & 0xff;
	out[7] = i & 0xff;
}

QCA::SecureArray saslPrep(const QCA::SecureArray &in)
{
	// TODO
	return in;
}

}

}
