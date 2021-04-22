/*
 * jignle-connection.h - Jingle Connection - minimal data transfer unit for an application
 * Copyright (C) 2021  Sergey Ilinykh
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

#ifndef JINGLE_CONNECTION_H
#define JINGLE_CONNECTION_H

/**
 * A transport may have multiple connections.
 * For example an ICE transport may have up to 65537 connections (65535 data/sctp-channels + 2 raw)
 */

#include "bytestream.h"
#include "jingle.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
#include <QNetworkDatagram>
#else
#include <QHostAddress>
#endif

namespace XMPP { namespace Jingle {
#if QT_VERSION < QT_VERSION_CHECK(5, 8, 0)
    // stub implementation
    class NetworkDatagram {
    public:
        bool       _valid = false;
        QByteArray _data;
        inline NetworkDatagram(const QByteArray &data, const QHostAddress &destinationAddress = QHostAddress(),
                               quint16 port = 0) :
            _valid(true),
            _data(data)
        {
            Q_UNUSED(destinationAddress);
            Q_UNUSED(port)
        }
        inline NetworkDatagram() { }

        inline bool       isValid() const { return _valid; }
        inline QByteArray data() const { return _data; }
    };
#else
    typedef QNetworkDatagram NetworkDatagram;
#endif

    class Connection : public ByteStream {
        Q_OBJECT
    public:
        using Ptr = QSharedPointer<Connection>; // will be shared between transport and application
        virtual bool              hasPendingDatagrams() const;
        virtual NetworkDatagram   receiveDatagram(qint64 maxSize = -1);
        virtual bool              sendDatagram(const NetworkDatagram &data);
        virtual size_t            blockSize() const;
        virtual int               component() const;
        virtual TransportFeatures features() const = 0;

        inline void setId(const QString &id) { _id = id; }
        inline bool isRemote() const { return _isRemote; }
        inline void setRemote(bool value) { _isRemote = value; }

    signals:
        void connected();
        void disconnected();

    protected:
        qint64 writeData(const char *data, qint64 maxSize);
        qint64 readData(char *data, qint64 maxSize);

        bool    _isRemote = false;
        QString _id;
    };

    using ConnectionAcceptorCallback = std::function<bool(Connection::Ptr)>;
    struct ConnectionAcceptor {
        TransportFeatures          features;
        ConnectionAcceptorCallback callback;
        int                        componentIndex;
    };
}}

#endif // JINGLE_CONNECTION_H
