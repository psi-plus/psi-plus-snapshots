/*
 * Copyright (C) 2006  Remko Troncon
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

#ifndef XMPP_HTMLELEMENT_H
#define XMPP_HTMLELEMENT_H

#include <QDomElement>

class QString;

namespace XMPP
{
	class HTMLElement
	{
	public:
		HTMLElement();
		HTMLElement(const QDomElement &body);

		void setBody(const QDomElement &body);
		const QDomElement& body() const;
		QString toString(const QString &rootTagName = "body") const;
		QString text() const;
		void filterOutUnwanted(bool strict = false);

	private:
		void filterOutUnwantedRecursive(QDomElement &el, bool strict);

		QDomDocument doc_;
		QDomElement body_;
	};
}

#endif
