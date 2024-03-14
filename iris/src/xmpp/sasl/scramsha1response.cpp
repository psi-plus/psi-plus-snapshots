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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "xmpp/sasl/scramsha1response.h"

#include "xmpp/jid/jid.h"

#include <QByteArray>
#include <QRegularExpression>
#include <QString>
#include <QTextStream>
#include <QtCrypto>
#include <QtDebug>

namespace XMPP {
QCA::SecureArray HMAC_SHA_1(const QCA::SecureArray &key, const QCA::SecureArray &str)
{
    QCA::SecureArray result = QCA::MessageAuthenticationCode("hmac(sha1)", key).process(str);
    return result;
}

SCRAMSHA1Response::SCRAMSHA1Response(const QByteArray &server_first_message, const QByteArray &password_in,
                                     const QByteArray &client_first_message, const QString &salted_password_base64)
{
    Q_UNUSED(rand);
    QString pass_in = QString::fromUtf8(password_in);
    QString pass_out;

    QRegularExpression pattern("r=(.*),s=(.+),i=(\\d+)");
    auto               match = pattern.match(QString(server_first_message));
    isValid_                 = match.hasMatch();
    if (!isValid_) {
        qWarning("SASL/SCRAM-SHA-1: Failed to match pattern for server-final-message.");
        return;
    }
    QCA::PBKDF2 hi("sha1");
    if (!hi.context()) {
        qWarning("SASL/SCRAM-SHA-1: sha1 is not supported by qca.");
        isValid_ = false;
        return;
    }

    QString clientservernonce = match.captured(1);
    QString salt              = match.captured(2);
    QString icount            = match.captured(3);

    unsigned int dkLen;

    QCA::Hash shaHash("sha1");
    shaHash.update("", 0);
    dkLen = shaHash.final().size();

    QByteArray password;

    // SaltedPassword  := Hi(Normalize(password), salt, i)
    if (salted_password_base64.size() > 0)
        salted_password_
            = QCA::SymmetricKey(QCA::SecureArray(QCA::Base64().stringToArray(salted_password_base64.toUtf8())));
    if (salted_password_.size() == 0) {
        if (!StringPrepCache::saslprep(pass_in, 1023, pass_out)) {
            isValid_ = false;
            return;
        }

        password = pass_out.toUtf8();
        salted_password_
            = hi.makeKey(QCA::SecureArray(password), QCA::InitializationVector(QCA::Base64().stringToArray(salt)),
                         dkLen, icount.toULong());
    }

    // ClientKey       := HMAC(SaltedPassword, "Client Key")
    QCA::SecureArray client_key(HMAC_SHA_1(salted_password_.toByteArray(), QByteArray("Client Key")));

    // StoredKey       := H(ClientKey)
    QCA::SecureArray stored_key = QCA::Hash("sha1").process(client_key);

    // assemble client-final-message-without-proof

    QString gs2_header;
    {
        QRegularExpression pattern("(.+)n=.+");
        gs2_header = pattern.match(QString(client_first_message)).captured(1);
    }

    QString     client_final_message;
    QTextStream final_message_stream(&client_final_message);
    final_message_stream << "c=" << QCA::Base64().arrayToString((gs2_header.toUtf8()));
    final_message_stream << ",r=" << clientservernonce;

    // AuthMessage     := client-first-message-bare + "," + server-first-message + "," +
    // client-final-message-without-proof
    QRegularExpression extract_cfmb_pattern("(n=.+)");
    match    = extract_cfmb_pattern.match(QString(client_first_message));
    isValid_ = match.hasMatch();
    if (!isValid_) {
        return;
    }

    QString client_first_message_bare = match.captured(1);

    QCA::SecureArray auth_message = QCA::SecureArray(client_first_message_bare.toUtf8());
    auth_message += QCA::SecureArray(",") + QCA::SecureArray(server_first_message);
    auth_message += QCA::SecureArray(",") + QCA::SecureArray(client_final_message.toUtf8());

    // ClientSignature := HMAC(StoredKey, AuthMessage)
    QCA::SecureArray client_signature = HMAC_SHA_1(stored_key, auth_message);

    // ClientProof     := ClientKey XOR ClientSignature
    QCA::SecureArray client_proof(client_key.size());
    for (int i = 0; i < client_proof.size(); ++i) {
        client_proof[i] = client_key[i] ^ client_signature[i];
    }

    // ServerKey       := HMAC(SaltedPassword, "Server Key")
    QCA::SecureArray server_key = HMAC_SHA_1(salted_password_, QByteArray("Server Key"));

    // ServerSignature := HMAC(ServerKey, AuthMessage)
    server_signature_ = HMAC_SHA_1(server_key, auth_message);

    final_message_stream << ",p=" << QCA::Base64().arrayToString(client_proof);
    value_ = client_final_message.toUtf8();
}

const QString SCRAMSHA1Response::getSaltedPassword() { return QCA::Base64().arrayToString(salted_password_); }
} // namespace XMPP
