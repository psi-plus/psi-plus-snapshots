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

// FIXME: Complete this

#include <QtTest/QtTest>
#include <QObject>

#include "qttestutil/qttestutil.h"
#include "xmpp/jid/jid.h"

using namespace XMPP;

class JidTest : public QObject
{
		Q_OBJECT

	private slots:
		void testConstructorWithString() {
			Jid testling("foo@bar/baz");

			QCOMPARE(testling.node(), QString("foo"));
			QCOMPARE(testling.domain(), QString("bar"));
			QCOMPARE(testling.resource(), QString("baz"));
		}
};

QTTESTUTIL_REGISTER_TEST(JidTest);
#include "jidtest.moc"
