/*
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

#ifndef XMPP_URL
#define XMPP_URL

class QString;

namespace XMPP
{
	class Url
	{
	public:
		Url(const QString &url="", const QString &desc="");
		Url(const Url &);
		Url & operator=(const Url &);
		~Url();

		QString url() const;
		QString desc() const;

		void setUrl(const QString &);
		void setDesc(const QString &);

	private:
		class Private;
		Private *d;
	};

	typedef QList<Url> UrlList;
}

#endif
