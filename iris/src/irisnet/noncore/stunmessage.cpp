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

#include "stunmessage.h"

#include <QSharedData>
#include <QtCrypto>
#include "stunutil.h"

#define ENSURE_D { if(!d) d = new Private; }

namespace XMPP {

using namespace StunUtil;

// some attribute types we need to explicitly support
enum
{
	AttribMessageIntegrity = 0x0008,
	AttribFingerprint      = 0x8028
};

// adapted from public domain source by Ross Williams and Eric Durbin
unsigned long crctable[256] =
{
	0x00000000L, 0x77073096L, 0xEE0E612CL, 0x990951BAL,
	0x076DC419L, 0x706AF48FL, 0xE963A535L, 0x9E6495A3L,
	0x0EDB8832L, 0x79DCB8A4L, 0xE0D5E91EL, 0x97D2D988L,
	0x09B64C2BL, 0x7EB17CBDL, 0xE7B82D07L, 0x90BF1D91L,
	0x1DB71064L, 0x6AB020F2L, 0xF3B97148L, 0x84BE41DEL,
	0x1ADAD47DL, 0x6DDDE4EBL, 0xF4D4B551L, 0x83D385C7L,
	0x136C9856L, 0x646BA8C0L, 0xFD62F97AL, 0x8A65C9ECL,
	0x14015C4FL, 0x63066CD9L, 0xFA0F3D63L, 0x8D080DF5L,
	0x3B6E20C8L, 0x4C69105EL, 0xD56041E4L, 0xA2677172L,
	0x3C03E4D1L, 0x4B04D447L, 0xD20D85FDL, 0xA50AB56BL,
	0x35B5A8FAL, 0x42B2986CL, 0xDBBBC9D6L, 0xACBCF940L,
	0x32D86CE3L, 0x45DF5C75L, 0xDCD60DCFL, 0xABD13D59L,
	0x26D930ACL, 0x51DE003AL, 0xC8D75180L, 0xBFD06116L,
	0x21B4F4B5L, 0x56B3C423L, 0xCFBA9599L, 0xB8BDA50FL,
	0x2802B89EL, 0x5F058808L, 0xC60CD9B2L, 0xB10BE924L,
	0x2F6F7C87L, 0x58684C11L, 0xC1611DABL, 0xB6662D3DL,
	0x76DC4190L, 0x01DB7106L, 0x98D220BCL, 0xEFD5102AL,
	0x71B18589L, 0x06B6B51FL, 0x9FBFE4A5L, 0xE8B8D433L,
	0x7807C9A2L, 0x0F00F934L, 0x9609A88EL, 0xE10E9818L,
	0x7F6A0DBBL, 0x086D3D2DL, 0x91646C97L, 0xE6635C01L,
	0x6B6B51F4L, 0x1C6C6162L, 0x856530D8L, 0xF262004EL,
	0x6C0695EDL, 0x1B01A57BL, 0x8208F4C1L, 0xF50FC457L,
	0x65B0D9C6L, 0x12B7E950L, 0x8BBEB8EAL, 0xFCB9887CL,
	0x62DD1DDFL, 0x15DA2D49L, 0x8CD37CF3L, 0xFBD44C65L,
	0x4DB26158L, 0x3AB551CEL, 0xA3BC0074L, 0xD4BB30E2L,
	0x4ADFA541L, 0x3DD895D7L, 0xA4D1C46DL, 0xD3D6F4FBL,
	0x4369E96AL, 0x346ED9FCL, 0xAD678846L, 0xDA60B8D0L,
	0x44042D73L, 0x33031DE5L, 0xAA0A4C5FL, 0xDD0D7CC9L,
	0x5005713CL, 0x270241AAL, 0xBE0B1010L, 0xC90C2086L,
	0x5768B525L, 0x206F85B3L, 0xB966D409L, 0xCE61E49FL,
	0x5EDEF90EL, 0x29D9C998L, 0xB0D09822L, 0xC7D7A8B4L,
	0x59B33D17L, 0x2EB40D81L, 0xB7BD5C3BL, 0xC0BA6CADL,
	0xEDB88320L, 0x9ABFB3B6L, 0x03B6E20CL, 0x74B1D29AL,
	0xEAD54739L, 0x9DD277AFL, 0x04DB2615L, 0x73DC1683L,
	0xE3630B12L, 0x94643B84L, 0x0D6D6A3EL, 0x7A6A5AA8L,
	0xE40ECF0BL, 0x9309FF9DL, 0x0A00AE27L, 0x7D079EB1L,
	0xF00F9344L, 0x8708A3D2L, 0x1E01F268L, 0x6906C2FEL,
	0xF762575DL, 0x806567CBL, 0x196C3671L, 0x6E6B06E7L,
	0xFED41B76L, 0x89D32BE0L, 0x10DA7A5AL, 0x67DD4ACCL,
	0xF9B9DF6FL, 0x8EBEEFF9L, 0x17B7BE43L, 0x60B08ED5L,
	0xD6D6A3E8L, 0xA1D1937EL, 0x38D8C2C4L, 0x4FDFF252L,
	0xD1BB67F1L, 0xA6BC5767L, 0x3FB506DDL, 0x48B2364BL,
	0xD80D2BDAL, 0xAF0A1B4CL, 0x36034AF6L, 0x41047A60L,
	0xDF60EFC3L, 0xA867DF55L, 0x316E8EEFL, 0x4669BE79L,
	0xCB61B38CL, 0xBC66831AL, 0x256FD2A0L, 0x5268E236L,
	0xCC0C7795L, 0xBB0B4703L, 0x220216B9L, 0x5505262FL,
	0xC5BA3BBEL, 0xB2BD0B28L, 0x2BB45A92L, 0x5CB36A04L,
	0xC2D7FFA7L, 0xB5D0CF31L, 0x2CD99E8BL, 0x5BDEAE1DL,
	0x9B64C2B0L, 0xEC63F226L, 0x756AA39CL, 0x026D930AL,
	0x9C0906A9L, 0xEB0E363FL, 0x72076785L, 0x05005713L,
	0x95BF4A82L, 0xE2B87A14L, 0x7BB12BAEL, 0x0CB61B38L,
	0x92D28E9BL, 0xE5D5BE0DL, 0x7CDCEFB7L, 0x0BDBDF21L,
	0x86D3D2D4L, 0xF1D4E242L, 0x68DDB3F8L, 0x1FDA836EL,
	0x81BE16CDL, 0xF6B9265BL, 0x6FB077E1L, 0x18B74777L,
	0x88085AE6L, 0xFF0F6A70L, 0x66063BCAL, 0x11010B5CL,
	0x8F659EFFL, 0xF862AE69L, 0x616BFFD3L, 0x166CCF45L,
	0xA00AE278L, 0xD70DD2EEL, 0x4E048354L, 0x3903B3C2L,
	0xA7672661L, 0xD06016F7L, 0x4969474DL, 0x3E6E77DBL,
	0xAED16A4AL, 0xD9D65ADCL, 0x40DF0B66L, 0x37D83BF0L,
	0xA9BCAE53L, 0xDEBB9EC5L, 0x47B2CF7FL, 0x30B5FFE9L,
	0xBDBDF21CL, 0xCABAC28AL, 0x53B39330L, 0x24B4A3A6L,
	0xBAD03605L, 0xCDD70693L, 0x54DE5729L, 0x23D967BFL,
	0xB3667A2EL, 0xC4614AB8L, 0x5D681B02L, 0x2A6F2B94L,
	0xB40BBE37L, 0xC30C8EA1L, 0x5A05DF1BL, 0x2D02EF8DL
};

class Crc32
{
private:
	quint32 result;

public:
	Crc32()
	{
		clear();
	}

