/*
 * xmpp_reference.h - XMPP References / XEP-0372
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPPREFERENCE_H
#define XMPPREFERENCE_H

#include "jingle-ft.h"

#include <QDomElement>
#include <QSharedPointer>

namespace XMPP {
    extern const QString MEDIASHARING_NS;
    extern const QString REFERENCE_NS;

    class MediaSharing
    {
    public:
        Jingle::FileTransfer::File file;
        QStringList sources;

        inline bool isValid() const { return file.isValid(); }
    };

    class Reference
    {
    public:
        enum Type : char {

            Mention,
            Data
        };

        Reference();
        Reference(Type type, const QString &uri);
        ~Reference();
        Reference(const Reference &other);
        Reference &operator=(const Reference &other);

        bool isValid() const { return d != nullptr; }
        Type type() const;
        const QString &uri() const;

        void setRange(int begin, int end);
        int begin() const;
        int end() const;

        const QString &anchor() const;
        void setAnchor(const QString &anchor);

        void setMediaSharing(const MediaSharing &);
        const MediaSharing &mediaSharing() const;

        bool fromXml(const QDomElement &e);
        QDomElement toXml(QDomDocument *) const;

    private:
        class Private;
        QSharedDataPointer<Private> d;
    };
} // namespace XMPP

#endif // XMPPREFERENCE_H
