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

#ifndef XMPP_BITSOFBINARY_H
#define XMPP_BITSOFBINARY_H

#include "xmpp/jid/jid.h"

#include <QDomElement>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QSharedDataPointer>

namespace XMPP {
    class Client;
    class JT_BitsOfBinary;

    class BoBData
    {
        class Private;
    public:
        BoBData();
        BoBData(const BoBData &other);
        BoBData(const QDomElement &);
        ~BoBData();
        BoBData &operator=(const BoBData &other);

        bool isNull() const;

        QString cid() const;
        void setCid(const QString &);

        QByteArray data() const;
        void setData(const QByteArray &);

        QString type() const;
        void setType(const QString &);

        unsigned int maxAge() const;
        void setMaxAge(unsigned int);

        void fromXml(const QDomElement &);
        QDomElement toXml(QDomDocument *doc) const;

    private:
        QSharedDataPointer<Private> d;
    };

    class BoBCache : public QObject
    {
        Q_OBJECT

    public:
        BoBCache(QObject *parent);
        virtual void put(const BoBData &) = 0;
        virtual BoBData get(const QString &) = 0;
    };

    class BoBManager : public QObject
    {
        Q_OBJECT

    public:
        BoBManager(Client *);
        void setCache(BoBCache*);

        BoBData bobData(const QString &);
        // file data, mime type, max age in seconds
        BoBData append(const QByteArray &data, const QString &type,
                            unsigned int maxAge = 0);
        QString append(QFile &file,
                             const QString &type = "application/octet-stream");
        void append(const BoBData &);

    private:
        BoBCache *_cache;
        QHash<QString, QPair<QString,QString> > _localFiles; //cid => (filename, mime)
    };
} // namespace XMPP

#endif // XMPP_BITSOFBINARY_H
