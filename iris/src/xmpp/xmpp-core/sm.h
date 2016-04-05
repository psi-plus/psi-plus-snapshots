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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef XMPP_SM_H
#define XMPP_SM_H

#include <QObject>
#include <QQueue>
#include <QDomElement>
#include <QDateTime>

#define NS_STREAM_MANAGEMENT   "urn:xmpp:sm:3"
#define SM_TIMER_INTERVAL_SECS 20

//#define IRIS_SM_DEBUG

namespace XMPP
{
	class SMState
	{
	public:
		SMState();
		void reset();
		bool isResumption() const { return !resumption_id.isEmpty(); }
		bool isEnabled() const { return enabled; }
		void setEnabled(bool e) { enabled = e; }

	public:
		bool enabled;
		quint32 received_count;
		quint32 server_last_handled;
		QQueue<QDomElement> send_queue;
		QString resumption_id;
		struct {
			QString host;
			int port;
		} resumption_location;
	};

	class StreamManagement : QObject
	{
	public:
		StreamManagement(QObject *parent = 0);
		XMPP::SMState &state() { return state_; }
		const XMPP::SMState &state() const { return state_; }
		bool isActive() const { return sm_started || sm_resumed; }
		bool isResumed() const { return sm_resumed; }
		void reset();
		void started(const QString &resumption_id);
		void resumed(quint32 last_handled);
		void setLocation(const QString &host, int port);
		int  lastAckElapsed() const;
		int  takeAckedCount();
		QDomElement getUnacknowledgedStanza();
		int  addUnacknowledgedStanza(const QDomElement &e);
		void processAcknowledgement(quint32 last_handled);
		bool processNormalStanza(const QDomElement &e);
		void markStanzaHandled();
		QDomElement generateRequestStanza(QDomDocument &doc);
		QDomElement makeResponseStanza(QDomDocument &doc);

	private:
		SMState state_;
		bool sm_started;
		bool sm_resumed;
		int  sm_stanzas_notify;
		int  sm_resend_pos;
		struct {
			QDateTime point_time;
			bool pause_mode;
		} sm_timeout_data;
	};
}

#endif //XMPP_SM_H
