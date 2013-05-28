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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <QObject>
#include <QtTest/QtTest>

#include "xmpp/sasl/plainmessage.h"
#include "qttestutil/qttestutil.h"

using namespace XMPP;

class PlainMessageTest : public QObject
{
		Q_OBJECT

	private slots:
		void testConstructor_WithoutAuthzID() {
			PLAINMessage message("", QString("user"), "pass");
			QCOMPARE(message.getValue(), QByteArray("\0user\0pass", 10));
		}

		void testConstructor_WithAuthzID() {
			PLAINMessage message(QString("authz"), QString("user"), "pass");
			QCOMPARE(message.getValue(), QByteArray("authz\0user\0pass", 15));
		}

		void testConstructor_WithNonASCIICharacters() {
			PLAINMessage message(QString("authz") + QChar(0x03A8) /* psi */, QString("user") + QChar(0x03A8) /* psi */, "pass");
			QCOMPARE(message.getValue(), QByteArray("authz\xCE\xA8\0user\xCE\xA8\0pass", 19));
		}
};

QTTESTUTIL_REGISTER_TEST(PlainMessageTest);
#include "plainmessagetest.moc"
