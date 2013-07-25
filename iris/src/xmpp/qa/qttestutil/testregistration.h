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

#ifndef QTTESTUTIL_TESTREGISTRATION_H
#define QTTESTUTIL_TESTREGISTRATION_H

#include "qttestutil/testregistry.h"

namespace QtTestUtil {

	/**
	 * A wrapper class around a test to manage registration and static
	 * creation of an instance of the test class.
	 * This class is used by QTTESTUTIL_REGISTER_TEST(), and you should not
	 * use this class directly.
	 */
	template<typename TestClass>
	class TestRegistration
	{
		public:
			TestRegistration() {
				test_ = new TestClass();
				TestRegistry::getInstance()->registerTest(test_);
			}

			~TestRegistration() {
				delete test_;
			}

		private:
			TestClass* test_;
	};

}

#endif
