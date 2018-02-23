/*
 * subsets.h - Implementation of Result Set Management (XEP-0059)
 * Copyright (C) 2018  Aleksey Andreev
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

#ifndef XMPP_SUBSETS_H
#define XMPP_SUBSETS_H

#include <QDomDocument>

namespace XMPP
{
    class SubsetsClientManager
    {
    public:
        SubsetsClientManager();
        ~SubsetsClientManager();

        void reset();
        bool isValid() const;
        bool isFirst() const;
        bool isLast() const;
        int  count() const;
        void setMax(int max);

        void getCount();
        void getFirst();
        void getNext();
        void getLast();
        void getPrevious();
        void getByIndex();

        static QDomElement findElement(const QDomElement &el, bool child);
        bool updateFromElement(const QDomElement &el, int itemsCount);
        QDomElement makeQueryElement(QDomDocument *doc) const;

    private:
        class Private;
        Private *d;
    };
}

#endif
