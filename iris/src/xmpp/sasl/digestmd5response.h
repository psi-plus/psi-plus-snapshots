/*
 * Copyright (C) 2008  Remko Troncon
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

#ifndef DIGESTMD5RESPONSE_H
#define DIGESTMD5RESPONSE_H

#include <QByteArray>
#include <QString>

namespace XMPP {
	class RandomNumberGenerator;

	class DIGESTMD5Response
	{
		public:
			DIGESTMD5Response(
					const QByteArray& challenge,
					const QString& service,
					const QString& host,
					const QString& realm,
					const QString& user,
					const QString& authz,
					const QByteArray& password,
					const RandomNumberGenerator& rand);

			const QByteArray& getValue() const {
				return value_;
			}

			bool isValid() const {
				return isValid_;
			}

		private:
			bool isValid_;
			QByteArray value_;
	};
}

#endif
