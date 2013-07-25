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

#ifndef QTTESTUTIL_TESTREGISTRY_H
#define QTTESTUTIL_TESTREGISTRY_H

#include <QList>

class QObject;

namespace QtTestUtil {

	/**
	 * A registry of QtTest test classes.
	 * All test classes registered with QTTESTUTIL_REGISTER_TEST add
	 * themselves to this registry. All registered tests can then be run at
	 * once using runTests().
	 */
	class TestRegistry
	{
		public:
			/**
			 * Retrieve the single instance of the registry.
			 */
			static TestRegistry* getInstance();

			/**
			 * Register a QtTest test.
			 * This method is called  by QTTESTUTIL_REGISTER_TEST, and you should
			 * not use this method directly.
			 */
			void registerTest(QObject*);

			/**
			 * Run all registered tests using QTest::qExec()
			 */
			int runTests(int argc, char* argv[]);

		private:
			TestRegistry() {}

		private:
			QList<QObject*> tests_;
	};
}

#endif
