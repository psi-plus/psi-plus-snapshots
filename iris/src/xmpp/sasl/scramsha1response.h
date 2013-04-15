/*
 * Copyright (C) 2010  Tobias Markmann
 * See COPYING for license details.
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
