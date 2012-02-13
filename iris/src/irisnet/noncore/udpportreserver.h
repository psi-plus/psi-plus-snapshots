/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#ifndef UDPPORTRESERVER_H
#define UDPPORTRESERVER_H

#include <QObject>
#include <QList>

class QHostAddress;
class QUdpSocket;

namespace XMPP {

// call both setAddresses() and setPorts() at least once for socket
//   reservations to occur.  at any time you can update the list of addresses
//   (interfaces) and ports to reserve.  note that the port must be available
//   on all addresses in order for it to get reserved.
// note: you must return all sockets back to this class before destructing
class UdpPortReserver : public QObject
{
	Q_OBJECT

public:
	UdpPortReserver(QObject *parent = 0);
	~UdpPortReserver();

	void setAddresses(const QList<QHostAddress> &addrs);
	void setPorts(int start, int len);
	void setPorts(const QList<int> &ports);

	// return true if all ports got reserved, false if only some
	//   or none got reserved
	bool reservedAll() const;

	// may return less than asked for, if we had less reserved ports
	//   left. some attempt is made to return aligned or consecutive port
	//   values, but this is just a best effort and not a guarantee.  if
	//   not all ports were able to be reserved earlier, then this call
	//   may attempt to reserve those ports again.  the sockets in the
	//   returned list are ordered by port (in ascending order) and then
	//   by address (in the order provided).  since all addresses must be
	//   able to bind to a port for it to be considered reserved, this
	//   function always returns a list with a size that is a multiple of
	//   the number of addresses.
	QList<QUdpSocket*> borrowSockets(int portCount, QObject *parent = 0);

	void returnSockets(const QList<QUdpSocket*> &sockList);

private:
	class Private;
	Private *d;
};

}

#endif
