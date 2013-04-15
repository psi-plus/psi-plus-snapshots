/*
 * Copyright (C) 2010  Tobias Markmann
 * See COPYING for license details.
 */

#include "xmpp/sasl/scramsha1message.h"

#include <QString>
#include <QTextStream>
#include <QtCrypto>
#include <QDebug>

#include "xmpp/base/randomnumbergenerator.h"
#include "xmpp/jid/jid.h"
#include "xmpp/base64/base64.h"

namespace XMPP {

bool Normalize(const QString &username_in, QString &username_out ) {
	// SASLprep
	if (StringPrepCache::saslprep(username_in, 1024, username_out)) {
		// '=' -> '=3D'	 and ',' -> '=2C'
		username_out.replace("=", "=3D");
		username_out.replace(",", "=2C");
		return true;
	} else {
		return false;
	}
}

SCRAMSHA1Message::SCRAMSHA1Message(const QString& authzid, const QString& authcid, const QByteArray& cnonce, const RandomNumberGenerator& rand) : isValid_(true)
{
	QString result;
	QByteArray clientnonce;
	QString username;

	if (!Normalize(authcid, username)) {
		isValid_ = false;
		return;
	}

	if (cnonce.size() == 0) {
		// make a cnonce
		QByteArray a;
		a.resize(32);
		for(int n = 0; n < (int)a.size(); ++n) {
			a[n] = (char) rand.generateNumberBetween(0, 255); 
		}
		clientnonce = Base64::encode(a).toLatin1();
	} else clientnonce = cnonce;
	
	QTextStream(&result) << "n,";
	if (authzid.size() > 0) {
		QTextStream(&result) << authzid.toUtf8();
	}
	QTextStream(&result) << ",n=" << username << ",r=" << clientnonce;
	value_ = result.toUtf8();
}

}
