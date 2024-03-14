/*
 * Copyright (C) 2010  Sergey Ilinykh
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "xmpp_bitsofbinary.h"

#include "xmpp_client.h"
#include "xmpp_hash.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"

#include <QCryptographicHash>
#include <QFile>

using namespace XMPP;

class BoBData::Private : public QSharedData {
public:
    QByteArray data; // file data itself
    QString    type; // mime type. e.g. image/png
    // QString cid;      // content identifier without "cid:"
    Hash         hash;
    unsigned int maxAge; // seconds to live
};

BoBData::BoBData() : d(new Private) { }

BoBData::BoBData(const BoBData &other) : d(other.d) { }

BoBData::BoBData(const QDomElement &e) : d(new Private) { fromXml(e); }

BoBData::~BoBData() { }

BoBData &BoBData::operator=(const BoBData &other)
{
    if (this == &other)
        return *this; // Protect against self-assignment
    d = other.d;
    return *this;
}

bool BoBData::isNull() const { return !d->hash.isValid() || d->data.isNull(); }

Hash BoBData::cidToHash(const QString &cid)
{
    if (!cid.endsWith(QLatin1String("@bob.xmpp.org")))
        return Hash();
    return Hash::from(QStringView{cid}.left(cid.size() - sizeof("@bob.xmpp.org") + 1));
}

QString BoBData::cid() const
{
    if (isNull())
        return QString();
    return QString("%1+%2@bob.xmpp.org").arg(d->hash.stringType(), QString::fromLatin1(d->hash.data().toHex()));
}

void BoBData::setCid(const QString &cid) { d->hash = cidToHash(cid); }

const Hash &BoBData::hash() const { return d->hash; }

void BoBData::setHash(const Hash &hash) { d->hash = hash; }

QByteArray BoBData::data() const { return d->data; }

void BoBData::setData(const QByteArray &data) { d->data = data; }

QString BoBData::type() const { return d->type; }

void BoBData::setType(const QString &type) { d->type = type; }

unsigned int BoBData::maxAge() const { return d->maxAge; }

void BoBData::setMaxAge(unsigned int maxAge) { d->maxAge = maxAge; }

void BoBData::fromXml(const QDomElement &data)
{
    setCid(data.attribute("cid"));
    d->maxAge = data.attribute("max-age").toUInt();
    d->type   = data.attribute("type");
    d->data   = QByteArray::fromBase64(data.text().replace("\n", "").toLatin1());
}

QDomElement BoBData::toXml(QDomDocument *doc) const
{
    QDomElement data = doc->createElementNS("urn:xmpp:bob", "data");
    data.setAttribute("cid", cid());
    data.setAttribute("max-age", d->maxAge);
    data.setAttribute("type", d->type);
    data.appendChild(doc->createTextNode(QString::fromLatin1(d->data.toBase64())));
    return data;
}

// ---------------------------------------------------------
// BoBCache
// ---------------------------------------------------------
BoBCache::BoBCache(QObject *parent) : QObject(parent) { }

//------------------------------------------------------------------------------
// BoBManager
//------------------------------------------------------------------------------
BoBManager::BoBManager(Client *client) : QObject(client), _cache(nullptr) { new JT_BoBServer(client->rootTask()); }

void BoBManager::setCache(BoBCache *cache) { _cache = cache; }

BoBData BoBManager::bobData(const QString &cid)
{
    BoBData bd;
    Hash    h = BoBData::cidToHash(cid);
    if (_cache) {
        bd = _cache->get(h);
    }
    if (!bd.isNull())
        return bd;
    auto it = _localFiles.find(h);
    if (it != _localFiles.end()) {
        QPair<QString, QString> fileData = it.value();
        QFile                   file(fileData.first);
        if (file.open(QIODevice::ReadOnly)) {
            bd.setHash(h);
            bd.setData(file.readAll());
            bd.setMaxAge(0);
            bd.setType(fileData.second);
        }
    }
    return bd;
}

BoBData BoBManager::append(const QByteArray &data, const QString &type, unsigned int maxAge)
{
    BoBData b;
    b.setHash(Hash::from(Hash::Sha1, data));
    b.setData(data);
    b.setMaxAge(maxAge);
    b.setType(type);
    if (_cache) {
        _cache->put(b);
    }
    return b;
}

XMPP::Hash BoBManager::append(QFile &file, const QString &type)
{
    bool isOpen = file.isOpen();
    if (isOpen || file.open(QIODevice::ReadOnly)) {
        Hash h = Hash::from(Hash::Sha1, &file);
        if (h.isValid()) {
            _localFiles[h] = QPair<QString, QString>(file.fileName(), type);
        }
        if (!isOpen) {
            file.close();
        }
        return h;
    }
    return XMPP::Hash();
}

void BoBManager::append(const BoBData &data)
{
    if (!data.isNull() && _cache) {
        _cache->put(data);
    }
}
