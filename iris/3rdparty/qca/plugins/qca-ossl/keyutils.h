/*
 * Copyright (C) 2026  QCA contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "qcaprovider.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/param_build.h>
#endif

#include <initializer_list>
#include <memory>

namespace opensslQCAPlugin {

struct BnDeleter
{
    void operator()(BIGNUM *pointer) const
    {
        BN_free(pointer);
    }
};

struct BnClearDeleter
{
    void operator()(BIGNUM *pointer) const
    {
        BN_clear_free(pointer);
    }
};

struct BnCtxDeleter
{
    void operator()(BN_CTX *pointer) const
    {
        BN_CTX_free(pointer);
    }
};

struct PkeyDeleter
{
    void operator()(EVP_PKEY *pointer) const
    {
        EVP_PKEY_free(pointer);
    }
};

struct PkeyCtxDeleter
{
    void operator()(EVP_PKEY_CTX *pointer) const
    {
        EVP_PKEY_CTX_free(pointer);
    }
};

using BnPtr      = std::unique_ptr<BIGNUM, BnDeleter>;
using BnClearPtr = std::unique_ptr<BIGNUM, BnClearDeleter>;
using BnCtxPtr   = std::unique_ptr<BN_CTX, BnCtxDeleter>;
using PkeyPtr    = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;

EVP_PKEY_CTX *newPkeyContext(EVP_PKEY *pkey);
const EVP_MD *signatureDigestToEvp(QCA::SignatureDigest digest, bool legacyProviderAvailable);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
struct ParamBldDeleter
{
    void operator()(OSSL_PARAM_BLD *pointer) const
    {
        OSSL_PARAM_BLD_free(pointer);
    }
};

struct ParamDeleter
{
    void operator()(OSSL_PARAM *pointer) const
    {
        OSSL_PARAM_free(pointer);
    }
};

using ParamBldPtr = std::unique_ptr<OSSL_PARAM_BLD, ParamBldDeleter>;
using ParamPtr    = std::unique_ptr<OSSL_PARAM, ParamDeleter>;

struct PkeyBnParameter
{
    const char   *name;
    const BIGNUM *value;
};

EVP_PKEY *pkeyFromBnParameters(const char *algorithm, int selection, std::initializer_list<PkeyBnParameter> parameters);

QCA::BigInteger pkeyBnParameter(const EVP_PKEY *pkey, const char *name, bool sensitive = false);
#endif

} // namespace opensslQCAPlugin