	void clear()
	{
		result = 0xffffffff;
	}

	void update(const QByteArray &in)
	{
		for(int n = 0; n < in.size(); ++n)
			result = (result >> 8) ^ (crctable[(result & 0xff) ^ (quint8)in[n]]);
	}

	quint32 final()
	{
		return result ^= 0xffffffff;
	}

	static quint32 process(const QByteArray &in)
	{
		Crc32 c;
		c.update(in);
		return c.final();
	}
};

static quint8 magic_cookie[4] = { 0x21, 0x12, 0xA4, 0x42 };

// do 3-field check of stun packet
// returns length of packet not counting the header, or -1 on error
static int check_and_get_length(const QByteArray &buf)
{
	// stun packets are at least 20 bytes
	if(buf.size() < 20)
		return -1;

	// minimal 3-field check

	// top 2 bits of packet must be 0
	if(buf[0] & 0xC0)
		return -1;

	const quint8 *p = (const quint8 *)buf.data();
	quint16 mlen = read16(p + 2);

	// bottom 2 bits of message length field must be 0
	if(mlen & 0x03)
		return -1;

	// (also, the message length should be a reasonable size)
	if(mlen + 20 > buf.size())
		return -1;

	// magic cookie must be set
	if(memcmp(p + 4, magic_cookie, 4) != 0)
		return -1;

	return mlen;
}

#define ATTRIBUTE_AREA_START  20
#define ATTRIBUTE_AREA_MAX    65535
#define ATTRIBUTE_VALUE_MAX   65531

// note: because the attribute area of the packet has a maximum size of
//   2^16-1, and each attribute itself has a 4 byte header, it follows that
//   the maximum size of an attribute's value is 2^16-5.  this means that,
//   even if padded with up to 3 bytes, the physical size of an attribute's
//   value will not overflow a 16-bit unsigned integer.
static quint16 round_up_length(quint16 in)
{
	Q_ASSERT(in <= ATTRIBUTE_VALUE_MAX);
	quint16 out = in;
	quint16 remainder = out % 4;
	if(remainder != 0)
		out += (4 - remainder);
	return out;
}

// buf    = entire stun packet
// offset = byte index of current attribute (first is offset=20)
// type   = take attribute type
// len    = take attribute value length (value is at offset + 4)
// returns offset of next attribute, -1 if no more
static int get_attribute_props(const QByteArray &buf, int offset, quint16 *type, int *len)
{
	Q_ASSERT(offset >= ATTRIBUTE_AREA_START);

	const quint8 *p = (const quint8 *)buf.data();

	// need at least 4 bytes for an attribute
	if(offset + 4 > buf.size())
		return -1;

	quint16 _type = read16(p + offset);
	offset += 2;
	quint16 _alen = read16(p + offset);
	offset += 2;

	// get physical length.  stun attributes are 4-byte aligned, and may
	//   contain 0-3 bytes of padding.
	quint16 plen = round_up_length(_alen);
	if(offset + plen > buf.size())
		return -1;

	*type = _type;
	*len = _alen;
	return offset + plen;
}

// buf    = entire stun packet
// type   = attribute type to find
// len    = take attribute value length (value is at offset + 4)
// next   = take offset of next attribute
// returns offset of found attribute, -1 if not found
static int find_attribute(const QByteArray &buf, quint16 type, int *len, int *next = 0)
{
	int at = ATTRIBUTE_AREA_START;
	quint16 _type;
	int _len;
	int _next;

	while(1)
	{
		_next = get_attribute_props(buf, at, &_type, &_len);
		if(_next == -1)
			break;
		if(_type == type)
		{
			*len = _len;
			if(next)
				*next = _next;
			return at;
		}
		at = _next;
	}

	return -1;
}

// buf  = stun packet to append attribute to
// type = type of attribute
// len  = length of value
// returns offset of new attribute, or -1 if it can't fit
// note: attribute value is located at offset + 4 and is uninitialized
// note: padding following attribute is zeroed out
static int append_attribute_uninitialized(QByteArray *buf, quint16 type, int len)
{
	if(len > ATTRIBUTE_VALUE_MAX)
		return -1;

	quint16 alen = (quint16)len;
	quint16 plen = round_up_length(alen);

	if((buf->size() - ATTRIBUTE_AREA_START) + 4 + plen > ATTRIBUTE_AREA_MAX)
		return -1;

	int at = buf->size();
	buf->resize(buf->size() + 4 + plen);
	quint8 *p = (quint8 *)buf->data();

	write16(p + at, type);
	write16(p + at + 2, alen);

	// padding
	for(int n = 0; n < plen - alen; ++n)
		p[at + alen + n] = 0;

	return at;
}

static quint32 fingerprint_calc(const quint8 *buf, int size)
{
	QByteArray region = QByteArray::fromRawData((const char *)buf, size);
	return Crc32::process(region) ^ 0x5354554e;
}

static QByteArray message_integrity_calc(const quint8 *buf, int size, const QByteArray &key)
{
	QCA::MessageAuthenticationCode hmac("hmac(sha1)", key);
	QByteArray region = QByteArray::fromRawData((const char *)buf, size);
	QByteArray result = hmac.process(region).toByteArray();
	Q_ASSERT(result.size() == 20);
	return result;
}

// look for fingerprint attribute and confirm it
// buf = entire stun packet
// returns true if fingerprint attribute exists and is correct
static bool fingerprint_check(const QByteArray &buf)
{
	int at, len;
	at = find_attribute(buf, AttribFingerprint, &len);
	if(at == -1 || len != 4) // value must be 4 bytes
		return false;

	const quint8 *p = (const quint8 *)buf.data();
	quint32 fpval = read32(p + at + 4);
	quint32 fpcalc = fingerprint_calc(p, at);
	if(fpval == fpcalc)
		return true;
	else
		return false;
}

// copy the input buffer and prepare for message integrity checking.  the
//   packet is truncated after the message-integrity attribute (since nothing
//   after it is protected), and the packet length is adjusted in the header
//   accordingly.
// buf    = input stun packet
// out    = take output stun packet
// offset = take offset of message-integrity attribute
// returns true if message-integrity attribute exists and packet is prepared
// note: message-integrity value is at offset + 4 and is exactly 20 bytes
static bool message_integrity_prep(const QByteArray &buf, QByteArray *out, int *offset)
{
	int at, len, next;
	at = find_attribute(buf, AttribMessageIntegrity, &len, &next);
	if(at == -1 || len != 20) // value must be 20 bytes
		return false;

	// prepare new attribute area size
	int i = next - ATTRIBUTE_AREA_START;

	// new value must be divisible by 4
	if(i % 4 != 0)
		return false;

	// copy truncated packet
	*out = buf.mid(0, next);

	// set new length in header
	quint16 newlen = (quint16)i;
	write16((quint8 *)out->data() + 2, newlen);

	*offset = at;
	return true;
}

// confirm message integrity
// buf    = prepared stun packet (from message_integrity_prep())
// offset = offset of message-integrity attribute
// key    = the HMAC key
// returns true if correct
static bool message_integrity_check(const QByteArray &buf, int offset, const QByteArray &key)
{
	QByteArray mival = QByteArray::fromRawData(buf.data() + offset + 4, 20);
	QByteArray micalc = message_integrity_calc((const quint8 *)buf.data(), offset, key);
	if(mival == micalc)
		return true;
	else
		return false;
}

class StunMessage::Private : public QSharedData
{
public:
	StunMessage::Class mclass;
	quint16 method;
	quint8 magic[4];
	quint8 id[12];
	QList<Attribute> attribs;

