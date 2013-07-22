/*
 * networkaccessmanager.h - NetworkAccessManager with some adv. features
 * Copyright (C) 2013 Il'inykh Sergey (rion)
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


#ifndef NETWORKACCESSMANAGER_H
#define NETWORKACCESSMANAGER_H

#include <QNetworkAccessManager>

class QNetworkDiskCache;
class QUrl;
class NetworkAccessManagerPrivate;
class ByteStream;


class NetworkSchemeHandler
{
public:
	virtual ~NetworkSchemeHandler() { }
	virtual QNetworkReply* createRequest(QNetworkAccessManager::Operation op,
										 const QNetworkRequest & req,
										 QIODevice * outgoingData = 0) = 0;
};


class NetworkAccessManager : public QNetworkAccessManager
{
	Q_OBJECT
public:
	explicit NetworkAccessManager(QObject *parent = 0);
	~NetworkAccessManager();

	QNetworkDiskCache &cache() const;
	void setSchemeHandler(const QString &scheme, NetworkSchemeHandler *);

protected:
	QNetworkReply* createRequest(Operation op, const QNetworkRequest &req,
								 QIODevice *outgoingData = 0);

signals:

public slots:

private:
	NetworkAccessManagerPrivate *d;
};

#endif // NETWORKACCESSMANAGER_H
