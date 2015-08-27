/*
 * entitytimetask.h - Entity time fetching task
 * Copyright (C) 2007  Maciej Niedzielski
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef ENTITYTIMETASK_H
#define ENTITYTIMETASK_H

#include <QDomElement>
#include "xmpp_task.h"
#include "xmpp_jid.h"
#include "maybe.h"

class EntityTimeTask : public XMPP::Task
{
public:
	EntityTimeTask(Task*);

	void onGo();
	bool take(const QDomElement &);
	void get(const XMPP::Jid &jid);
	const XMPP::Jid & jid() const;

	const QString& timezoneOffsetString() const;
	Maybe<int> timezoneOffset() const;

private:
	QDomElement iq_;
	XMPP::Jid jid_;
	QString utc_, tzoString_;
	Maybe<int> tzo_;
};

#endif
