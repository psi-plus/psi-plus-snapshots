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

#include <QPointer>
#include <QTimer>

namespace XMPP { namespace Jingle {

    class ConnectionWaiter : public QObject {
        Q_OBJECT

        std::function<void(Connection::Ptr)> ready;
        std::function<void()>                failed;
        Connection::Ptr                      connection;
        QWeakPointer<Transport>              transport;

        void waitConnected()
        {
            connect(connection.data(), &Connection::error, this,
                    [this](int code) { onFailed(QString("error=%1").arg(code)); });
            connect(connection.data(), &Connection::connected, this, &ConnectionWaiter::onReady);
        }

        void onFailed(const QString &errorMessage = QString())
        {
            if (!errorMessage.isEmpty())
                qDebug("ConnectionWaiter: error: %s", qPrintable(errorMessage));
            if (connection)
                connection->disconnect(this); // qt signals
            if (auto t = transport.lock()) {
                t->disconnect(this);
            }
            failed();
            deleteLater();
        }

        void onReady()
        {
            connection->disconnect(this); // qt signals
            transport.lock()->disconnect(this);
            ready(connection);
            deleteLater();
        }

    public:
        ConnectionWaiter(TransportFeatures features, std::function<void(Connection::Ptr)> &&ready,
                         std::function<void()> &&failed, Application *app) :
            QObject(app), ready(std::move(ready)), failed(std::move(failed))
        {
            auto tr   = app->transport();
            transport = tr;
            Q_ASSERT(!tr.isNull());

            connect(tr.data(), &Transport::stateChanged, this, [this]() {
                if (transport.lock()->state() == State::Finished) {
                    onFailed(QLatin1String("Transport is dead but no connection"));
                }
            });
            if (tr->isLocal()) {
                connection = tr->addChannel(features, app->contentName());
                if (!connection) {
                    onFailed(QString("No channel on %1 transport").arg(tr->pad()->ns()));
                    return;
                }
                waitConnected();
            } else {
                app->transport()->addAcceptor(
                    features, [this, self = QPointer<ConnectionWaiter>(this)](Connection::Ptr newConnection) {
                        if (!self || connection)
                            return false;
                        connection = newConnection;
                        if (connection->isOpen())
                            onReady();
                        else
                            waitConnected();
                        return true;
                    });
            }
        }

