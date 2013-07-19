/*
 * httppoll.h - HTTP polling proxy
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

#ifndef CS_HTTPPOLL_H
#define CS_HTTPPOLL_H

#include "bytestream.h"

class QUrl;

// CS_NAMESPACE_BEGIN

class HttpPoll : public ByteStream
{
	Q_OBJECT
public:
	enum Error { ErrConnectionRefused = ErrCustom, ErrHostNotFound, ErrProxyConnect, ErrProxyNeg, ErrProxyAuth };
	HttpPoll(QObject *parent=0);
	~HttpPoll();

	virtual QAbstractSocket* abstractSocket() const;

	void setAuth(const QString &user, const QString &pass="");
	void connectToUrl(const QUrl &url);
	void connectToHost(const QString &proxyHost, int proxyPort, const QUrl &url);

	int pollInterval() const;
	void setPollInterval(int seconds);

	// from ByteStream
	bool isOpen() const;
	void close();

signals:
	void connected();
	void syncStarted();
	void syncFinished();

protected:
	int tryWrite();

private slots:
	void http_result();
	void http_error(int);
	void do_sync();

private:
	class Private;
	Private *d;

	void resetConnection(bool clear=false);
	QByteArray makePacket(const QString &ident, const QString &key, const QString &newkey, const QByteArray &block);
	void resetKey();
	const QString & getKey(bool *);
};

class HttpProxyPost : public QObject
{
	Q_OBJECT
public:
	enum Error { ErrConnectionRefused, ErrHostNotFound, ErrSocket, ErrProxyConnect, ErrProxyNeg, ErrProxyAuth };
	HttpProxyPost(QObject *parent=0);
	~HttpProxyPost();

	QAbstractSocket* abstractSocket() const;

	void setUseSsl(bool state);
	void setAuth(const QString &user, const QString &pass="");
	bool isActive() const;
	void post(const QString &proxyHost, int proxyPort, const QUrl &url, const QByteArray &data, bool asProxy=true);
	void stop();
	QByteArray body() const;
	QString getHeader(const QString &) const;

signals:
	void result();
	void error(int);

private slots:
	void sock_connected();
	void sock_connectionClosed();
	void sock_readyRead();
	void sock_error(int);
	void tls_readyRead();
	void tls_readyReadOutgoing();
	void tls_error();

private:
	class Private;
	Private *d;

	void resetConnection(bool clear=false);
	void processData(const QByteArray &block);
};

class HttpProxyGetStream : public QObject
{
	Q_OBJECT
public:
	enum Error { ErrConnectionRefused, ErrHostNotFound, ErrSocket, ErrProxyConnect, ErrProxyNeg, ErrProxyAuth };
	HttpProxyGetStream(QObject *parent=0);
	~HttpProxyGetStream();

	void setAuth(const QString &user, const QString &pass="");
	bool isActive() const;
	void get(const QString &proxyHost, int proxyPort, const QString &url, bool ssl=false, bool asProxy=false);
	void stop();
	QString getHeader(const QString &) const;
	int length() const; // -1 for unknown

signals:
	void handshaken();
	void dataReady(const QByteArray &buf);
	void finished();
	void error(int);

private slots:
	void sock_connected();
	void sock_connectionClosed();
	void sock_readyRead();
	void sock_error(int);

	void tls_readyRead();
	void tls_readyReadOutgoing();
	void tls_error();

private:
	class Private;
	Private *d;

	void resetConnection(bool clear=false);
	void processData(const QByteArray &block);
};

// CS_NAMESPACE_END

#endif
