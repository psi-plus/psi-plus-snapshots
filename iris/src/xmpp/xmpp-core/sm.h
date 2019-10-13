/*
 * sm.h - XMPP Stream Management protocol
 * Copyright (C) 2016  Aleksey Andreev
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

#ifndef XMPP_SM_H
#define XMPP_SM_H

#include <QDomElement>
#include <QElapsedTimer>
#include <QObject>
#include <QQueue>

#define NS_STREAM_MANAGEMENT "urn:xmpp:sm:3"
#define SM_TIMER_INTERVAL_SECS 40

//#define IRIS_SM_DEBUG

namespace XMPP {
class SMState {
public:
    SMState();
    void resetCounters();
    bool isResumption() const { return !resumption_id.isEmpty(); }
    bool isEnabled() const { return enabled; }
    bool isLocationValid() { return !resumption_location.host.isEmpty() && resumption_location.port != 0; }
    void setEnabled(bool e) { enabled = e; }

public:
    bool                enabled;
    quint32             received_count;
    quint32             server_last_handled;
    QQueue<QDomElement> send_queue;
    QString             resumption_id;
    struct {
        QString host;
        quint16 port;
    } resumption_location;
};

class StreamManagement : QObject {
public:
    StreamManagement(QObject *parent = nullptr);
    XMPP::SMState &      state() { return state_; }
    const XMPP::SMState &state() const { return state_; }
    bool                 isActive() const { return sm_started || sm_resumed; }
    bool                 isResumed() const { return sm_resumed; }
    void                 reset();
    void                 start(const QString &resumption_id);
    void                 resume(quint32 last_handled);
    void                 setLocation(const QString &host, int port);
    int                  lastAckElapsed() const;
    int                  takeAckedCount();
    void                 countInputRawData(int bytes);
    QDomElement          getUnacknowledgedStanza();
    int                  addUnacknowledgedStanza(const QDomElement &e);
    void                 processAcknowledgement(quint32 last_handled);
    void                 markStanzaHandled();
    QDomElement          generateRequestStanza(QDomDocument &doc);
    QDomElement          makeResponseStanza(QDomDocument &doc);

private:
    SMState state_;
    bool    sm_started;
    bool    sm_resumed;
    int     sm_stanzas_notify;
    int     sm_resend_pos;
    struct {
        QElapsedTimer elapsed_timer;
        bool          waiting_answer = false;
    } sm_timeout_data;
};
} // namespace XMPP

#endif // XMPP_SM_H
