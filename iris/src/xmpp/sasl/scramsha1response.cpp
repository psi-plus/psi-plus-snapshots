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

#include "xmpp/sasl/scramsha1response.h"

#include <QByteArray>
#include <QString>
#include <QTextStream>
#include <QtCrypto>
#include <QtDebug>
#include <QRegExp>

#include "xmpp/base/randomnumbergenerator.h"
#include "xmpp/jid/jid.h"

namespace XMPP {
	QCA::SecureArray HMAC_SHA_1(const QCA::SecureArray &key, const QCA::SecureArray &str) {
		QCA::SecureArray result = QCA::MessageAuthenticationCode("hmac(sha1)", key).process(str);
		return result;
	}

	SCRAMSHA1Response::SCRAMSHA1Response(const QByteArray& server_first_message, const QByteArray& password_in, const QByteArray& client_first_message, const QString &salted_password_base64, const RandomNumberGenerator& rand)
{
	Q_UNUSED(rand);
	QString pass_in(password_in);
	QString pass_out;

	QRegExp pattern("r=(.*),s=(.+),i=(\\d+)");
	int pos = pattern.indexIn(QString(server_first_message));
	isValid_ = true;
	if (pos > -1) {
		QString clientservernonce = pattern.cap(1);
		QString salt = pattern.cap(2);
		QString icount = pattern.cap(3);

		size_t dkLen;

		QCA::Hash shaHash("sha1");
		shaHash.update("", 0);
		dkLen = shaHash.final().size();

		QCA::PBKDF2 hi("sha1");

		QByteArray password;

		// SaltedPassword  := Hi(Normalize(password), salt, i)
		if (salted_password_base64.size() > 0)
			salted_password_ = QCA::SymmetricKey(QCA::SecureArray(QCA::Base64().stringToArray(salted_password_base64.toUtf8())));
		if (salted_password_.size() == 0) {
			if (!StringPrepCache::saslprep(pass_in, 1023, pass_out)) {
				isValid_ = false;
				return;
			}

			password = pass_out.toUtf8();
			salted_password_ = hi.makeKey(QCA::SecureArray(password), QCA::InitializationVector(QCA::Base64().stringToArray(salt)), dkLen, icount.toULong());
		}

		// ClientKey       := HMAC(SaltedPassword, "Client Key")
		QCA::SecureArray client_key(HMAC_SHA_1(salted_password_.toByteArray(), QByteArray("Client Key")));

		// StoredKey       := H(ClientKey)
		QCA::SecureArray stored_key = QCA::Hash("sha1").process(client_key);

		// assemble client-final-message-without-proof

		QString gs2_header;
		{
			QRegExp pattern("(.+)n=.+");
			pattern.indexIn(QString(client_first_message));
			gs2_header = pattern.cap(1);
		}

		QString client_final_message;
		QTextStream final_message_stream(&client_final_message);
		final_message_stream << "c=" << QCA::Base64().arrayToString((gs2_header.toUtf8()));
		final_message_stream << ",r=" << clientservernonce;

		// AuthMessage     := client-first-message-bare + "," + server-first-message + "," + client-final-message-without-proof
		QRegExp extract_cfmb_pattern("(n=.+)");
		if (extract_cfmb_pattern.indexIn(QString(client_first_message)) < 0) {
			isValid_ = false;
			return;
		}

		QString client_first_message_bare = extract_cfmb_pattern.cap(1);

		QCA::SecureArray auth_message = QCA::SecureArray(client_first_message_bare.toUtf8());
		auth_message += QCA::SecureArray(",") + QCA::SecureArray(server_first_message);
		auth_message += QCA::SecureArray(",") + QCA::SecureArray(client_final_message.toUtf8());

		// ClientSignature := HMAC(StoredKey, AuthMessage)
		QCA::SecureArray client_signature = HMAC_SHA_1(stored_key, auth_message);

		// ClientProof     := ClientKey XOR ClientSignature
		QCA::SecureArray client_proof(client_key.size());
		for(int i = 0; i < client_proof.size(); ++i) {
			client_proof[i] = client_key[i] ^ client_signature[i];
		}

		// ServerKey       := HMAC(SaltedPassword, "Server Key")
		QCA::SecureArray server_key = HMAC_SHA_1(salted_password_, QByteArray("Server Key"));

		// ServerSignature := HMAC(ServerKey, AuthMessage)
		server_signature_ = HMAC_SHA_1(server_key, auth_message);

		final_message_stream << ",p=" << QCA::Base64().arrayToString(client_proof);
		value_ = client_final_message.toUtf8();
	} else {
		qWarning("SASL/SCRAM-SHA-1: Failed to match pattern for server-final-message.");
		isValid_ = false;
	}
}

const QString SCRAMSHA1Response::getSaltedPassword() {
	return QCA::Base64().arrayToString(salted_password_);
}

}
