/*
 * Copyright (C) 2004-2007  Justin Karneges <justin@affinix.com>
 * Copyright (C) 2004-2006  Brad Hards <bradh@frogmouth.net>
 * Copyright (C) 2013-2016  Ivan Romanov <drizt@land.ru>
 * Copyright (C) 2017       Fabian Vogt <fabian@ritter-vogt.de>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#pragma once

#include "qcaprovider.h"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/x509v3.h>

using namespace QCA;
namespace opensslQCAPlugin {

// We need this to support openssl3 (could be removed once we only support >= 4 if that ever happens)
template<typename T> static void *our_X509V3_EXT_d2i(T *ext)
{
    using NonConstType = std::remove_const_t<T>;

    return X509V3_EXT_d2i(const_cast<NonConstType *>(ext));
}

// We need this to support openssl1 (could be removed once we only support >= 3 if that ever happens)
template<typename T> static int our_X509_NAME_get_index_by_NID(T *name, int nid, int lastpos)
{
    using NonConstType = std::remove_const_t<T>;

    return X509_NAME_get_index_by_NID(const_cast<NonConstType *>(name), nid, lastpos);
}

// We need this to support openssl1 (could be removed once we only support >= 3 if that ever happens)
template<typename T> static int our_X509_NAME_get_index_by_OBJ(T *name, const ASN1_OBJECT *obj, int lastpos)
{
    using NonConstType = std::remove_const_t<T>;

    return X509_NAME_get_index_by_OBJ(const_cast<NonConstType *>(name), obj, lastpos);
}

int        passphrase_cb(char *buf, int size, int rwflag, void *u);
EVP_PKEY  *createPkeyFromExisting(const RSAPrivateKey &key);
Validity   convert_verify_error(int err);
X509_NAME *new_cert_name(const CertificateInfo &info);
QByteArray bio2ba(BIO *b);

inline QByteArray qca_ASN1_STRING_toByteArray(const ASN1_STRING *x)
{
    if (!x)
        return {};

    const int            length = ASN1_STRING_length(x);
    const unsigned char *data   = ASN1_STRING_get0_data(x);

    if (length <= 0 || !data)
        return {};

    return QByteArray(reinterpret_cast<const char *>(data), length);
}

inline BIGNUM *bi2bn(const BigInteger &n)
{
    SecureArray buf = n.toArray();
    return BN_bin2bn((const unsigned char *)buf.data(), buf.size(), nullptr);
}

BigInteger        bn2bi(const BIGNUM *n);
inline BigInteger bn2bi_free(BIGNUM *n)
{
    BigInteger bi = bn2bi(n);
    BN_free(n);
    return bi;
}

}
