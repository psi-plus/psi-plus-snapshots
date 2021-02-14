/*
 * jignle-transport.h - Base Jingle transport classes
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

#ifndef JINGLE_TRANSPORT_H
#define JINGLE_TRANSPORT_H

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
        enum Hint { AvoidRelays = 1 };
        Q_DECLARE_FLAGS(Hints, Hint)

        using Ptr = QSharedPointer<Connection>; // will be shared between transport and application
        virtual bool            hasPendingDatagrams() const;
        virtual NetworkDatagram receiveDatagram(qint64 maxSize = -1);
        virtual size_t          blockSize() const;
        virtual int             component() const;

        inline void  setHints(Hints hints) { _hints = hints; }
        inline Hints hints() const { return _hints; }

    signals:
        void connected();

    protected:
        Hints _hints;
    };
    Q_DECLARE_OPERATORS_FOR_FLAGS(Connection::Hints)

    class TransportManager;
    class TransportManagerPad : public SessionManagerPad {
        Q_OBJECT
    public:
        typedef QSharedPointer<TransportManagerPad> Ptr;

        virtual TransportManager *manager() const = 0;
    };

    class Transport : public QObject {
        Q_OBJECT
    public:
        Transport(TransportManagerPad::Ptr pad, Origin creator);

        /*enum Direction { // incoming or outgoing file/data transfer.
            Outgoing,
            Incoming
        };*/

        inline Origin                   creator() const { return _creator; }
        inline State                    state() const { return _state; }
        inline State                    prevState() const { return _prevState; }
        inline Reason                   lastReason() const { return _lastReason; }
        inline XMPP::Stanza::Error      lastError() const { return _lastError; }
        inline TransportManagerPad::Ptr pad() const { return _pad; }
        bool                            isRemote() const;
        inline bool                     isLocal() const { return !isRemote(); }

        /**
         * @brief prepare to send content-add/session-initiate
         *  When ready, the application first set update type to ContentAdd and then emit updated()
         */
        virtual void prepare() = 0;

        /**
         * @brief start really transfer data. starting with connection to remote candidates for example
         */
        virtual void start() = 0; // for local transport start searching for candidates (including probing proxy,stun
                                  // etc) for remote transport try to connect to all proposed hosts in order their
                                  // priority. in-band transport may just emit updated() here
        virtual void stop();
        virtual bool update(const QDomElement &el) = 0; // accepts transport element on incoming transport-info
        virtual bool hasUpdates() const            = 0;

        /**
         * @brief Get an session update from the transport which looks most appropriate
         * @param ensureTransportElement - return minimal transport element even if no updates.
         *                                 The parameters is mostly required to satisfy XEP-0166 requirement
         *                                 for <transport/> element in the <content/> element
         * @return dom element and optional task completion callback
         */
        virtual OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement = false) = 0;
        virtual bool                        isValid() const                                         = 0;

        // returns all the available transport features while addChannel() can use just a subset of them
        virtual TransportFeatures features() const = 0;

        // a component is basically is a subconnection usually with a dedicated IP port. A component may have one or
        // more channels. Components are pretty much generic but channels aren't. Channels give connection objects,
        // wbile components are rather indexes.

        /**
         * @brief maxSupportedComponents
         * @return max number of components >=1.
         *
         * If the function returns -1 then it assumes number of components is unlimited. Even so it may depend on some
         * factors like number of still available udp ports for example for udp based transport.
         */
        virtual int maxSupportedComponents() const;

        /**
         * @brief addComponent add another component to the transport connection
         * @return index of the component (starting from 1 - second component. see below)
         *
         * Note, component with index 0 is considered to be always added since it doesn't make sense to have a transport
         * w/o any component. So if the transport has just one component, the application should avoid calling this
         * function.
         */
        virtual int addComponent();

        /**
         * @brief maxSupportedChannelsPerComponent returns max number supported channels for specific features set
         * @param features - required features
         * @return number of channel. 0 if not supported for given features set or non-zero if suported.
         *
         * Transports MUST reimplement this method, otherwise by default it's 0 (the transport won't work)
         *
         * Channels transfer some specific data. For example if those are DataOriented channels then for ICE transport
         * we can assume max SCTP channels. Again for ICE and time-oriented channels it's 1 since the transport doesn't
         * handle any channel protocols in this case and it's up to the application to do any multiplexing.
         */
        virtual int maxSupportedChannelsPerComponent(TransportFeatures features) const;

        /**
         * @brief addChannel adds a channel to the component.
         * @param features   - required channel features. like DataOriented for example
         * @param component  - index of component to add the channel to.
         * @return connection object which eventually will fire `connected()` signal
         *
         * It's necessary to add components in advance since we always have at least one component.
         * For example a file transfer transport may just call `addChannel(TransportFeature::DataOriented)` to have
         * reliable connection object on the component 0. Transports not supporting components notation are considered
         * to support just one component with index 0
         */
        virtual Connection::Ptr addChannel(TransportFeatures features, int component = 0) const = 0;

        /**
         * @brief channels
         * @return the list of all added channels both local and remote
         */
        virtual QList<Connection::Ptr> channels() const = 0;
    signals:
        /**
         * found some candidates and they have to be sent. takeUpdate has to be called from this signal
         * handler. if it's just always ready then signal has to be sent at least once otherwise
         * session-initiate won't be sent.
         */
        void updated();
        void connected(); // this signal is for app logic. maybe to finally start drawing some progress bar
        void failed();    // transport ailed for whatever reason. aborted for example. _state will be State::Finished
        void stateChanged();

    protected:
        // just updates state and signals about the change. No any loggic attached to the new state
        void setState(State newState);

        State                               _state     = State::Created;
        State                               _prevState = State::Created;
        Origin                              _creator   = Origin::None;
        QSharedPointer<TransportManagerPad> _pad;
        Reason                              _lastReason;
        XMPP::Stanza::Error                 _lastError;
        int                                 _channelCount = 1;
    };

    // It's an available transports collection per application
    struct TransportSelector {
        virtual ~TransportSelector();
        // Allocate the most preferred transport from the set
        // Returned transport is removed from the list of available.
        virtual QSharedPointer<Transport> getNextTransport() = 0;

        // Allocate alike transport (e.g. we have remote transport but instead want to use our
        // own of the same type and similar parameters)
        // Returned transport is removed from the list of available.
        virtual QSharedPointer<Transport> getAlikeTransport(QSharedPointer<Transport> alike) = 0;

        // Checks if replacement old with newer is possible (e.g. calls canReplace) and removes
        // the newer transport from the list of available.
        // Returns false if impossible.
        virtual bool replace(QSharedPointer<Transport> old, QSharedPointer<Transport> newer) = 0;

        // Put transport back to the set for future use
        virtual void backupTransport(QSharedPointer<Transport>) = 0;

        // Where we can allocate another transport for a replacement
        virtual bool hasMoreTransports() const = 0;

        // Check where we can (still) use this transport for the application
        virtual bool hasTransport(QSharedPointer<Transport>) const = 0;

        /*
            >0: transport `a` is more preferred than `b`
            <0: transport `a` is less preferred
            =0: it's essentially the same transport, so hardly a replacement.
        */
        virtual int compare(QSharedPointer<Transport> a, QSharedPointer<Transport> b) const = 0;

        // Returns false if it's impossible to replace old with newer for example if the newer is
        // not supported or already proven to be useless.
        // Default implementation checks is thew newer transport is among remaining or same as old
        virtual bool canReplace(QSharedPointer<Transport> old, QSharedPointer<Transport> newer);
    };

    class TransportManager : public QObject {
        Q_OBJECT
    public:
        TransportManager(QObject *parent = nullptr);

        // may show more features than Transport instance. For example some transports may work in both reliable and not
        // reliable modes
        virtual TransportFeatures features() const              = 0;
        virtual void              setJingleManager(Manager *jm) = 0;

        // FIXME rename methods
        virtual QSharedPointer<Transport> newTransport(const TransportManagerPad::Ptr &pad, Origin creator) = 0;
        virtual TransportManagerPad *     pad(Session *session)                                             = 0;

        // this method is supposed to gracefully close all related sessions as a preparation for plugin unload for
        // example
        virtual void closeAll() = 0;

        virtual QStringList discoFeatures() const = 0;
    signals:
        void abortAllRequested(); // mostly used by transport instances to abort immediately
    };
}}

#endif
