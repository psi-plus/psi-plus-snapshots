/*
 * xmpp_discoitem.h
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

#ifndef XMPP_DISCOITEM
#define XMPP_DISCOITEM

#include <QString>
#include <QCryptographicHash>

#include "xmpp/jid/jid.h"
#include "xmpp_features.h"
#include "xmpp_xdata.h"
#include "xmpp_agentitem.h"

namespace XMPP {
	class DiscoItemPrivate;

	class DiscoItem
	{
	public:
		DiscoItem();
		~DiscoItem();

		const Jid &jid() const;
		const QString &node() const;
		const QString &name() const;

		void setJid(const Jid &);
		void setName(const QString &);
		void setNode(const QString &);

		enum Action {
			None = 0,
			Remove,
			Update
		};

		Action action() const;
		void setAction(Action);

		const Features &features() const;
		void setFeatures(const Features &);

		struct Identity
		{
			QString category;
			QString type;
			QString lang;
			QString name;

			inline Identity() {}
			inline Identity(const QString &categoty, const QString &type,
							const QString &lang = QString::null, const QString &name = QString::null) :
				category(categoty), type(type), lang(lang), name(name) {}
			bool operator==(const Identity &other) const;
		};

		typedef QList<Identity> Identities;

		const Identities &identities() const;
		void setIdentities(const Identities &);
		inline void setIdentities(const Identity &id) { setIdentities(Identities() << id); }

		const QList<XData> &extensions() const;
		void setExtensions(const QList<XData> &extlist);
		XData registeredExtension(const QString &ns) const;

		// some useful helper functions
		static Action string2action(QString s);
		static QString action2string(Action a);

		DiscoItem & operator= (const DiscoItem &);
		DiscoItem(const DiscoItem &);

		operator AgentItem() const { return toAgentItem(); }
		AgentItem toAgentItem() const;
		void fromAgentItem(const AgentItem &);

		QString capsHash(QCryptographicHash::Algorithm algo) const;

		static DiscoItem fromDiscoInfoResult(const QDomElement &x);
		QDomElement toDiscoInfoResult(QDomDocument *doc) const;

	private:
		QSharedDataPointer<DiscoItemPrivate> d;
	};

	bool operator<(const DiscoItem::Identity &a, const DiscoItem::Identity &b);
}

#endif
