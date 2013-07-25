/*
 * Copyright (C) 2010  Tobias Markmann
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

#ifndef SCRAMSHA1RESPONSE_H
#define SCRAMSHA1RESPONSE_H

#include <QByteArray>
#include <QString>
#include <QtCrypto>

namespace XMPP {
	class RandomNumberGenerator;

	class SCRAMSHA1Response
	{
		public:
			SCRAMSHA1Response(
					const QByteArray& server_first_message,
					const QByteArray& password,
					const QByteArray& client_first_message,
					const QString &salted_password_base64,
					const RandomNumberGenerator& rand);

			const QByteArray& getValue() const {
				return value_;
			}

			const QCA::SecureArray getServerSignature() const {
				return server_signature_;
			}

			const QString getSaltedPassword();

			bool isValid() const {
				return isValid_;
			}

		private:
			bool isValid_;
			QByteArray value_;
			QCA::SecureArray server_signature_;
			QCA::SymmetricKey salted_password_;
	};
}

#endif
