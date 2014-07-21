/*
 * xmpp.h - XMPP "core" library API
 * Copyright (C) 2003  Justin Karneges
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

#ifndef XMPP_STREAM_H
#define XMPP_STREAM_H

#include <QDomElement>
#include <QObject>

#include "xmpp_stanza.h"
#include "xmpp/jid/jid.h"

class QDomDocument;

namespace XMPP
{
	class Stream : public QObject
	{
		Q_OBJECT
	public:
		enum Error { ErrParse, ErrProtocol, ErrStream, ErrCustom = 10 };
		enum StreamCond {
			GenericStreamError,
			Conflict,
			ConnectionTimeout,
			InternalServerError,
			InvalidFrom,
			InvalidXml,
			PolicyViolation,
			ResourceConstraint,
			SystemShutdown
		};

		Stream(QObject *parent=0);
		virtual ~Stream();

		virtual QDomDocument & doc() const=0;
		virtual QString baseNS() const=0;
		virtual bool old() const=0;

		virtual void close()=0;
		virtual bool stanzaAvailable() const=0;
		virtual Stanza read()=0;
		virtual void write(const Stanza &s, bool notify = false)=0;

		virtual int errorCondition() const=0;
		virtual QString errorText() const=0;
		virtual QDomElement errorAppSpec() const=0;

		Stanza createStanza(Stanza::Kind k, const Jid &to="", const QString &type="", const QString &id="");
		Stanza createStanza(const QDomElement &e);

		static QString xmlToString(const QDomElement &e, bool clip=false);

		static void cleanup();

	signals:
		void connectionClosed();
		void delayedCloseFinished();
		void readyRead();
		void stanzaWritten();
		void error(int);
	};
}

#endif
