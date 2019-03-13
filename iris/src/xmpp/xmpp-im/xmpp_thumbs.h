/*
 * Copyright (C) 2019  Sergey Ilinykh
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

#ifndef XMPP_THUMBS_H
#define XMPP_THUMBS_H

#include <QString>
#include <QUrl>
#include <QDomElement>

#define XMPP_THUMBS_NS "urn:xmpp:thumbs:1" // TODO make nsdb.cpp/h with static declarations of all ns

namespace XMPP
{
    class Thumbnail
    {
    public:
        inline Thumbnail() : width(0), height(0) {}
        // data - for outgoing it's actual image data. for incoming - cid
        inline Thumbnail(const QByteArray &data,
                           const QString &mimeType = QString(),
                           quint32 width = 0, quint32 height = 0) :
            data(data), mimeType(mimeType),
            width(width), height(height) { }
        Thumbnail(const QDomElement &el);

        inline bool isValid() const { return uri.isValid(); }
        QDomElement toXml(QDomDocument *doc) const;

        QUrl       uri;
        QByteArray data;
        QString    mimeType;
        quint32    width;
        quint32    height;
    };
}

#endif
