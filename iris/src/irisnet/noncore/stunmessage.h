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

#ifndef STUNMESSAGE_H
#define STUNMESSAGE_H

#include <QByteArray>
#include <QList>
#include <QSharedDataPointer>

namespace XMPP {

class StunMessage
{
public:
	enum Class
	{
		Request,
		SuccessResponse,
		ErrorResponse,
		Indication
	};

	enum ValidationFlags
	{
		Fingerprint      = 0x01,

		// you must have the hmac(sha1) algorithm in QCA to use
		MessageIntegrity = 0x02
	};

	enum ConvertResult
	{
		ConvertGood,
		ErrorFormat,
		ErrorFingerprint,
		ErrorMessageIntegrity,
		ErrorConvertUnknown = 64
	};

	class Attribute
	{
	public:
		quint16 type;
		QByteArray value;
	};

	StunMessage();
	StunMessage(const StunMessage &from);
	~StunMessage();
	StunMessage & operator=(const StunMessage &from);

	bool isNull() const;
	Class mclass() const;
	quint16 method() const;
	const quint8 *magic() const; // 4 bytes
	const quint8 *id() const; // 12 bytes
	QList<Attribute> attributes() const;

	// returns the first instance or null
	QByteArray attribute(quint16 type) const;

	void setClass(Class mclass);
	void setMethod(quint16 method);
	void setMagic(const quint8 *magic); // 4 bytes
	void setId(const quint8 *id); // 12 bytes
	void setAttributes(const QList<Attribute> &attribs);

	QByteArray toBinary(int validationFlags = 0, const QByteArray &key = QByteArray()) const;
	static StunMessage fromBinary(const QByteArray &a, ConvertResult *result = 0, int validationFlags = 0, const QByteArray &key = QByteArray());

	// minimal 3-field check
	static bool isProbablyStun(const QByteArray &a);

	// extract out the class value from a raw packet.  assumes that 'a' has
	//   already passed isProbablyStun()
	static Class extractClass(const QByteArray &a);

	// examine raw data, such as from a stream, to see if it contains a
	//   stun packet
	static bool containsStun(const quint8 *data, int size);

	// try to read a stun packet from the raw data, else return null.
	//   a successful result can be passed to fromBinary()
	static QByteArray readStun(const quint8 *data, int size);

private:
	class Private;
	QSharedDataPointer<Private> d;
};

}

#endif
