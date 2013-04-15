/*
 * Copyright (C) 2008  Remko Troncon
 * See COPYING for license details.
 */

#include <QObject>
#include <QtTest/QtTest>
#include <QtCrypto>

#include "qttestutil/qttestutil.h"
#include "xmpp/sasl/scramsha1response.h"
#include "xmpp/sasl/scramsha1signature.h"
#include "xmpp/base/unittest/incrementingrandomnumbergenerator.h"

using namespace XMPP;

class SCRAMSHA1ResponseTest : public QObject
{
		Q_OBJECT

	private slots:
		void testConstructor_WithAuthzid() {

		}

		void testConstructor_WithoutAuthzid() {
			if (QCA::isSupported("hmac(sha1)")) {
				SCRAMSHA1Response resp1("r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096",
									"pencil", "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL", "", IncrementingRandomNumberGenerator(255));
				const QCA::SecureArray sig = resp1.getServerSignature();
				QByteArray resp_sig("v=rmF9pqV8S7suAoZWja4dJRkFsKQ=");
				SCRAMSHA1Signature sig1(resp_sig, sig);
				QByteArray resp1_ok("c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts=");
				QCOMPARE(resp1.getValue(), resp1_ok);
				QVERIFY(sig1.isValid());
			} else {
				QFAIL("hmac(sha1) not supported in QCA.");
			}
		}
	
	private:
		QCA::Initializer initializer;
};

QTTESTUTIL_REGISTER_TEST(SCRAMSHA1ResponseTest);
#include "scramsha1responsetest.moc"
