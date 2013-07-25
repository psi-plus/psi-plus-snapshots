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

#include "qttestutil/qttestutil.h"
#include "xmpp/base/randomnumbergenerator.h"

using namespace XMPP;

class RandomNumberGeneratorTest : public QObject
{
     Q_OBJECT

	private:
		class DummyRandomNumberGenerator : public RandomNumberGenerator
		{
			public:
				DummyRandomNumberGenerator(double value, double maximum) : value_(value), maximum_(maximum) {}

				double generateNumber() const { return value_; }
				double getMaximumGeneratedNumber() const { return maximum_; }

			private:
				double value_;
				double maximum_;
		};

	private slots:
		void testGenerateNumberBetween() {
			DummyRandomNumberGenerator testling(5,10);
			QCOMPARE(75.0, testling.generateNumberBetween(50.0, 100.0));
		}

		void testGenerateNumberBetween_Minimum() {
			DummyRandomNumberGenerator testling(0,10);
			QCOMPARE(0.0, testling.generateNumberBetween(0.0, 100.0));
		}

		void testGenerateNumberBetween_Maximum() {
			DummyRandomNumberGenerator testling(10,10);
			QCOMPARE(100.0, testling.generateNumberBetween(0.0, 100.0));
		}
};

QTTESTUTIL_REGISTER_TEST(RandomNumberGeneratorTest);
#include "randomnumbergeneratortest.moc"
