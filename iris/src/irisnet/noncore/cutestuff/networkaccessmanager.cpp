/*
 * networkaccessmanager.cpp - NetworkAccessManager with some adv. features
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

#include <QNetworkRequest>
#include <QNetworkDiskCache>
#include <QBuffer>
#include <QRegExp>
#include <QNetworkReply>
#include <QtCrypto>

#include "networkaccessmanager.h"
#include "bytestream.h"
#include "bsocket.h"

/*

  NetworkAccessManagers objectives:
  1) Use QCA instead of Qt's QSslSocket
  2) Allow custom proxy types (http over xmpp for example?)
  3) Simple extensibility (scheme-based handlers?)
  optional objectives:
  1) proxy chains
*/


//--------------------------------------------
// NetworkAccessManagerPrivate
//--------------------------------------------
class NetworkAccessManagerPrivate : public QObject
{
	Q_OBJECT

public:
	NetworkAccessManager *q;
	QNetworkDiskCache cache;
	QHash<QString, NetworkSchemeHandler*> schemeHandlers;

public:
	NetworkAccessManagerPrivate(NetworkAccessManager *nam) :
		q(nam) {}

	QNetworkReply* createRequest(QNetworkAccessManager::Operation op,
								 const QNetworkRequest &req,
								 QIODevice *outgoingData);
};


//--------------------------------------------
// HttpNetworkReply
//--------------------------------------------
class HttpNetworkReply : public QNetworkReply
{
	Q_OBJECT

	BSocket *bs;
	QNetworkProxy proxy;

public:
	HttpNetworkReply(QNetworkAccessManager::Operation op,
					 const QNetworkRequest &req,
					 QIODevice *outgoingData,
					 NetworkAccessManagerPrivate *nam) :
		QNetworkReply(nam),
		bs(0)
	{
		Q_UNUSED(op);
		Q_UNUSED(req);
		Q_UNUSED(outgoingData);
	}

	qint64 readData(char *data, qint64 maxlen)
	{
		Q_UNUSED(data);
		Q_UNUSED(maxlen);
		return 0;
	}

	void abort()
	{

	}

//	void setProxy(const QNetworkProxy &proxy)
//	{
//		this->proxy = proxy;
//	}

//	void connect(const QUrl &url)
//	{
//		switch (this->proxy.type()) {
//		case QNetworkProxy::Socks5Proxy:
//		case QNetworkProxy::HttpProxy:
//		case QNetworkProxy::HttpCachingProxy:
//			Q_ASSERT("proxy connection is not implemented");
//			break;
//		default:
//			bs = new BSocket(this);

//			break;
//		}
//	}

private slots:

};


//--------------------------------------------
// NetworkAccessManagerPrivate
//--------------------------------------------
QNetworkReply* NetworkAccessManagerPrivate::createRequest(QNetworkAccessManager::Operation op,
							 const QNetworkRequest &req,
							 QIODevice *outgoingData)
{
	if (req.url().scheme() != "https" && req.url().scheme() != "http") {
		NetworkSchemeHandler *hander = schemeHandlers.value(req.url().scheme());
		if (hander) {
			return hander->createRequest(op, req, outgoingData);
		}
		return 0;
	}

	return new HttpNetworkReply(op, req, outgoingData, this);
}



//--------------------------------------------
// NetworkAccessManager
//--------------------------------------------
NetworkAccessManager::NetworkAccessManager(QObject *parent) :
	QNetworkAccessManager(parent),
	d(new NetworkAccessManagerPrivate(this))
{
}

NetworkAccessManager::~NetworkAccessManager()
{
	delete d;
}

QNetworkDiskCache &NetworkAccessManager::cache() const
{
	return d->cache;
}

void NetworkAccessManager::setSchemeHandler(const QString &scheme,
											NetworkSchemeHandler *handler)
{
	d->schemeHandlers[scheme] = handler;
}

QNetworkReply* NetworkAccessManager::createRequest(
		Operation op, const QNetworkRequest &req, QIODevice *outgoingData)
{
	QNetworkReply *reply = d->createRequest(op, req, outgoingData);
	if (!reply) {
		reply = QNetworkAccessManager::createRequest(op, req, outgoingData);
	}
	return reply;
}


#include "networkaccessmanager.moc"
