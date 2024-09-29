/*
 * xmpp_forwarding.h - Stanza Forwarding (XEP-0297)
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

#ifndef XMPP_FORWARDING_H
#define XMPP_FORWARDING_H

#include <memory>
#include <QObject>
#include <QDateTime>
#include <QDomElement>

#include "xmpp_message.h"

namespace XMPP
{
    class Client;
    class Stream;
    class Message;
    class JT_PushMessage;

    class Forwarding
    {
    public:
        Forwarding();
        Forwarding(const Forwarding &);
        ~Forwarding();

        Forwarding & operator=(const Forwarding &);

        enum Type {
            ForwardedNone,
            ForwardedMessage, // XEP-0297
            ForwardedCarbonsReceived, // XEP-0280
            ForwardedCarbonsSent, // XEP-0280
        };
        Type type() const;
        void setType(Type type);
        bool isCarbons() const;

        QDateTime timeStamp() const;
        void setTimeStamp(const QDateTime &ts);

        Message message() const;
        void setMessage(const Message &msg);

        bool fromXml(const QDomElement &e, Client *client);
        QDomElement toXml(Stream *stream) const;

    private:
        Type type_;
        QDateTime ts_;
        Message msg_;
    };

    class ForwardingManager : public QObject
    {
        Q_OBJECT

    public:
        ForwardingManager(JT_PushMessage *push_m);
        ForwardingManager(const ForwardingManager &) = delete;
        ForwardingManager & operator=(const ForwardingManager &) = delete;
        ~ForwardingManager();

        void setEnabled(bool enabled);
        bool isEnabled() const;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };

}

#endif
