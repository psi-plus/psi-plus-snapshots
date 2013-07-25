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

#ifndef INCREMENTINGRANDOMNUMBERGENERATOR_H
#define INCREMENTINGRANDOMNUMBERGENERATOR_H

#include <QtDebug>

#include "xmpp/base/randomnumbergenerator.h"

namespace XMPP {
	class IncrementingRandomNumberGenerator : public RandomNumberGenerator
	{
		public:
			IncrementingRandomNumberGenerator(int maximumNumber = 10) : maximumNumber_(maximumNumber), currentNumber_(maximumNumber_) {}

			virtual double generateNumber() const {
				currentNumber_ = (currentNumber_ + 1) % (maximumNumber_ + 1);
				return currentNumber_;
			}

			virtual double getMaximumGeneratedNumber() const {
				return maximumNumber_;
			}

		private:
			int maximumNumber_;
			mutable int currentNumber_;
	};
}

#endif
