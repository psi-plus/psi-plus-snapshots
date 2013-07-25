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

#ifndef QTTESTUTIL_H
#define QTTESTUTIL_H

#include "qttestutil/testregistration.h"

/**
 * A macro to register a test class.
 *
 * This macro will create a static variable which registers the
 * testclass with the TestRegistry, and creates an instance of the
 * test class.
 *
 * Execute this macro in the body of your unit test's .cpp file, e.g.
 *    class MyTest {
 *			...
 *		};
 *
 *		QTTESTUTIL_REGISTER_TEST(MyTest)
 */
#define QTTESTUTIL_REGISTER_TEST(TestClass) \
	static QtTestUtil::TestRegistration<TestClass> TestClass##Registration

#endif
