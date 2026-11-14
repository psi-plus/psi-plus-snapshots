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

#include "evpkey.h"
#include "keyutils.h"

#include <openssl/rsa.h>

#include <limits>

namespace opensslQCAPlugin {

namespace {
static SecureArray rsaRawSign(EVP_PKEY *pkey, const SecureArray &input)
{
    PkeyCtxPtr context(newPkeyContext(pkey));
    if (!context || EVP_PKEY_sign_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_PADDING) <= 0)
        return {};

    size_t outputSize = 0;
    if (EVP_PKEY_sign(context.get(),
                      nullptr,
                      &outputSize,
                      reinterpret_cast<const unsigned char *>(input.data()),
                      static_cast<size_t>(input.size())) <= 0 ||
        outputSize == 0 || outputSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return {};

    SecureArray output(static_cast<int>(outputSize));
    if (EVP_PKEY_sign(context.get(),
                      reinterpret_cast<unsigned char *>(output.data()),
                      &outputSize,
                      reinterpret_cast<const unsigned char *>(input.data()),
                      static_cast<size_t>(input.size())) <= 0 ||
        outputSize > static_cast<size_t>(output.size()))
        return {};

    output.resize(static_cast<int>(outputSize));
    return output;
}

static bool rsaRawVerifyRecover(EVP_PKEY *pkey, const SecureArray &signature, SecureArray *output)
{
    if (!output)
        return false;

    PkeyCtxPtr context(newPkeyContext(pkey));
    if (!context || EVP_PKEY_verify_recover_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(context.get(), RSA_PKCS1_PADDING) <= 0)
        return false;

    size_t outputSize = 0;
    if (EVP_PKEY_verify_recover(context.get(),
                                nullptr,
                                &outputSize,
                                reinterpret_cast<const unsigned char *>(signature.data()),
                                static_cast<size_t>(signature.size())) <= 0 ||
        outputSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    SecureArray recovered(static_cast<int>(outputSize));
    if (EVP_PKEY_verify_recover(context.get(),
                                reinterpret_cast<unsigned char *>(recovered.data()),
                                &outputSize,
                                reinterpret_cast<const unsigned char *>(signature.data()),
                                static_cast<size_t>(signature.size())) <= 0 ||
        outputSize > static_cast<size_t>(recovered.size()))
        return false;

    recovered.resize(static_cast<int>(outputSize));
    *output = recovered;
    return true;
}
} // namespace

EVPKey::EVPKey()
{
    pkey     = nullptr;
    raw_type = false;
    state    = Idle;
    mdctx    = EVP_MD_CTX_new();
}

EVPKey::EVPKey(const EVPKey &from)
{
    pkey = from.pkey;
    if (pkey)
        EVP_PKEY_up_ref(pkey);
    raw_type = false;
    state    = Idle;
    mdctx    = EVP_MD_CTX_new();
    EVP_MD_CTX_copy(mdctx, from.mdctx);
}

EVPKey::~EVPKey()
{
    reset();
    EVP_MD_CTX_free(mdctx);
}

void EVPKey::reset()
{
    EVP_PKEY_free(pkey);
    pkey = nullptr;
    raw.clear();
    raw_type = false;
}

void EVPKey::startSign(const EVP_MD *type)
{
    state = SignActive;
    if (!type) {
        raw_type = true;
        raw.clear();
    } else {
        raw_type = false;
        EVP_MD_CTX_init(mdctx);
        if (!EVP_SignInit_ex(mdctx, type, nullptr))
            state = SignError;
    }
}

void EVPKey::startSignError()
{
    raw_type = false;
    raw.clear();
    state = SignError;
}

void EVPKey::startVerify(const EVP_MD *type)
{
    state = VerifyActive;
    if (!type) {
        raw_type = true;
        raw.clear();
    } else {
        raw_type = false;
        EVP_MD_CTX_init(mdctx);
        if (!EVP_VerifyInit_ex(mdctx, type, nullptr))
            state = VerifyError;
    }
}

void EVPKey::startVerifyError()
{
    raw_type = false;
    raw.clear();
    state = VerifyError;
}

void EVPKey::update(const MemoryRegion &in)
{
    if (state == SignActive) {
        if (raw_type)
            raw += in;
        else if (!EVP_SignUpdate(mdctx, in.data(), static_cast<unsigned int>(in.size())))
            state = SignError;
    } else if (state == VerifyActive) {
        if (raw_type)
            raw += in;
        else if (!EVP_VerifyUpdate(mdctx, in.data(), static_cast<unsigned int>(in.size())))
            state = VerifyError;
    }
}

SecureArray EVPKey::endSign()
{
    if (state != SignActive)
        return {};

    SecureArray output;
    if (raw_type) {
        if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA) {
            state = SignError;
            return {};
        }

        output = rsaRawSign(pkey, raw);
        if (output.isEmpty()) {
            state = SignError;
            return {};
        }
    } else {
        const int maximumSize = EVP_PKEY_size(pkey);
        if (maximumSize <= 0) {
            state = SignError;
            return {};
        }

        output.resize(maximumSize);
        unsigned int outputSize = static_cast<unsigned int>(output.size());
        if (!EVP_SignFinal(mdctx, reinterpret_cast<unsigned char *>(output.data()), &outputSize, pkey)) {
            state = SignError;
            return {};
        }
        output.resize(static_cast<int>(outputSize));
    }

    state = Idle;
    return output;
}

bool EVPKey::endVerify(const SecureArray &sig)
{
    if (state != VerifyActive)
        return false;

    if (raw_type) {
        if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA) {
            state = VerifyError;
            return false;
        }

        SecureArray recovered;
        if (!rsaRawVerifyRecover(pkey, sig, &recovered) || recovered != raw) {
            state = VerifyError;
            return false;
        }
    } else if (EVP_VerifyFinal(mdctx,
                               reinterpret_cast<const unsigned char *>(sig.data()),
                               static_cast<unsigned int>(sig.size()),
                               pkey) != 1) {
        state = VerifyError;
        return false;
    }

    state = Idle;
    return true;
}

} // namespace opensslQCAPlugin
