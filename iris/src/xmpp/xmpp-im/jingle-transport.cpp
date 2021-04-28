/*
 * jignle-transport.cpp - Base Jingle transport classes
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

#include "jingle-transport.h"
#include "jingle-session.h"

#include <QPointer>

namespace XMPP { namespace Jingle {
    //----------------------------------------------------------------------------
    // TransportManager
    //----------------------------------------------------------------------------
    TransportManager::TransportManager(QObject *parent) : QObject(parent) { }

    bool TransportManager::canMakeConnection(TransportFeatures desiredFeatures, const QString &)
    {
        return (features() & desiredFeatures) == desiredFeatures;
    }
    QStringList TransportManager::ns() const { return discoFeatures(); }
    void        TransportManager::closeAll(const QString &) { emit abortAllRequested(); }

    //----------------------------------------------------------------------------
    // Transport
    //----------------------------------------------------------------------------
    Transport::Transport(TransportManagerPad::Ptr pad, Origin creator) : _creator(creator), _pad(pad) { }

    bool Transport::isRemote() const { return _pad->session()->role() != _creator; }

    void Transport::stop() { _state = State::Finished; }

    int Transport::maxSupportedComponents() const { return 1; }

    void Transport::setComponentsCount(int) { }

    int Transport::maxSupportedChannelsPerComponent(TransportFeatures) const { return 0; }

    void Transport::setState(State newState)
    {
        _prevState = _state;
        _state     = newState;
        emit stateChanged();
    }

    bool Transport::wasAccepted() const
    {
        if (isRemote())
            return _state >= State::ApprovedToSend && _state != State::Pending;
        return _state >= State::ApprovedToSend;
    }

    void Transport::onFinish(Reason::Condition condition, const QString &message)
    {
        _lastReason = Reason(condition, message);
        _prevState  = _state;
        _state      = State::Finished;
        QPointer<Transport> p(this);
        if (condition != Reason::Condition::Success && condition != Reason::Condition::NoReason)
            emit failed();
        if (p)
            emit stateChanged();
    }

    void Transport::addAcceptor(TransportFeatures features, ConnectionAcceptorCallback &&acceptor, int componentIndex)
    {
        _connectionAcceptors.append({ features, std::move(acceptor), componentIndex });
    }

    const QList<ConnectionAcceptor> &Transport::acceptors() const { return _connectionAcceptors; }

    bool Transport::notifyIncomingConnection(Connection::Ptr connection) const
    {
        for (auto const &acceptor : _connectionAcceptors) {
            if ((connection->features() & acceptor.features) == acceptor.features
                && (acceptor.componentIndex < 0 || acceptor.componentIndex == connection->component())
                && acceptor.callback(connection))
                return true;
        }
        return false;
    }

    //----------------------------------------------------------------------------
    // TransportSelector
    //----------------------------------------------------------------------------
    TransportSelector::~TransportSelector() { }

    bool TransportSelector::canReplace(QSharedPointer<Transport> old, QSharedPointer<Transport> newer)
    {
        return newer && (hasTransport(newer) || compare(old, newer) == 0);
    }
}}