        ~ConnectionWaiter() { qDebug("~ConnectionWaiter"); }
    };

    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    ApplicationManager::ApplicationManager(QObject *parent) : QObject(parent) { }
    QStringList ApplicationManager::ns() const { return discoFeatures(); }

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
        if ((isRemote() && _state == State::Pending) || !_transport) {
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
            if (_transport->state() >= State::Accepted) {
                _update
                    = { _pad->session()->role() == _creator ? Action::ContentAdd : Action::ContentAccept, Reason() };
            } else if (_transport->hasUpdates() && _transport->state() == State::ApprovedToSend) {
                if (_pendingTransportReplace == PendingTransportReplace::Planned) {
                    _update = { Action::TransportReplace, _transportReplaceReason };
                } else if (inTrReplace) { // both sides already know it's replace. but not accepted yet.
                    _update = { _transport->isLocal() ? Action::TransportInfo : Action::TransportAccept, Reason() };
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
                if (_transport->state() == State::Finished) { // 2nd transport failed shortly
                    _update = { _transportSelector->hasMoreTransports()
                                    ? (_transport->isLocal() ? Action::TransportReplace : Action::TransportReject)
                                    : Action::ContentRemove,
                                _transport->lastReason() };
                } else if (_transport->hasUpdates() && _transport->state() == State::ApprovedToSend) {
                    _update = { _transport->isLocal() ? Action::TransportInfo : Action::TransportAccept, Reason() };
                }
                break;
            }

            if (_transport->hasUpdates()) {
                if (_transport->state() >= State::ApprovedToSend && _transport->state() < State::Finished)
                    _update = { _pendingTransportReplace == PendingTransportReplace::Planned ? Action::TransportReplace
                                                                                             : Action::TransportInfo,
                                Reason() };
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
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate(true);
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
            Q_ASSERT(_transport->hasUpdates());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            if (_pendingTransportReplace == PendingTransportReplace::Planned) {
                _pendingTransportReplace = PendingTransportReplace::NeedAck;
            }
            if (_update.reason.isValid())
                updates << _update.reason.toXml(doc);
            return OutgoingUpdate { updates, [this, transportCB](Task *task) {
                                       transportCB(task);
                                       if (task->success())
                                           _pendingTransportReplace = PendingTransportReplace::InProgress;
                                       // else transport will report failure from its callback => select next tran.
                                   } };
        case Action::TransportAccept:
            Q_ASSERT(_transport->hasUpdates());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            return OutgoingUpdate { updates, [this, transportCB](Task *task) {
                                       transportCB(task);
                                       if (task->success()) {
                                           _pendingTransportReplace = PendingTransportReplace::None;
                                           if (_state == State::Connecting || _state == State::Active)
                                               _transport->start();
                                       }
                                       // else transport will report failure from its callback => select next tran.
                                   } };
        default:
            break;
        }

        return OutgoingUpdate(); // TODO
    }

    OutgoingTransportInfoUpdate Application::wrapOutgoingTransportUpdate(bool ensureTransportElement)
    {
        QDomElement      transportEl;
        OutgoingUpdateCB transportCB;
        std::tie(transportEl, transportCB) = _transport->takeOutgoingUpdate(ensureTransportElement);
        auto wrapCB                        = [tr = _transport.toWeakRef(), cb = std::move(transportCB)](Task *task) {
            auto transport = tr.lock();
            if (!transport) {
                return;
            }
            if (cb)
                cb(task);
        };
        return OutgoingTransportInfoUpdate { transportEl, wrapCB };
    }

    void Application::expectSingleConnection(TransportFeatures features, std::function<void(Connection::Ptr)> &&ready)
    {
        new ConnectionWaiter(
            features, std::move(ready),
            [this]() {
                qDebug("Application::expectSingleConnection: stopping failed %s transport",
                       qPrintable(_transport->pad()->ns()));
                _transport->stop();
                selectNextTransport();
            },
            this);
    }

    bool Application::isRemote() const { return _pad->session()->role() != _creator; }

    bool Application::selectNextTransport(const QSharedPointer<Transport> alikeTransport)
    {
        if (!_transportSelector->hasMoreTransports()) {
            if (_transport) {
                qDebug("Application::selectNextTransport: stopping %s transport", qPrintable(_transport->pad()->ns()));
                _transport->disconnect(this);
                _transport->stop();
            }
            _state             = (isRemote() || _state > State::ApprovedToSend) ? State::Finishing : State::Finished;
            _terminationReason = Reason(Reason::FailedTransport);
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

    void Application::incomingTransportAccept(const QDomElement &el)
    {
        if (_pendingTransportReplace != PendingTransportReplace::InProgress) {
            return; // ignore out of order
        }
        _pendingTransportReplace = PendingTransportReplace::None;
        if (_transport->update(el) && _state >= State::Connecting)
            _transport->start();
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

            if (transport->isLocal()) {
                auto ts = _transport->state() == State::Finished ? _transport->prevState() : _transport->state();
                if (_transport->isRemote() || ts > State::Unacked) {
                    // if remote knows of the current transport
                    _pendingTransportReplace = PendingTransportReplace::Planned;
                } else if (_transport->isLocal() && ts == State::Unacked) {
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
            qDebug("Application::setTransport: resetting %s transport in favor of %s",
                   qPrintable(_transport->pad()->ns()), qPrintable(transport->pad()->ns()));
            _transport->disconnect(this);
            _transport.reset();
        }

        _transport = transport;

        connect(_transport.data(), &Transport::updated, this, &Application::updated);
        connect(_transport.data(), &Transport::failed, this, [this]() { selectNextTransport(); });

        if (_transport && _transport->state() < State::Finishing && _state >= State::ApprovedToSend) {
            QTimer::singleShot(0, this, [this, wp = _transport.toWeakRef()]() {
                auto p = wp.lock();
                if (p && p == _transport) {
                    prepareTransport();
                }
            });
        }

        return true;
    }

    bool ApplicationManagerPad::incomingSessionInfo(const QDomElement &) { return false; /* unsupported by default */ }

}}

#include "jingle-application.moc"
