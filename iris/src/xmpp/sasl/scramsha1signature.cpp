/*
 * Copyright (C) 2010  Tobias Markmann
 * See COPYING for license details.
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