	Private()
	{
		mclass = (StunMessage::Class)-1;
		method = 0;
		memcpy(magic, magic_cookie, 4);
		memset(id, 0, 12);
	}
};

StunMessage::StunMessage() :
	d(0)
{
}

StunMessage::StunMessage(const StunMessage &from) :
	d(from.d)
{
}

StunMessage::~StunMessage()
{
}

StunMessage & StunMessage::operator=(const StunMessage &from)
{
	d = from.d;
	return *this;
}

bool StunMessage::isNull() const
{
	return (d ? false : true);
}

StunMessage::Class StunMessage::mclass() const
{
	Q_ASSERT(d);
	return d->mclass;
}

quint16 StunMessage::method() const
{
	Q_ASSERT(d);
	return d->method;
}

const quint8 *StunMessage::magic() const
{
	Q_ASSERT(d);
	return d->magic;
}

const quint8 *StunMessage::id() const
{
	Q_ASSERT(d);
	return d->id;
}

QList<StunMessage::Attribute> StunMessage::attributes() const
{
	Q_ASSERT(d);
	return d->attribs;
}

QByteArray StunMessage::attribute(quint16 type) const
{
	Q_ASSERT(d);

	foreach(const Attribute &i, d->attribs)
	{
		if(i.type == type)
			return i.value;
	}
	return QByteArray();
}

void StunMessage::setClass(Class mclass)
{
	ENSURE_D
	d->mclass = mclass;
}

void StunMessage::setMethod(quint16 method)
{
	ENSURE_D
	d->method = method;
}

void StunMessage::setMagic(const quint8 *magic)
{
	ENSURE_D
	memcpy(d->magic, magic, 4);
}

void StunMessage::setId(const quint8 *id)
{
	ENSURE_D
	memcpy(d->id, id, 12);
}

void StunMessage::setAttributes(const QList<Attribute> &attribs)
{
	ENSURE_D
	d->attribs = attribs;
}

QByteArray StunMessage::toBinary(int validationFlags, const QByteArray &key) const
{
	Q_ASSERT(d);

	// header
	QByteArray buf(20, 0);
	quint8 *p = (quint8 *)buf.data();

	quint8 classbits = 0;
	if(d->mclass == Request)
		classbits = 0; // 00
	else if(d->mclass == Indication)
		classbits = 1; // 01
	else if(d->mclass == SuccessResponse)
		classbits = 2; // 10
	else if(d->mclass == ErrorResponse)
		classbits = 3; // 11
	else
		Q_ASSERT(0);

	// method bits are split into 3 sections
	quint16 m1, m2, m3;
	m1 = d->method & 0x0f80; // M7-11
	m1 <<= 2;
	m2 = d->method & 0x0070; // M4-6
	m2 <<= 1;
	m3 = d->method & 0x000f; // M0-3

	// class bits are split into 2 sections
	quint16 c1, c2;
	c1 = classbits & 0x02; // C1
	c1 <<= 7;
	c2 = classbits & 0x01; // C0
	c2 <<= 4;

	quint16 type = m1 | m2 | m3 | c1 | c2;
	write16(p, type);
	write16(p + 2, 0);
	memcpy(p + 4, d->magic, 4);
	memcpy(p + 8, d->id, 12);

	foreach(const Attribute &i, d->attribs)
	{
		int at = append_attribute_uninitialized(&buf, i.type, i.value.size());
		if(at == -1)
			return QByteArray();

		p = (quint8 *)buf.data(); // follow the resize

		memcpy(buf.data() + at + 4, i.value.data(), i.value.size());
	}

	// set attribute area size
	write16(p + 2, buf.size() - ATTRIBUTE_AREA_START);

	if(validationFlags & MessageIntegrity)
	{
		quint16 alen = 20; // size of hmac(sha1)
		int at = append_attribute_uninitialized(&buf, AttribMessageIntegrity, alen);
		if(at == -1)
			return QByteArray();

		p = (quint8 *)buf.data(); // follow the resize

		// set attribute area size to include the new attribute
		write16(p + 2, buf.size() - ATTRIBUTE_AREA_START);

		// now calculate the hash and fill in the value
		QByteArray result = message_integrity_calc(p, at, key);
		Q_ASSERT(result.size() == alen);
		memcpy(p + at + 4, result.data(), alen);
	}

	if(validationFlags & Fingerprint)
	{
		quint16 alen = 4; // size of crc32
		int at = append_attribute_uninitialized(&buf, AttribFingerprint, alen);
		if(at == -1)
			return QByteArray();

		p = (quint8 *)buf.data(); // follow the resize

		// set attribute area size to include the new attribute
		write16(p + 2, buf.size() - ATTRIBUTE_AREA_START);

		// now calculate the fingerprint and fill in the value
		quint32 fpcalc = fingerprint_calc(p, at);
		write32(p + at + 4, fpcalc);
	}

	return buf;
}

StunMessage StunMessage::fromBinary(const QByteArray &a, ConvertResult *result, int validationFlags, const QByteArray &key)
{
	int mlen = check_and_get_length(a);
	if(mlen == -1)
	{
		if(result)
			*result = ErrorFormat;
		return StunMessage();
	}

	if(validationFlags & Fingerprint)
	{
		if(!fingerprint_check(a))
		{
			if(result)
				*result = ErrorFingerprint;
			return StunMessage();
		}
	}

	QByteArray in;

	if(validationFlags & MessageIntegrity)
	{
		int offset;
		if(!message_integrity_prep(a, &in, &offset))
		{
			if(result)
				*result = ErrorMessageIntegrity;
			return StunMessage();
		}

		if(!message_integrity_check(in, offset, key))
		{
			if(result)
				*result = ErrorMessageIntegrity;
			return StunMessage();
		}
	}
	else
		in = a;

	// all validating complete, now just parse the packet

	const quint8 *p = (const quint8 *)in.data();

	// method bits are split into 3 sections
	quint16 m1, m2, m3;
	m1 = p[0] & 0x3e; // M7-11
	m1 <<= 6;
	m2 = p[1] & 0xe0; // M4-6
	m2 >>= 1;
	m3 = p[1] & 0x0f; // M0-3

	// class bits are split into 2 sections
	quint8 c1, c2;
	c1 = p[0] & 0x01; // C1
	c1 <<= 1;
	c2 = p[1] & 0x10; // C0
	c2 >>= 4;

	quint16 method = m1 | m2 | m3;
	quint8 classbits = c1 | c2;

	Class mclass;
	if(classbits == 0) // 00
		mclass = Request;
	else if(classbits == 1) // 01
		mclass = Indication;
	else if(classbits == 2) // 10
		mclass = SuccessResponse;
	else // 11
		mclass = ErrorResponse;

	StunMessage out;
	out.setClass(mclass);
	out.setMethod(method);
	out.setMagic(p + 4);
	out.setId(p + 8);

	QList<Attribute> list;
	int at = ATTRIBUTE_AREA_START;
	while(1)
	{
		quint16 type;
		int len;
		int next;

		next = get_attribute_props(in, at, &type, &len);
		if(next == -1)
			break;

		Attribute attrib;
		attrib.type = type;
		attrib.value = in.mid(at + 4, len);
		list += attrib;

		at = next;
	}
	out.setAttributes(list);

	if(result)
		*result = ConvertGood;
	return out;
}

bool StunMessage::isProbablyStun(const QByteArray &a)
{
	return (check_and_get_length(a) != -1 ? true : false);
}

StunMessage::Class StunMessage::extractClass(const QByteArray &in)
{
	const quint8 *p = (const quint8 *)in.data();

	// class bits are split into 2 sections
	quint8 c1, c2;
	c1 = p[0] & 0x01; // C1
	c1 <<= 1;
	c2 = p[1] & 0x10; // C0
	c2 >>= 4;

	quint8 classbits = c1 | c2;

	Class mclass;
	if(classbits == 0) // 00
		mclass = Request;
	else if(classbits == 1) // 01
		mclass = Indication;
	else if(classbits == 2) // 10
		mclass = SuccessResponse;
	else // 11
		mclass = ErrorResponse;

	return mclass;
}

bool StunMessage::containsStun(const quint8 *data, int size)
{
	// check_and_get_length does a full packet check so it works even on a stream
	return (check_and_get_length(QByteArray::fromRawData((const char *)data, size)) != -1 ? true : false);
}

QByteArray StunMessage::readStun(const quint8 *data, int size)
{
	QByteArray in = QByteArray::fromRawData((const char *)data, size);
	int mlen = check_and_get_length(in);
	if(mlen != -1)
		return QByteArray((const char *)data, mlen + 20);
	else
		return QByteArray();
}

}
