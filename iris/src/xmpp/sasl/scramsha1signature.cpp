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

#include "xmpp/sasl/scramsha1signature.h"

#include <QByteArray>
#include <QString>
#include <QTextStream>
#include <QtCrypto>
#include <QtDebug>
#include <QRegExp>

#include "xmpp/base/randomnumbergenerator.h"

namespace XMPP {

	SCRAMSHA1Signature::SCRAMSHA1Signature(const QByteArray &server_final_message, const QCA::SecureArray &server_signature_should)
{
	QRegExp pattern("v=([^,]*)");
	int pos = pattern.indexIn(QString(server_final_message));
	isValid_ = true;
	if (pos > -1) {
		QString server_signature = pattern.cap(1);
		QCA::SecureArray server_sig(QCA::Base64().stringToArray(server_signature));
		if (server_sig != server_signature_should) isValid_ = false;
	} else {
		qWarning("SASL/SCRAM-SHA-1: Failed to match pattern for server-final-message.");
		isValid_ = false;
	}
}

}
