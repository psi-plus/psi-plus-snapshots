/*
 * entitytimetask.cpp - Entity time fetching task
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <QTime>
#include "entitytimetask.h"
#include "xmpp_xmlcommon.h"

using namespace XMPP;

/**
 * \class EntityTimeTask
 * \brief Gets entity time
 *
 * This task can be used to get time zone information of an entity.
 */


// convert [+|-]hh:mm to minutes
static Maybe<int> stringToOffset(const QString &off)
{
	QTime t = QTime::fromString(off.mid(1), "hh:mm");

	if (t.isValid() && (off[0] == '+' || off[0] == '-')) {
		int m = t.hour() * 60 + t.minute();
		if (off[0] == '-')
			m = -m;
		return m;
	}
	else {
		return Maybe<int>();
	}
}

/**
 * \brief Create new task.
 */
EntityTimeTask::EntityTimeTask(Task* parent) : Task(parent)
{
}

/**
 * \brief Queried entity's JID.
 */
const Jid & EntityTimeTask::jid() const
{
	return jid_;
}

/**
 * \brief Prepares the task to get information from JID.
 */
void EntityTimeTask::get(const Jid &jid)
{
	jid_ = jid;
	iq_ = createIQ(doc(), "get", jid_.full(), id());
	QDomElement time = doc()->createElement("time");
	time.setAttribute("xmlns", "urn:xmpp:time");
	iq_.appendChild(time);
}

void EntityTimeTask::onGo()
{
	send(iq_);
}

bool EntityTimeTask::take(const QDomElement &x)
{
	if (!iqVerify(x, jid_, id()))
		return false;

	if (x.attribute("type") == "result") {
		bool found = false;
		QDomElement q = queryTag(x, "time");
		QDomElement tag;
		tag = findSubTag(q, "utc", &found);
		if (found)
			utc_ = tagContent(tag);
		tag = findSubTag(q, "tzo", &found);
		if (found) {
			tzoString_ = tagContent(tag);
			tzo_ = stringToOffset(tzoString_);
		}
		setSuccess();
	}
	else {
		setError(x);
	}

	return true;
}

/**
 * \brief Timezone offset in [+|-]hh:mm format (or empty string if no data).
 */
const QString& EntityTimeTask::timezoneOffsetString() const
{
	return tzoString_;
}

/**
 * \brief Timezone offset in minutes (if available).
 */
Maybe<int> EntityTimeTask::timezoneOffset() const
{
	return tzo_;
}
