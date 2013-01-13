/*
 * bytestream_manager.h - base class for bytestreams over xmpp
 * Copyright (C) 2003  Justin Karneges, Rion
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef BYTESTREAM_MANAGER_H
#define BYTESTREAM_MANAGER_H

#include <QObject>
#include "xmpp/jid/jid.h"
#include "bytestream.h"

namespace XMPP
{
	class Client;
	class BytestreamManager;

	class BSConnection : public ByteStream
	{
	public:
		enum Error { ErrRefused = ErrCustom, ErrConnect, ErrProxy, ErrSocket };
		enum State { Idle, Requesting, Connecting, WaitingForAccept, Active };

		BSConnection(QObject *parent = 0) : ByteStream(parent) {}

		virtual void connectToJid(const Jid &peer, const QString &sid) = 0;
		virtual void accept() = 0;

		virtual Jid peer() const = 0;
		virtual QString sid() const = 0;
		virtual BytestreamManager* manager() const = 0;
	};

	class BytestreamManager : public QObject
	{
		Q_OBJECT

	public:
		BytestreamManager(Client *);
		virtual ~BytestreamManager();

		virtual bool isAcceptableSID(const Jid &peer, const QString &sid) const = 0;
		QString genUniqueSID(const Jid &peer) const;
		virtual BSConnection* createConnection() = 0;
		virtual void deleteConnection(BSConnection *c, int msec = 0);

	protected:
		virtual const char* sidPrefix() const = 0;

	signals:
		void incomingReady();
	};
}

#endif
