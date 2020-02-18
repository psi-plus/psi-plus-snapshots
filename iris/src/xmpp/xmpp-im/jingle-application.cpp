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

namespace XMPP { namespace Jingle {
    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    ApplicationManager::ApplicationManager(QObject *parent) : QObject(parent) {}

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
                _update = { canReplaceTransport() ? Action::TransportReplace : Action::ContentRemove,
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
                        _update = { canReplaceTransport() ? Action::TransportReplace : Action::ContentRemove,
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
                _update = { canReplaceTransport() ? Action::TransportReplace : Action::ContentRemove,
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
            return OutgoingUpdate { updates, [this, transportCB](bool success) {
                                       transportCB(success);
                                       setState(State::Pending);
                                   } };

        case Action::ContentAccept:
            contentEl.appendChild(makeLocalAnswer());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);

            setState(State::Unacked);
            return OutgoingUpdate { updates, [this, transportCB](bool success) {
                                       transportCB(success);
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
        auto wrapCB = [this, tr = _transport.toWeakRef(), cb = std::move(transportCB)](bool success) {
            auto transport = tr.lock();
            if (transport && cb)
                cb(success);
            if (_pendingTransportReplace == PendingTransportReplace::NeedAck) {
                _pendingTransportReplace
                    = success ? PendingTransportReplace::InProgress : PendingTransportReplace::None;
                emit updated();
            }
        };
        return OutgoingTransportInfoUpdate { transportEl, wrapCB };
    }
}}
