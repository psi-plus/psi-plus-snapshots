/*
 * httpconnect.h - HTTP "CONNECT" proxy
 * Copyright (C) 2003  Justin Karneges
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

#ifndef CS_HTTPCONNECT_H
#define CS_HTTPCONNECT_H

#include "bytestream.h"

// CS_NAMESPACE_BEGIN

class HttpConnect : public ByteStream
{
	Q_OBJECT
public:
	enum Error { ErrConnectionRefused = ErrCustom, ErrHostNotFound, ErrProxyConnect, ErrProxyNeg, ErrProxyAuth };
	HttpConnect(QObject *parent=0);
	~HttpConnect();

	void setAuth(const QString &user, const QString &pass="");
	void connectToHost(const QString &proxyHost, int proxyPort, const QString &host, int port);

	// from ByteStream
	void close();
	qint64 bytesToWrite() const;
protected:
	qint64 writeData(const char *data, qint64 maxSize);

signals:
	void connected();

private slots:
	void sock_connected();
	void sock_connectionClosed();
	void sock_delayedCloseFinished();
	void sock_readyRead();
	void sock_bytesWritten(qint64);
	void sock_error(int);

private:
	class Private;
	Private *d;

	void resetConnection(bool clear=false);
};

// CS_NAMESPACE_END

#endif
