/*
 * jignle-application.h - Base Jingle application classes
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

#ifndef JINGLE_APPLICATION_H
#define JINGLE_APPLICATION_H

#include "jingle-transport.h"

class QTimer;

namespace XMPP { namespace Jingle {

    class ApplicationManager;
    class ApplicationManagerPad : public SessionManagerPad {
        Q_OBJECT
    public:
        typedef QSharedPointer<ApplicationManagerPad> Ptr;

        using SessionManagerPad::SessionManagerPad;

        virtual ApplicationManager *manager() const = 0;

        /*
         * for example we transfer a file
         * then first file may generate name "file1", next "file2" etc
         * As result it will be sent as <content name="file1" ... >
         */
        virtual QString generateContentName(Origin senders) = 0;

        virtual bool incomingSessionInfo(const QDomElement &el);
    };

    // Represents a session for single application. for example a single file in a file transfer session.
    // There maybe multiple application instances in a session.
    // It's designed as QObject to exposed to JavaScript (qml/webkit)
    class Application : public QObject {
        Q_OBJECT
    public:
        struct Update {
            Action action;
            Reason reason;
        };

        enum SetDescError {
            Ok,
            Unparsed,
            IncompatibleParameters // this one is for <reason>
        };

        enum ApplicationFlag {
            InitialApplication = 0x1, // the app came with session-initiate
            UserFlag           = 0x100
        };
        Q_DECLARE_FLAGS(ApplicationFlags, ApplicationFlag)

        virtual void setState(State state) = 0; // likely just remember the state and not generate any signals
        virtual XMPP::Stanza::Error lastError() const  = 0;
        virtual Reason              lastReason() const = 0;

        inline ApplicationManagerPad::Ptr pad() const { return _pad; }
        inline State                      state() const { return _state; }
        inline Origin                     creator() const { return _creator; }
        inline Origin                     senders() const { return _senders; }
        inline QString                    contentName() const { return _contentName; }
        inline QSharedPointer<Transport>  transport() const { return _transport; }
        inline TransportSelector         *transportSelector() const { return _transportSelector.data(); }
        bool                              isRemote() const;
        inline bool                       isLocal() const { return !isRemote(); }
        inline ApplicationFlags           flags() const { return _flags; }
        inline void                       markInitialApplication(bool state)
        {
            if (state)
                _flags |= InitialApplication;
            else
                _flags &= ~InitialApplication;
        }

        virtual SetDescError setRemoteOffer(const QDomElement &description)  = 0;
        virtual SetDescError setRemoteAnswer(const QDomElement &description) = 0;
        virtual QDomElement  makeLocalOffer()                                = 0;
        virtual QDomElement  makeLocalAnswer()                               = 0;

        /**
         * @brief evaluateOutgoingUpdate computes and prepares next update which will be taken with takeOutgoingUpdate
         *   The updated will be taked immediately if considered to be most preferred among other updates types of
         *   other applications.
         * @return update type
         */
        virtual Update evaluateOutgoingUpdate();
        // this may return something only when evaluateOutgoingUpdate() != NoAction
        virtual OutgoingUpdate takeOutgoingUpdate();

        /**
         * @brief setTransport checks if transport is compatible and stores it
         * @param transport
         * @return false if not compatible
         */
        bool setTransport(const QSharedPointer<Transport> &transport, const Reason &reason = Reason());

        /**
         * @brief selectNextTransport selects next transport from compatible transports list.
         *   The list is usually stored in the application
         * @return
         */
        bool selectNextTransport(const QSharedPointer<Transport> alikeTransport = QSharedPointer<Transport>());

        /**
         * @brief Checks where transport-replace is possible atm
         * @return
         */
        virtual bool isTransportReplaceEnabled() const;

        /**
         * @brief wantBetterTransport checks if the transport is a better match for the application
         * Used in content is provided twice with two different transports
         * @return
         */
        virtual bool wantBetterTransport(const QSharedPointer<Transport> &) const;

        /**
         * @brief prepare to send content-add/session-initiate
         *  When ready, the application first set update type to ContentAdd and then emit updated()
         */
        virtual void prepare()                                                                            = 0;
        virtual void start()                                                                              = 0;
        virtual void remove(Reason::Condition cond = Reason::Success, const QString &comment = QString()) = 0;

        virtual void incomingRemove(const Reason &r) = 0;
        void         incomingTransportAccept(const QDomElement &el);

    protected:
        /**
         * @brief wraps transport update so transport can be safely-deleted before callback is triggered
         */
        OutgoingTransportInfoUpdate wrapOutgoingTransportUpdate(bool ensureTransportElement = false);

        /**
         * @brief initTransport in general connects any necessary for the application transport signals
         */
        virtual void prepareTransport() = 0;

        void expectSingleConnection(TransportFeatures features, std::function<void(Connection::Ptr)> &&ready);

    signals:
        void updated(); // signal for session it has to send updates to remote. so it will follow with
                        // takeOutgoingUpdate() eventually
        void stateChanged(State);

    protected:
        State            _state = State::Created;
        ApplicationFlags _flags;

        enum class PendingTransportReplace {
            None,      // not in the replace mode
            Planned,   // didn't send a replacement yet. working on it.
            NeedAck,   // we sent replacement. waiting for iq ack
            InProgress // not yet accepted but acknowledged
        };

        // has to be set when whatever way remote knows about the current transport
        // bool _remoteKnowsOfTheTransport = false;

        // per session object responsible for all applications of this type
        QSharedPointer<ApplicationManagerPad> _pad;

        // content properties as come from the request
        QString _contentName;
        Origin  _creator;
        Origin  _senders;

        // current transport. either local or remote. has info about origin and state
        QSharedPointer<Transport>         _transport;
        QScopedPointer<TransportSelector> _transportSelector;

        // if transport-replace is in progress. will be set to true when accepted by both sides.
        PendingTransportReplace _pendingTransportReplace = PendingTransportReplace::None;

        // while it's valid - we are in unaccepted yet transport-replace
        Reason _transportReplaceReason;

        // when set the content will be removed with this reason
        Reason _terminationReason;

        // evaluated update to be sent
        Update _update;

        QTimer *transportInitTimer = nullptr;
    };

    inline bool operator<(const Application::Update &a, const Application::Update &b)
    {
        return a.action < b.action || (a.action == b.action && a.reason.condition() < b.reason.condition());
    }

    class ApplicationManager : public QObject {
        Q_OBJECT
    public:
        ApplicationManager(QObject *parent = nullptr);

        virtual void         setJingleManager(Manager *jm) = 0;
        virtual Application *startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName,
                                              Origin creator, Origin senders)
            = 0;
        virtual ApplicationManagerPad *pad(Session *session) = 0;

        // this method is supposed to gracefully close all related sessions as a preparation for plugin unload for
        // example
        virtual void closeAll(const QString &ns = QString()) = 0;

        virtual QStringList ns() const;
        virtual QStringList discoFeatures() const = 0;
    };

}}

#endif
