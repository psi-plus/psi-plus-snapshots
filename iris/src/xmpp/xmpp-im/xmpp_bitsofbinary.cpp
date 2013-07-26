/*
 * Copyright (C) 2010 Rion
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "xmpp_bitsofbinary.h"
#include "xmpp_xmlcommon.h"
#include "xmpp_client.h"
#include "xmpp_tasks.h"
#include <QCryptographicHash>
#include <QFile>

using namespace XMPP;

class BoBData::Private : public QSharedData
{
public:
	QByteArray data;
	QString type;
	QString cid;
	unsigned int maxAge;
};

BoBData::BoBData()
	: d(new Private)
{

}

BoBData::BoBData(const BoBData &other)
	: d(other.d)
{

}

BoBData::BoBData(const QDomElement &e)
	: d(new Private)
{
	fromXml(e);
}

BoBData::~BoBData()
{

}

BoBData & BoBData::operator=(const BoBData &other)
{
	if (this==&other) return *this; //Protect against self-assignment
	d = other.d;
	return *this;
}

bool BoBData::isNull() const
{
	return d->cid.isEmpty() || d->data.isNull();
}

QString BoBData::cid() const
{
	return d->cid;
}

void BoBData::setCid(const QString &cid)
{
	d->cid = cid;
}

QByteArray BoBData::data() const
{
	return d->data;
}

void BoBData::setData(const QByteArray &data)
{
	d->data = data;
}

QString BoBData::type() const
{
	return d->type;
}

void BoBData::setType(const QString &type)
{
	d->type = type;
}

unsigned int BoBData::maxAge() const
{
	return d->maxAge;
}

void BoBData::setMaxAge(unsigned int maxAge)
{
	d->maxAge = maxAge;
}

void BoBData::fromXml(const QDomElement &data)
{
	d->cid = data.attribute("cid");
	d->maxAge = data.attribute("max-age").toInt();
	d->type = data.attribute("type");
	d->data = QCA::Base64().stringToArray(data.text().replace("\n",""))
			.toByteArray();
}

QDomElement BoBData::toXml(QDomDocument *doc) const
{
	QDomElement data = doc->createElement("data");
	data.setAttribute("xmlns", "urn:xmpp:bob");
	data.setAttribute("cid", d->cid);
	data.setAttribute("max-age", d->maxAge);
	data.setAttribute("type", d->type);
	data.appendChild(doc->createTextNode(QCA::Base64().arrayToString(d->data)));
	return data;
}



// ---------------------------------------------------------
// BoBCache
// ---------------------------------------------------------
BoBCache::BoBCache(QObject *parent)
	: QObject(parent)
{

}


//------------------------------------------------------------------------------
// BoBManager
//------------------------------------------------------------------------------
BoBManager::BoBManager(Client *client)
	: QObject(client)
	, _cache(0)
{
	new JT_BoBServer(client->rootTask());
}

void BoBManager::setCache(BoBCache *cache)
{
	_cache = cache;
}

BoBData BoBManager::bobData(const QString &cid)
{
	BoBData bd;
	if (_cache) {
		bd = _cache->get(cid);
	}
	if (bd.isNull() && _localFiles.contains(cid)) {
		QPair<QString, QString> fileData = _localFiles.value(cid);
		QFile file(fileData.first);
		if (file.open(QIODevice::ReadOnly)) {
			bd.setCid(cid);
			bd.setData(file.readAll());
			bd.setMaxAge(0);
			bd.setType(fileData.second);
		}
	}
	return bd;
}

BoBData BoBManager::append(const QByteArray &data, const QString &type,
								unsigned int maxAge)
{
	BoBData b;
	b.setCid(QString("sha1+%1@bob.xmpp.org").arg(QString(
		QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex())));
	b.setData(data);
	b.setMaxAge(maxAge);
	b.setType(type);
	if (_cache) {
		_cache->put(b);
	}
	return b;
}

QString BoBManager::append(QFile &file, const QString &type)
{
	bool isOpen = file.isOpen();
	if (isOpen || file.open(QIODevice::ReadOnly)) {
		QString cid = QString("sha1+%1@bob.xmpp.org").arg(
			QString(QCryptographicHash::hash(file.readAll(),
											QCryptographicHash::Sha1).toHex()));
		_localFiles[cid] = QPair<QString,QString>(file.fileName(), type);
		if (!isOpen) {
			file.close();
		}
		return cid;
	}
	return QString();
}

void BoBManager::append(const BoBData &data)
{
	if (!data.isNull() && _cache) {
		_cache->put(data);
	}
}
