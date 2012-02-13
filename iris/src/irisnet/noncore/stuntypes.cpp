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

#include "stuntypes.h"

#include <stdio.h>
#include <QtCrypto>
#include "stunutil.h"

#define STRING_MAX_CHARS 127
#define STRING_MAX_BYTES 763

namespace XMPP {

using namespace StunUtil;

namespace StunTypes {

static void xorIPv4(QByteArray *in, const quint8 *magic)
{
	quint8 *p = (quint8 *)in->data();
	p[2] ^= magic[0];
	p[3] ^= magic[1];
	for(int n = 0; n < 4; ++n)
		p[n + 4] ^= magic[n];
}

static void xorIPv6(QByteArray *in, const quint8 *magic, const quint8 *id)
{
	quint8 *p = (quint8 *)in->data();
	p[2] ^= magic[0];
	p[3] ^= magic[1];
	for(int n = 0; n < 4; ++n)
		p[n + 4] ^= magic[n];
	for(int n = 0; n < 12; ++n)
		p[n + 8] ^= id[n];
}

static bool validateString(const QByteArray &in, QString *out)
{
	if(in.size() <= STRING_MAX_BYTES)
	{
		QString s = QString::fromUtf8(in);
		if(s.length() <= STRING_MAX_CHARS)
		{
			*out = s;
			return true;
		}
	}

	return false;
}

QByteArray createMappedAddress(const QHostAddress &addr, quint16 port)
{
	QByteArray out;

	if(addr.protocol() == QAbstractSocket::IPv6Protocol)
	{
		out = QByteArray(20, 0);
		out[1] = 0x02; // IPv6
		Q_IPV6ADDR addr6 = addr.toIPv6Address();
		memcpy(out.data() + 4, addr6.c, 16);
	}
	else if(addr.protocol() == QAbstractSocket::IPv4Protocol)
	{
		out = QByteArray(8, 0);
		out[1] = 0x01; // IPv4
		write32((quint8 *)out.data() + 4, addr.toIPv4Address());
	}
	else
		Q_ASSERT(0);

	write16((quint8 *)out.data() + 2, port);
	return out;
}

QByteArray createUsername(const QString &username)
{
	return username.left(STRING_MAX_CHARS).toUtf8();
}

QByteArray createErrorCode(int code, const QString &reason)
{
	QByteArray out(4, 0);

	int ih = code / 100;
	int il = code % 100;
	ih &= 0x07; // keep only lower 3 bits

	unsigned char ch = (unsigned char)ih;
	unsigned char cl = (unsigned char)il;
	out[2] = ch;
	out[3] = cl;
	out += reason.left(STRING_MAX_CHARS).toUtf8();
	return out;
}

QByteArray createUnknownAttributes(const QList<quint16> &typeList)
{
	if(typeList.isEmpty())
		return QByteArray();

	QByteArray out(typeList.count() * 2, 0);
	for(int n = 0; n < typeList.count(); ++n)
		write16((quint8 *)out.data() + (n * 2), typeList[n]);
	return out;
}

QByteArray createRealm(const QString &realm)
{
	return realm.left(STRING_MAX_CHARS).toUtf8();
}

QByteArray createNonce(const QString &nonce)
{
	return nonce.left(STRING_MAX_CHARS).toUtf8();
}

QByteArray createXorMappedAddress(const QHostAddress &addr, quint16 port, const quint8 *magic, const quint8 *id)
{
	QByteArray out = createMappedAddress(addr, port);
	if(addr.protocol() == QAbstractSocket::IPv6Protocol)
		xorIPv6(&out, magic, id);
	else // IPv4
		xorIPv4(&out, magic);
	return out;
}

QByteArray createChannelNumber(quint16 i)
{
	QByteArray val(4, 0);
	write16((quint8 *)val.data(), i);
	// bytes 2-3 are zeroed out
	return val;
}

QByteArray createLifetime(quint32 i)
{
	QByteArray val(4, 0);
	write32((quint8 *)val.data(), i);
	return val;
}

QByteArray createXorPeerAddress(const QHostAddress &addr, quint16 port, const quint8 *magic, const quint8 *id)
{
	return createXorMappedAddress(addr, port, magic, id);
}

QByteArray createXorRelayedAddress(const QHostAddress &addr, quint16 port, const quint8 *magic, const quint8 *id)
{
	return createXorMappedAddress(addr, port, magic, id);
}

QByteArray createEvenPort(bool reserve)
{
	QByteArray val(1, 0);
	unsigned char c = 0;
	if(reserve)
		c |= 0x80; // set high bit
	val[0] = c;
	return val;
}

QByteArray createRequestedTransport(quint8 proto)
{
	QByteArray val(4, 0);
	val[0] = proto;
	// bytes 1-3 are zeroed out
	return val;
}

QByteArray createReservationToken(const QByteArray &token)
{
	Q_ASSERT(token.size() == 8);
	return token;
}

QByteArray createPriority(quint32 i)
{
	QByteArray val(4, 0);
	write32((quint8 *)val.data(), i);
	return val;
}

QByteArray createSoftware(const QString &str)
{
	return str.left(STRING_MAX_CHARS).toUtf8();
}

QByteArray createAlternateServer(const QHostAddress &addr, quint16 port)
{
	return createMappedAddress(addr, port);
}

QByteArray createIceControlled(quint64 i)
{
	QByteArray val(8, 0);
	write64((quint8 *)val.data(), i);
	return val;
}

QByteArray createIceControlling(quint64 i)
{
	QByteArray val(8, 0);
	write64((quint8 *)val.data(), i);
	return val;
}

bool parseMappedAddress(const QByteArray &val, QHostAddress *addr, quint16 *port)
{
	if(val[1] == 0x02 && val.size() == 20) // IPv6
	{
		*port = read16((const quint8 *)val.data() + 2);
		QByteArray buf = val.mid(4);
		*addr = QHostAddress((quint8 *)buf.data());
		return true;
	}
	else if(val[1] == 0x01 && val.size() == 8) // IPv4
	{
		*port = read16((const quint8 *)val.data() + 2);
		*addr = QHostAddress(read32((const quint8 *)val.data() + 4));
		return true;
	}
	else
		return false;
}

bool parseUsername(const QByteArray &val, QString *username)
{
	return validateString(val, username);
}

bool parseErrorCode(const QByteArray &val, int *code, QString *reason)
{
	if(val.size() < 4)
		return false;

	unsigned char ch = (unsigned char)val[2];
	unsigned char cl = (unsigned char)val[3];
	int ih = ch & 0x07; // lower 3 bits
	int x = ih * 100 + (int)cl;

	QString str;
	if(validateString(val.mid(4), &str))
	{
		*code = x;
		*reason = str;
		return true;
	}

	return false;
}

bool parseUnknownAttributes(const QByteArray &val, QList<quint16> *typeList)
{
	if(val.size() % 2 != 0)
		return false;

	typeList->clear();
	int count = val.size() / 2;
	for(int n = 0; n < count; ++n)
		typeList->append(read16((const quint8 *)val.data() + (n * 2)));
	return true;
}

bool parseRealm(const QByteArray &val, QString *realm)
{
	return validateString(val, realm);
}

bool parseNonce(const QByteArray &val, QString *nonce)
{
	return validateString(val, nonce);
}

bool parseXorMappedAddress(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port)
{
	if(val.size() < 4)
		return false;

	QByteArray buf;
	if(val[1] == 0x02 && val.size() == 20) // IPv6
	{
		buf = val;
		xorIPv6(&buf, magic, id);
	}
	else if(val[1] == 0x01 && val.size() == 8) // IPv4
	{
		buf = val;
		xorIPv4(&buf, magic);
	}
	else
		return false;

	return parseMappedAddress(buf, addr, port);
}

bool parseChannelNumber(const QByteArray &val, quint16 *i)
{
	if(val.size() != 4)
		return false;

	const quint8 *p = (const quint8 *)val.data();
	*i = read16(p);
	return true;
}

bool parseLifetime(const QByteArray &val, quint32 *i)
{
	if(val.size() != 4)
		return false;

	const quint8 *p = (const quint8 *)val.data();
	*i = read32(p);
	return true;
}

bool parseXorPeerAddress(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port)
{
	return parseXorMappedAddress(val, magic, id, addr, port);
}

bool parseXorRelayedAddress(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port)
{
	return parseXorMappedAddress(val, magic, id, addr, port);
}

bool parseEvenPort(const QByteArray &val, bool *reserve)
{
	if(val.size() != 1)
		return false;

	unsigned char c = val[0];
	if(c & 0x80)
		*reserve = true;
	else
		*reserve = false;

	return true;
}

bool parseRequestedTransport(const QByteArray &val, quint8 *proto)
{
	if(val.size() != 4)
		return false;

	*proto = val[0];
	return true;
}

bool parseReservationToken(const QByteArray &val, QByteArray *token)
{
	if(val.size() != 8)
		return false;

	*token = val;
	return true;
}

bool parsePriority(const QByteArray &val, quint32 *i)
{
	if(val.size() != 4)
		return false;

	const quint8 *p = (const quint8 *)val.data();
	*i = read32(p);
	return true;
}

bool parseSoftware(const QByteArray &val, QString *str)
{
	*str = QString::fromUtf8(val);
	return true;
}

bool parseAlternateServer(const QByteArray &val, QHostAddress *addr, quint16 *port)
{
	return parseMappedAddress(val, addr, port);
}

bool parseIceControlled(const QByteArray &val, quint64 *i)
{
	if(val.size() != 8)
		return false;

	const quint8 *p = (const quint8 *)val.data();
	*i = read64(p);
	return true;
}

bool parseIceControlling(const QByteArray &val, quint64 *i)
{
	if(val.size() != 8)
		return false;

	const quint8 *p = (const quint8 *)val.data();
	*i = read64(p);
	return true;
}

#define METHOD_ENTRY(x) \
	{ x, #x }

struct MethodEntry
{
	Method method;
	const char *str;
} method_table[] =
{
	METHOD_ENTRY(Binding),
	METHOD_ENTRY(Allocate),
	METHOD_ENTRY(Refresh),
	METHOD_ENTRY(Send),
	METHOD_ENTRY(Data),
	METHOD_ENTRY(CreatePermission),
	METHOD_ENTRY(ChannelBind),
	{ (Method)-1, 0 }
};

QString methodToString(int method)
{
	for(int n = 0; method_table[n].str; ++n)
	{
		if(method_table[n].method == (Method)method)
			return QString::fromLatin1(method_table[n].str);
	}

	return QString();
}

#define ATTRIB_ENTRY(x) \
	{ x, #x }

struct AttribEntry
{
	Attribute type;
	const char *str;
} attrib_table[] =
{
	ATTRIB_ENTRY(MAPPED_ADDRESS),
	ATTRIB_ENTRY(USERNAME),
	ATTRIB_ENTRY(MESSAGE_INTEGRITY),
	ATTRIB_ENTRY(ERROR_CODE),
	ATTRIB_ENTRY(UNKNOWN_ATTRIBUTES),
	ATTRIB_ENTRY(REALM),
	ATTRIB_ENTRY(NONCE),
	ATTRIB_ENTRY(XOR_MAPPED_ADDRESS),
	ATTRIB_ENTRY(CHANNEL_NUMBER),
	ATTRIB_ENTRY(LIFETIME),
	ATTRIB_ENTRY(XOR_PEER_ADDRESS),
	ATTRIB_ENTRY(DATA),
	ATTRIB_ENTRY(XOR_RELAYED_ADDRESS),
	ATTRIB_ENTRY(EVEN_PORT),
	ATTRIB_ENTRY(REQUESTED_TRANSPORT),
	ATTRIB_ENTRY(DONT_FRAGMENT),
	ATTRIB_ENTRY(RESERVATION_TOKEN),
	ATTRIB_ENTRY(PRIORITY),
	ATTRIB_ENTRY(USE_CANDIDATE),
	ATTRIB_ENTRY(SOFTWARE),
	ATTRIB_ENTRY(ALTERNATE_SERVER),
	ATTRIB_ENTRY(FINGERPRINT),
	ATTRIB_ENTRY(ICE_CONTROLLED),
	ATTRIB_ENTRY(ICE_CONTROLLING),
	{ (Attribute)-1, 0 }
};

QString attributeTypeToString(int type)
{
	for(int n = 0; attrib_table[n].str; ++n)
	{
		if(attrib_table[n].type == (Attribute)type)
		{
			QString name = QString::fromLatin1(attrib_table[n].str);
			name.replace('_', '-');
			return name;
		}
	}

	return QString();
}

static QString quoted(const QString &in)
{
	return QString("\"") + in + '\"';
}

QString attributeValueToString(int type, const QByteArray &val, const quint8 *magic, const quint8 *id)
{
	switch((Attribute)type)
	{
		case MAPPED_ADDRESS:
		{
			QHostAddress addr;
			quint16 port;
			if(parseMappedAddress(val, &addr, &port))
				return addr.toString() + ';' + QString::number(port);
			break;
		}
		case USERNAME:
		{
			QString str;
			if(parseUsername(val, &str))
				return quoted(str);
			break;
		}
		case MESSAGE_INTEGRITY:
		{
			return QCA::arrayToHex(val);
		}
		case ERROR_CODE:
		{
			int code;
			QString reason;
			if(parseErrorCode(val, &code, &reason))
			{
				QString out = QString::number(code);
				if(!reason.isEmpty())
					out += QString(", ") + quoted(reason);
				return out;
			}
			break;
		}
		case UNKNOWN_ATTRIBUTES:
		{
			QList<quint16> typeList;
			if(parseUnknownAttributes(val, &typeList))
			{
				if(!typeList.isEmpty())
				{
					QStringList strList;
					foreach(quint16 i, typeList)
						strList += QString().sprintf("0x%04x", i);
					return strList.join(", ");
				}
				else
					return "(None)";
			}
			break;
		}
		case REALM:
		{
			QString str;
			if(parseRealm(val, &str))
				return quoted(str);
			break;
		}
		case NONCE:
		{
			QString str;
			if(parseNonce(val, &str))
				return quoted(str);
			break;
		}
		case XOR_MAPPED_ADDRESS:
		{
			QHostAddress addr;
			quint16 port;
			if(parseXorMappedAddress(val, magic, id, &addr, &port))
				return addr.toString() + ';' + QString::number(port);
			break;
		}
		case CHANNEL_NUMBER:
		{
			quint16 i;
			if(parseChannelNumber(val, &i))
				return QString().sprintf("0x%04x", (int)i);
			break;
		}
		case LIFETIME:
		{
			quint32 i;
			if(parseLifetime(val, &i))
				return QString::number(i);
			break;
		}
		case XOR_PEER_ADDRESS:
		{
			return attributeValueToString(XOR_MAPPED_ADDRESS, val, magic, id);
		}
		case DATA:
		{
			return QString("len=%1, ").arg(val.size()) + QCA::arrayToHex(val);
		}
		case XOR_RELAYED_ADDRESS:
		{
			return attributeValueToString(XOR_MAPPED_ADDRESS, val, magic, id);
		}
		case EVEN_PORT:
		{
			bool reserve;
			if(parseEvenPort(val, &reserve))
				return QString("reserve=") + (reserve ? "true" : "false");
			break;
		}
		case REQUESTED_TRANSPORT:
		{
			quint8 proto;
			if(parseRequestedTransport(val, &proto))
			{
				QString str = QString::number((int)proto);
				if(proto == 17) // UDP
					str += " (UDP)";
				else
					str += " (Unknown)";
				return str;
			}
			break;
		}
		case DONT_FRAGMENT:
		{
			return QString("");
		}
		case RESERVATION_TOKEN:
		{
			QByteArray token;
			if(parseReservationToken(val, &token))
				return QCA::arrayToHex(token);
			break;
		}
		case PRIORITY:
		{
			quint32 i;
			if(parsePriority(val, &i))
				return QString::number(i);
			break;
		}
		case USE_CANDIDATE:
		{
			return QString("");
		}
		case SOFTWARE:
		{
			QString out;
			if(parseSoftware(val, &out))
				return quoted(out);
			break;
		}
		case ALTERNATE_SERVER:
		{
			return attributeValueToString(MAPPED_ADDRESS, val, magic, id);
		}
		case FINGERPRINT:
		{
			return QCA::arrayToHex(val);
		}
		case ICE_CONTROLLED:
		{
			quint64 i;
			if(parseIceControlled(val, &i))
				return QString::number(i);
			break;
		}
		case ICE_CONTROLLING:
		{
			quint64 i;
			if(parseIceControlling(val, &i))
				return QString::number(i);
			break;
		}
	}

	return QString();
}

QString print_packet_str(const StunMessage &message)
{
	QString out;

	QString mclass;
	if(message.mclass() == StunMessage::Request)
		mclass = "Request";
	else if(message.mclass() == StunMessage::SuccessResponse)
		mclass = "Response (Success)";
	else if(message.mclass() == StunMessage::ErrorResponse)
		mclass = "Response (Error)";
	else if(message.mclass() == StunMessage::Indication)
		mclass = "Indication";
	else
		Q_ASSERT(0);

	out += QString("Class: %1\n").arg(mclass);
	out += QString("Method: %1\n").arg(methodToString(message.method()));
	out += QString("Transaction id: %1\n").arg(QCA::arrayToHex(QByteArray((const char *)message.id(), 12)));
	out += "Attributes:";
	QList<StunMessage::Attribute> attribs = message.attributes();
	if(!attribs.isEmpty())
	{
		foreach(const StunMessage::Attribute &a, attribs)
		{
			out += '\n';

			QString name = attributeTypeToString(a.type);
			if(!name.isNull())
			{
				QString val = attributeValueToString(a.type, a.value, message.magic(), message.id());
				if(val.isNull())
					val = QString("Unable to parse %1 bytes").arg(a.value.size());

				out += QString("  %1").arg(name);
				if(!val.isEmpty())
					out += QString(" = %1").arg(val);
			}
			else
				out += QString().sprintf("  Unknown attribute (0x%04x) of %d bytes", a.type, a.value.size());
		}
	}
	else
		out += "\n  (None)";

	return out;
}

void print_packet(const StunMessage &message)
{
	printf("%s\n", qPrintable(print_packet_str(message)));
}

}

}
