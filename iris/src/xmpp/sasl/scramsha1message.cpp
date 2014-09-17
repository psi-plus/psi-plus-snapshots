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

#include "xmpp/sasl/scramsha1message.h"

#include <QString>
#include <QTextStream>
#include <QtCrypto>
#include <QDebug>

#include "xmpp/base/randomnumbergenerator.h"
#include "xmpp/jid/jid.h"

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
		clientnonce = a.toBase64();
	} else clientnonce = cnonce;

	QTextStream(&result) << "n,";
	if (authzid.size() > 0) {
		QTextStream(&result) << authzid.toUtf8();
	}
	QTextStream(&result) << ",n=" << username << ",r=" << clientnonce;
	value_ = result.toUtf8();
}

}
