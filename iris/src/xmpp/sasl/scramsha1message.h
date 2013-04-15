/*
 * Copyright (C) 2010  Tobias Markmann
 * See COPYING for license details.
 */

#ifndef SCRAMSHA1MESSAGE_H
#define SCRAMSHA1MESSAGE_H

#include <QByteArray>
#include <QString>

#include "xmpp/base/randomnumbergenerator.h"

namespace XMPP {
	class SCRAMSHA1Message
	{
		public:
			SCRAMSHA1Message(const QString& authzid, const QString& authcid, const QByteArray& cnonce, const RandomNumberGenerator& rand);

			const QByteArray& getValue() {
				return value_;
			}

			bool isValid() const {
				return isValid_;
			}

		private:
			QByteArray value_;
			bool isValid_;
	};
}

#endif
