/*
 * jignle-ibb.h - Jingle In-Band Bytestream transport
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

#ifndef JINGLEIBB_H
#define JINGLEIBB_H

#include "jingle-transport.h"

namespace XMPP {
class IBBConnection;

namespace Jingle { namespace IBB {
    extern const QString NS;

    class Transport : public XMPP::Jingle::Transport {
        Q_OBJECT
    public:
        Transport(const TransportManagerPad::Ptr &pad, Origin creator);
        ~Transport() override;

        void                        prepare() override;
        void                        start() override;
        bool                        update(const QDomElement &transportEl) override;
        bool                        hasUpdates() const override;
        OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement = false) override;
        bool                        isValid() const override;
        TransportFeatures           features() const override;
        int                         maxSupportedChannelsPerComponent(TransportFeatures features) const override;

        Connection::Ptr        addChannel(TransportFeatures features, const QString &id, int component = 0) override;
        QList<Connection::Ptr> channels() const override;

    private:
        friend class Manager;

        struct Private;
        QScopedPointer<Private> d;
    };

    class Manager;
    class Pad : public TransportManagerPad {
        Q_OBJECT
        // TODO
    public:
        typedef QSharedPointer<Pad> Ptr;

        Pad(Manager *manager, Session *session);
        QString           ns() const override;
        Session *         session() const override;
        TransportManager *manager() const override;

        Connection::Ptr makeConnection(const QString &sid, size_t blockSize);

    private:
        Manager *_manager = nullptr;
        Session *_session = nullptr;
    };

    class Manager : public TransportManager {
        Q_OBJECT
    public:
        Manager(QObject *parent = nullptr);
        ~Manager() override;

        XMPP::Jingle::TransportFeatures         features() const override;
        void                                    setJingleManager(XMPP::Jingle::Manager *jm) override;
        QSharedPointer<XMPP::Jingle::Transport> newTransport(const TransportManagerPad::Ptr &pad,
                                                             Origin                          creator) override;
        TransportManagerPad *                   pad(Session *session) override;

        QStringList discoFeatures() const override;

        Connection::Ptr makeConnection(const Jid &peer, const QString &sid, size_t blockSize);
        bool            handleIncoming(IBBConnection *c);

    private:
        struct Private;
        QScopedPointer<Private> d;
    };
} // namespace IBB
} // namespace Jingle
} // namespace XMPP

#endif // JINGLEIBB_H
