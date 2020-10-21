/*
 * jignle-application.cpp - Base Jingle application classes
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

#include "jingle-application.h"
#include "jingle-session.h"
#include "xmpp_client.h"
#include "xmpp_task.h"

namespace XMPP { namespace Jingle {
    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    ApplicationManager::ApplicationManager(QObject *parent) : QObject(parent) { }

    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    Application::Update Application::evaluateOutgoingUpdate()
    {
        _update = { Action::NoAction, Reason() };
        if (_state == State::Finished || _state == State::Created
            || _pendingTransportReplace == PendingTransportReplace::NeedAck)
            return _update;

        if (_terminationReason.isValid()) {
            _update = { Action::ContentRemove, _terminationReason };
            return _update;
        }

        // missing transport means it's an incoming application with invalid transport,
        // but basically it shouldn't happen
        if ((_creator != _pad->session()->role() && _state == State::Pending) || !_transport) {
            return _update;
        }

        bool inTrReplace = _pendingTransportReplace == PendingTransportReplace::InProgress;

        if (_transport->state() == State::Finished) {
            if (inTrReplace && _transport->creator() != _pad->session()->role())
                _update = { Action::TransportReject, _transport->lastReason() };
            else
                _update = { _transportSelector->hasMoreTransports() ? Action::TransportReplace : Action::ContentRemove,
                            _transport->lastReason() };
            return _update;
        }

        switch (_state) {
        case State::ApprovedToSend:
            if (_transport->hasUpdates() && _transport->state() == State::ApprovedToSend) {
                if (inTrReplace) {
                    // either we are waiting for incoming transport-accept or local confirmation of remote
                    // transport-replace
                    bool localTranport = _pad->session()->role() == _transport->creator();
                    _update            = { localTranport ? Action::TransportInfo : Action::TransportAccept, Reason() };
                } else
                    _update = { _pad->session()->role() == _creator ? Action::ContentAdd : Action::ContentAccept,
                                Reason() };
            }
            break;
        case State::Pending:
            if (_creator != _pad->session()->role() && !inTrReplace && _transport->hasUpdates()
                && _transport->state() == State::ApprovedToSend) {
                // if remote transport has initial updates and it's not transport-replace then it's time to accept the
                // content
                _update = { Action::ContentAccept, Reason() };
                break;
            }
            // fallthrough
        case State::Connecting:
            if (inTrReplace) {
                // for transport replace we handle just replace until it's finished
                if (_transport->creator() == _pad->session()->role()) {
                    if (_transport->state() == State::Finished)
                        // replace over unconfirmed replace (2nd transport failed shortly)
                        _update = { _transportSelector->hasMoreTransports() ? Action::TransportReplace
                                                                            : Action::ContentRemove,
                                    _transport->lastReason() };
                    break;
                }

                if (_transport->hasUpdates() && _transport->state() == State::ApprovedToSend) {
                    _update = { Action::TransportAccept, Reason() };
                    break;
                }
                if (_transport->state() == State::Finished) {
                    _update = { Action::TransportReject, _transport->lastReason() };
                }
                break;
            }

            if (_transport->hasUpdates()) {
                if (_transport->state() == State::Connecting)
                    _update = { Action::TransportInfo, Reason() };
            } else if (_transport->state() == State::Finished) {
                _update = { _transportSelector->hasMoreTransports() ? Action::TransportReplace : Action::ContentRemove,
                            _transport->lastReason() };
            }
            break;
        case State::Active:
            if (_transport->hasUpdates())
                _update = { Action::TransportInfo, Reason() };

            break;
        default:
            break;
        }
        return _update;
    }

    OutgoingUpdate Application::takeOutgoingUpdate()
    {

        QDomElement      transportEl;
        OutgoingUpdateCB transportCB;
        auto             client = _pad->session()->manager()->client();
        auto             doc    = client->doc();

        ContentBase cb(_creator, _contentName);
        // we need to send senders for initial offer/answer
        if (_state == State::ApprovedToSend)
            cb.senders = _senders;
        QList<QDomElement> updates;
        auto               contentEl = cb.toXml(doc, "content");
        updates << contentEl;

        switch (_update.action) {
        case Action::ContentReject:
        case Action::ContentRemove:
            if (_update.reason.isValid())
                updates << _update.reason.toXml(doc);
            return OutgoingUpdate { updates, [this](bool) { setState(State::Finished); } };
        case Action::ContentAdd:
            contentEl.appendChild(makeLocalOffer());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);

            setState(State::Unacked);
            return OutgoingUpdate { updates, [this, transportCB](Task *task) {
                                       transportCB(task);
                                       if (task->success())
                                           setState(State::Pending);
                                   } };

        case Action::ContentAccept:
            contentEl.appendChild(makeLocalAnswer());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);

            setState(State::Unacked);
            return OutgoingUpdate { updates, [this, transportCB](Task *task) {
                                       transportCB(task);
                                       if (task->success())
                                           setState(State::Connecting);
                                   } };
        case Action::TransportInfo:
            Q_ASSERT(_transport->hasUpdates());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            return OutgoingUpdate { updates, transportCB };
        case Action::TransportReplace:
        case Action::TransportAccept: {
            Q_ASSERT(_transport->hasUpdates());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            return OutgoingUpdate { updates, transportCB };
        }
        default:
            break;
        }

        return OutgoingUpdate(); // TODO
    }

    OutgoingTransportInfoUpdate Application::wrapOutgoingTransportUpdate()
    {
        QDomElement      transportEl;
        OutgoingUpdateCB transportCB;
        std::tie(transportEl, transportCB) = _transport->takeOutgoingUpdate();
        auto wrapCB = [this, tr = _transport.toWeakRef(), cb = std::move(transportCB)](Task *task) {
            auto transport = tr.lock();
            if (transport && cb)
                cb(task);
            if (_pendingTransportReplace == PendingTransportReplace::NeedAck) {
                _pendingTransportReplace
                    = task->success() ? PendingTransportReplace::InProgress : PendingTransportReplace::None;
                emit updated();
            }
        };
        return OutgoingTransportInfoUpdate { transportEl, wrapCB };
    }

    bool Application::selectNextTransport(const QSharedPointer<Transport> alikeTransport)
    {
        if (!_transportSelector->hasMoreTransports()) {
            emit updated(); // will be evaluated to content-remove
            return false;
        }

        if (alikeTransport) {
            auto tr = _transportSelector->getAlikeTransport(alikeTransport);
            if (tr && setTransport(tr))
                return true;
        }

        QSharedPointer<Transport> t;
        while ((t = _transportSelector->getNextTransport()))
            if (setTransport(t))
                return true;

        emit updated(); // will be evaluated to content-remove
        return false;
    }

    bool Application::wantBetterTransport(const QSharedPointer<Transport> &t) const
    {
        if (!_transportSelector->hasTransport(t))
            return false;

        return !_transport || _transportSelector->compare(t, _transport) > 0;
    }

    bool Application::isTransportReplaceEnabled() const { return true; }

    bool Application::setTransport(const QSharedPointer<Transport> &transport, const Reason &reason)
    {
        if (!isTransportReplaceEnabled() || !_transportSelector->replace(_transport, transport))
            return false;

        qDebug("setting transport %s", qPrintable(transport->pad()->ns()));
        // in case we automatically select a new transport on our own we definitely will come up to this point
        if (_transport) {
            if (_transport->state() < State::Unacked && _transport->creator() == _pad->session()->role()
                && _transport->pad()->ns() != transport->pad()->ns()) {
                // the transport will be reused later since the remote doesn't know about it yet
                _transportSelector->backupTransport(_transport);
            }

            if (transport->creator() == _pad->session()->role()) {
                auto ts = _transport->state() == State::Finished ? _transport->prevState() : _transport->state();
                if (_transport->creator() != _pad->session()->role() || ts > State::Unacked) {
                    // if remote knows of the current transport
                    _pendingTransportReplace = PendingTransportReplace::InProgress;
                } else if (_transport->creator() == _pad->session()->role() && ts == State::Unacked) {
                    // if remote may know but we don't know yet about it
                    _pendingTransportReplace = PendingTransportReplace::NeedAck;
                }
            } else {
                _pendingTransportReplace = PendingTransportReplace::InProgress;
            }

            if (_pendingTransportReplace != PendingTransportReplace::None) {
                if (_transport->state() == State::Finished) { // initiate replace?
                    _transportReplaceReason = reason.isValid() ? reason : _transport->lastReason();
                } else {
                    _transportReplaceReason = reason;
                }
            }
            _transport->disconnect(this);
            _transport.reset();
        }

        _transport = transport;
        connect(transport.data(), &Transport::updated, this, &Application::updated);
        connect(transport.data(), &Transport::failed, this, [this]() { selectNextTransport(); });

        initTransport();

        if (_state >= State::Unacked) {
            _transport->prepare();
        }
        return true;
    }

    bool ApplicationManagerPad::incomingSessionInfo(const QDomElement &) { return false; /* unsupported by default */ }

}}
