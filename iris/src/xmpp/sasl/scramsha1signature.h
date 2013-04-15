/*
 * Copyright (C) 2010  Tobias Markmann
 * See COPYING for license details.
 */

#ifndef SCRAMSHA1SIGNATURE_H
#define SCRAMSHA1SIGNATURE_H

#include <QByteArray>
#include <QString>
#include <QtCrypto>

namespace XMPP {
	class SCRAMSHA1Signature
	{
		public:
			SCRAMSHA1Signature(const QByteArray &server_final_message, const QCA::SecureArray &server_signature_should);

			bool isValid() const { 
				return isValid_;
			}
		
		private:
			bool isValid_;
	};
}

#endif
