/*
 * bytestream_manager.cpp - base class for bytestreams over xmpp
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

#include <QTimer>

#include "xmpp_bytestream.h"
#include "xmpp_client.h"

namespace XMPP
{

BytestreamManager::BytestreamManager(Client *parent)
	: QObject(parent)
{

}

BytestreamManager::~BytestreamManager()
{

}

QString BytestreamManager::genUniqueSID(const Jid &peer) const
{
	// get unused key
	QString sid;
	do {
		sid = QString("%1%2").arg(sidPrefix())
							 .arg(qrand() & 0xffff, 4, 16, QChar('0'));
	} while(!isAcceptableSID(peer, sid));
	return sid;
}

/**
 * Deletes conection in specified interval
 */
void BytestreamManager::deleteConnection(BSConnection *c, int msec)
{
	if (msec) {
		QTimer::singleShot(msec, c, SLOT(deleteLater()));
	}
	else {
		delete c;
	}
}

}