/*
 * qca_bigint.cpp - Qt Cryptographic Architecture
 * Copyright (C) 2026  Psi developers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "qca_bigint.h"

namespace QCA {
namespace {

BigInteger positiveMod(const BigInteger &value, const BigInteger &modulus)
{
    const BigInteger zero(0);
    if (modulus <= zero)
        return zero;

    BigInteger result(value);
    result %= modulus;
    if (result < zero)
        result += modulus;
    return result;
}

} // namespace

namespace BigIntegerMath {

BigInteger modPow(const BigInteger &base, const BigInteger &exponent, const BigInteger &modulus)
{
    const BigInteger zero(0);
    const BigInteger two(2);

    if (modulus <= zero || exponent < zero)
        return zero;

    BigInteger result(1);
    result %= modulus;

    BigInteger factor = positiveMod(base, modulus);
    BigInteger power(exponent);

    while (power > zero) {
        BigInteger bit(power);
        bit %= two;
        if (bit != zero) {
            result *= factor;
            result %= modulus;
        }

        power /= two;
        if (power != zero) {
            // The bundled Botan BigInt multiplication is not guaranteed to
            // support the same object as both input and output for large
            // values. Keep the right-hand operand on the original shared
            // data while the left-hand copy detaches before multiplication.
            BigInteger squared(factor);
            squared *= factor;
            squared %= modulus;
            factor = squared;
        }
    }

    return result;
}

bool modInverse(const BigInteger &value, const BigInteger &modulus, BigInteger *inverse)
{
    const BigInteger zero(0);
    const BigInteger one(1);

    if (!inverse || modulus <= one)
        return false;

    BigInteger t(0);
    BigInteger newT(1);
    BigInteger r(modulus);
    BigInteger newR = positiveMod(value, modulus);

    while (newR != zero) {
        BigInteger quotient(r);
        quotient /= newR;

        BigInteger product(quotient);
        product *= newT;
        BigInteger nextT(t);
        nextT -= product;
        t    = newT;
        newT = nextT;

        product = quotient;
        product *= newR;
        BigInteger nextR(r);
        nextR -= product;
        r    = newR;
        newR = nextR;
    }

    if (r != one)
        return false;

    *inverse = positiveMod(t, modulus);
    return true;
}

} // namespace BigIntegerMath
} // namespace QCA
