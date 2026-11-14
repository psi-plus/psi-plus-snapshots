/*
 * qca_bigint.h - Qt Cryptographic Architecture
 * Copyright (C) 2026  Psi developers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/**
   \file qca_bigint.h

   Modular arithmetic helpers for %QCA BigInteger.

   \note You should not use this header directly from an
   application. You should just use <tt> \#include \<QtCrypto>
   </tt> instead.
*/

#ifndef QCA_BIGINT_H
#define QCA_BIGINT_H

#include "qca_tools.h"

namespace QCA {
namespace BigIntegerMath {

/**
   Compute base^exponent modulo modulus using binary exponentiation.

   The exponent must be non-negative and the modulus must be positive.
   Invalid arguments return zero.
*/
QCA_EXPORT BigInteger modPow(const BigInteger &base, const BigInteger &exponent, const BigInteger &modulus);

/**
   Compute the multiplicative inverse of \a value modulo \a modulus.

   Returns false when the modulus is invalid, \a inverse is null, or the
   inverse does not exist because value and modulus are not coprime.
*/
QCA_EXPORT bool modInverse(const BigInteger &value, const BigInteger &modulus, BigInteger *inverse);

} // namespace BigIntegerMath
} // namespace QCA

#endif
