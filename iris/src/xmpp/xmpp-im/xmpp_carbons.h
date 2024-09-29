/*
 * xmpp_carbons.h - Message Carbons (XEP-0280)
 * Copyright (C) 2019  Aleksey Andreev
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

#ifndef XMPP_CARBONS_H
#define XMPP_CARBONS_H

#include <memory>
#include <QObject>

class QDomDocument;
class QDomElement;

namespace XMPP
{
    class Task;
    class Client;
    class JT_PushMessage;

    class CarbonsManager : public QObject
    {
        Q_OBJECT

    public:
        CarbonsManager(JT_PushMessage *push_m);
        CarbonsManager(const CarbonsManager &) = delete;
        CarbonsManager & operator=(const CarbonsManager &) = delete;
        ~CarbonsManager();

        static QDomElement privateElement(QDomDocument &doc);

        void setEnabled(bool enable);
        bool isEnabled() const;

    signals:
        void finished();

    private:
        class Private;
        std::unique_ptr<Private> d;
    };
}

#endif
