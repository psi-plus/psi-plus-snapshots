/*
 * Copyright (C) 2010  Tobias Markmann
 * See COPYING for license details.
 */

#include <QObject>
#include <QtTest/QtTest>
#include <QtCrypto>

#include "qttestutil/qttestutil.h"
#include "xmpp/base/unittest/incrementingrandomnumbergenerator.h"
#include "xmpp/sasl/scramsha1message.h"

using namespace XMPP;

class SCRAMSHA1MessageTest : public QObject
{
		Q_OBJECT

	private slots:
		void testConstructor_WithAuthzid() {
		}

		void testConstructor_WithoutAuthzid() {
			SCRAMSHA1Message msg1("", "testuser", QByteArray(0, ' '), IncrementingRandomNumberGenerator(255));
			QByteArray msg1_good("n,,n=testuser,r=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
			QVERIFY(msg1.isValid());
			QCOMPARE(msg1.getValue(), msg1_good);

			SCRAMSHA1Message msg2("", "username=test,man", QByteArray(0, ' '), IncrementingRandomNumberGenerator(255));
			QByteArray msg2_good("n,,n=username=3Dtest=2Cman,r=AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
			QVERIFY(msg2.isValid());
			QCOMPARE(msg2.getValue(), msg2_good);
		}
	
	private:
		QCA::Initializer initializer;
};

QTTESTUTIL_REGISTER_TEST(SCRAMSHA1MessageTest);
#include "scramsha1messagetest.moc"
