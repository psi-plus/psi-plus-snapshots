/*
 * Copyright (C) 2008  Remko Troncon
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
#include "qttestutil/qttestutil.h"
#include "xmpp/base/unittest/incrementingrandomnumbergenerator.h"
#include "xmpp/sasl/scramsha1signature.h"

#include <QObject>
#include <QtCrypto>
#include <QtTest/QtTest>

using namespace XMPP;

class SCRAMSHA1ResponseTest : public QObject {
    Q_OBJECT

private slots:
    void testConstructor_WithAuthzid() { }

    void testConstructor_WithoutAuthzid()
    {
        if (QCA::isSupported("hmac(sha1)")) {
            SCRAMSHA1Response resp1("r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,i=4096", "pencil",
                                    "n,,n=user,r=fyko+d2lbbFgONRv9qkxdawL", "", IncrementingRandomNumberGenerator(255));
            const QCA::SecureArray sig = resp1.getServerSignature();
            QByteArray             resp_sig("v=rmF9pqV8S7suAoZWja4dJRkFsKQ=");
            SCRAMSHA1Signature     sig1(resp_sig, sig);
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
