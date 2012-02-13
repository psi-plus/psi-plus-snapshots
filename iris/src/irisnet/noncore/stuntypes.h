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

#ifndef STUNTYPES_H
#define STUNTYPES_H

#include <QString>
#include <QByteArray>
#include <QList>
#include <QHostAddress>
#include "stunmessage.h"

namespace XMPP {

namespace StunTypes {

enum Method
{
	Binding           = 0x001,
	Allocate          = 0x003,
	Refresh           = 0x004,
	Send              = 0x006,
	Data              = 0x007,
	CreatePermission  = 0x008,
	ChannelBind       = 0x009
};

enum Attribute
{
	MAPPED_ADDRESS       = 0x0001,
	USERNAME             = 0x0006,
	MESSAGE_INTEGRITY    = 0x0008,
	ERROR_CODE           = 0x0009,
	UNKNOWN_ATTRIBUTES   = 0x000a,
	REALM                = 0x0014,
	NONCE                = 0x0015,
	XOR_MAPPED_ADDRESS   = 0x0020,
	CHANNEL_NUMBER       = 0x000c,
	LIFETIME             = 0x000d,
	XOR_PEER_ADDRESS     = 0x0012,
	DATA                 = 0x0013,
	XOR_RELAYED_ADDRESS  = 0x0016,
	EVEN_PORT            = 0x0018,
	REQUESTED_TRANSPORT  = 0x0019,
	DONT_FRAGMENT        = 0x001a,
	RESERVATION_TOKEN    = 0x0022,

	PRIORITY             = 0x0024,
	USE_CANDIDATE        = 0x0025,

	SOFTWARE             = 0x8022,
	ALTERNATE_SERVER     = 0x8023,
	FINGERPRINT          = 0x8028,

	ICE_CONTROLLED       = 0x8029,
	ICE_CONTROLLING      = 0x802a
};

enum Error
{
	TryAlternate                  = 300,
	BadRequest                    = 400,
	Unauthorized                  = 401,
	UnknownAttribute              = 420,
	StaleNonce                    = 438,
	ServerError                   = 500,

	Forbidden                     = 403,
	AllocationMismatch            = 437,
	WrongCredentials              = 441,
	UnsupportedTransportProtocol  = 442,
	AllocationQuotaReached        = 486,
	InsufficientCapacity          = 508,

	RoleConflict                  = 487
};

QByteArray createMappedAddress(const QHostAddress &addr, quint16 port);
QByteArray createUsername(const QString &username);
QByteArray createErrorCode(int code, const QString &reason);
QByteArray createUnknownAttributes(const QList<quint16> &typeList);
QByteArray createRealm(const QString &realm);
QByteArray createNonce(const QString &nonce);
QByteArray createXorMappedAddress(const QHostAddress &addr, quint16 port, const quint8 *magic, const quint8 *id);
QByteArray createChannelNumber(quint16 i);
QByteArray createLifetime(quint32 i);
QByteArray createXorPeerAddress(const QHostAddress &addr, quint16 port, const quint8 *magic, const quint8 *id);
QByteArray createXorRelayedAddress(const QHostAddress &addr, quint16 port, const quint8 *magic, const quint8 *id);
QByteArray createEvenPort(bool reserve);
QByteArray createRequestedTransport(quint8 proto);
QByteArray createReservationToken(const QByteArray &token);
QByteArray createPriority(quint32 i);
QByteArray createSoftware(const QString &str);
QByteArray createAlternateServer(const QHostAddress &addr, quint16 port);
QByteArray createIceControlled(quint64 i);
QByteArray createIceControlling(quint64 i);

bool parseMappedAddress(const QByteArray &val, QHostAddress *addr, quint16 *port);
bool parseUsername(const QByteArray &val, QString *username);
bool parseErrorCode(const QByteArray &val, int *code, QString *reason);
bool parseUnknownAttributes(const QByteArray &val, QList<quint16> *typeList);
bool parseRealm(const QByteArray &val, QString *realm);
bool parseNonce(const QByteArray &val, QString *nonce);
bool parseXorMappedAddress(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port);
bool parseChannelNumber(const QByteArray &val, quint16 *i);
bool parseLifetime(const QByteArray &val, quint32 *i);
bool parseXorPeerAddress(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port);
bool parseXorRelayedAddress(const QByteArray &val, const quint8 *magic, const quint8 *id, QHostAddress *addr, quint16 *port);
bool parseEvenPort(const QByteArray &val, bool *reserve);
bool parseRequestedTransport(const QByteArray &val, quint8 *proto);
bool parseReservationToken(const QByteArray &val, QByteArray *token);
bool parsePriority(const QByteArray &val, quint32 *i);
bool parseSoftware(const QByteArray &val, QString *str);
bool parseAlternateServer(const QByteArray &val, QHostAddress *addr, quint16 *port);
bool parseIceControlled(const QByteArray &val, quint64 *i);
bool parseIceControlling(const QByteArray &val, quint64 *i);

QString methodToString(int method);
QString attributeTypeToString(int type);
QString attributeValueToString(int type, const QByteArray &val, const quint8 *magic, const quint8 *id);

QString print_packet_str(const StunMessage &message);
void print_packet(const StunMessage &message);

}

}

#endif
