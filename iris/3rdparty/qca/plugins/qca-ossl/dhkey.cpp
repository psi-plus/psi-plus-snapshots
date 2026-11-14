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

#include "dhkey.h"
#include "keyutils.h"
#include "utils.h"

#include <limits>
#include <memory>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#endif

namespace opensslQCAPlugin {

namespace {
static EVP_PKEY *dhFromBignums(const BIGNUM *p, const BIGNUM *g, const BIGNUM *y, const BIGNUM *x)
{
    if (!p || !g || !y)
        return nullptr;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (x) {
        return pkeyFromBnParameters("DH",
                                    EVP_PKEY_KEYPAIR,
                                    {{OSSL_PKEY_PARAM_FFC_P, p},
                                     {OSSL_PKEY_PARAM_FFC_G, g},
                                     {OSSL_PKEY_PARAM_PUB_KEY, y},
                                     {OSSL_PKEY_PARAM_PRIV_KEY, x}});
    }

    return pkeyFromBnParameters("DH",
                                EVP_PKEY_PUBLIC_KEY,
                                {{OSSL_PKEY_PARAM_FFC_P, p}, {OSSL_PKEY_PARAM_FFC_G, g}, {OSSL_PKEY_PARAM_PUB_KEY, y}});
#else
    struct DhDeleter
    {
        void operator()(DH *pointer) const
        {
            DH_free(pointer);
        }
    };

    std::unique_ptr<DH, DhDeleter> dh(DH_new());
    BnPtr                          pCopy(BN_dup(p));
    BnPtr                          gCopy(BN_dup(g));
    BnPtr                          yCopy(BN_dup(y));
    BnClearPtr                     xCopy(x ? BN_dup(x) : nullptr);

    if (!dh || !pCopy || !gCopy || !yCopy || (x && !xCopy))
        return nullptr;

    if (DH_set0_pqg(dh.get(), pCopy.get(), nullptr, gCopy.get()) != 1)
        return nullptr;

    pCopy.release();
    gCopy.release();

    if (DH_set0_key(dh.get(), yCopy.get(), xCopy.get()) != 1)
        return nullptr;

    yCopy.release();
    if (x)
        xCopy.release();

    EVP_PKEY *result = EVP_PKEY_new();
    if (!result)
        return nullptr;

    if (EVP_PKEY_assign_DH(result, dh.get()) != 1) {
        EVP_PKEY_free(result);
        return nullptr;
    }

    dh.release();
    return result;
#endif
}

static EVP_PKEY *dhFromParameters(const DLGroup &domain, const BigInteger &y, const BigInteger *x)
{
    BnPtr      p(bi2bn(domain.p()));
    BnPtr      g(bi2bn(domain.g()));
    BnPtr      publicKey(bi2bn(y));
    BnClearPtr privateKey(x ? bi2bn(*x) : nullptr);

    if (!p || !g || !publicKey || (x && !privateKey))
        return nullptr;

    if (privateKey)
        BN_set_flags(privateKey.get(), BN_FLG_CONSTTIME);

    return dhFromBignums(p.get(), g.get(), publicKey.get(), privateKey.get());
}

static EVP_PKEY *dhPublicKey(const DLGroup &domain, const BigInteger &y)
{
    return dhFromParameters(domain, y, nullptr);
}

static EVP_PKEY *dhPrivateKey(const DLGroup &domain, const BigInteger &y, const BigInteger &x)
{
    return dhFromParameters(domain, y, &x);
}

static EVP_PKEY *generateDhKey(const DLGroup &domain)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    BnPtr p(bi2bn(domain.p()));
    BnPtr g(bi2bn(domain.g()));
    if (!p || !g)
        return nullptr;

    PkeyPtr parameters(pkeyFromBnParameters(
        "DH", EVP_PKEY_KEY_PARAMETERS, {{OSSL_PKEY_PARAM_FFC_P, p.get()}, {OSSL_PKEY_PARAM_FFC_G, g.get()}}));
    if (!parameters)
        return nullptr;

    PkeyCtxPtr context(newPkeyContext(parameters.get()));
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0)
        return nullptr;

    EVP_PKEY *result = nullptr;
    if (EVP_PKEY_keygen(context.get(), &result) <= 0) {
        EVP_PKEY_free(result);
        return nullptr;
    }

    return result;
#else
    struct DhDeleter
    {
        void operator()(DH *pointer) const
        {
            DH_free(pointer);
        }
    };

    std::unique_ptr<DH, DhDeleter> dh(DH_new());
    BnPtr                          p(bi2bn(domain.p()));
    BnPtr                          g(bi2bn(domain.g()));
    if (!dh || !p || !g)
        return nullptr;

    if (DH_set0_pqg(dh.get(), p.get(), nullptr, g.get()) != 1)
        return nullptr;

    p.release();
    g.release();

    if (DH_generate_key(dh.get()) != 1)
        return nullptr;

    EVP_PKEY *result = EVP_PKEY_new();
    if (!result)
        return nullptr;

    if (EVP_PKEY_assign_DH(result, dh.get()) != 1) {
        EVP_PKEY_free(result);
        return nullptr;
    }

    dh.release();
    return result;
#endif
}
} // namespace

//----------------------------------------------------------------------------
// DHKey
//----------------------------------------------------------------------------
class DHKeyMaker : public QThread
{
    Q_OBJECT
public:
    DLGroup   domain;
    EVP_PKEY *result;

    DHKeyMaker(const DLGroup &_domain, QObject *parent = nullptr)
        : QThread(parent)
        , domain(_domain)
        , result(nullptr)
    {
    }

    ~DHKeyMaker() override
    {
        wait();
        EVP_PKEY_free(result);
    }

    void run() override
    {
        result = generateDhKey(domain);
    }

    EVP_PKEY *takeResult()
    {
        EVP_PKEY *pkey = result;
        result         = nullptr;
        return pkey;
    }
};

DHKey::DHKey(Provider *p)
    : DHContext(p)
{
    keymaker = nullptr;
    sec      = false;
}

DHKey::DHKey(const DHKey &from)
    : DHContext(from.provider())
    , evp(from.evp)
{
    keymaker = nullptr;
    sec      = from.sec;
}

DHKey::~DHKey()
{
    delete keymaker;
}

Provider::Context *DHKey::clone() const
{
    return new DHKey(*this);
}

bool DHKey::isNull() const
{
    return !evp.pkey;
}

PKey::Type DHKey::type() const
{
    return PKey::DH;
}

bool DHKey::isPrivate() const
{
    return sec;
}

bool DHKey::canExport() const
{
    return true;
}

void DHKey::convertToPublic()
{
    if (!sec)
        return;

    EVP_PKEY *publicKey = dhPublicKey(domain(), y());
    if (!publicKey)
        return;

    evp.reset();
    evp.pkey = publicKey;
    sec      = false;
}

int DHKey::bits() const
{
    return EVP_PKEY_bits(evp.pkey);
}

SymmetricKey DHKey::deriveKey(const PKeyBase &theirs)
{
    const auto *peer = static_cast<const DHKey *>(&theirs);
    if (!evp.pkey || !peer->evp.pkey)
        return {};

    PkeyCtxPtr context(newPkeyContext(evp.pkey));
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0 ||
        EVP_PKEY_derive_set_peer(context.get(), peer->evp.pkey) <= 0)
        return {};

    size_t outputSize = 0;
    if (EVP_PKEY_derive(context.get(), nullptr, &outputSize) <= 0 || outputSize == 0 ||
        outputSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return {};

    SecureArray result(static_cast<int>(outputSize));
    if (EVP_PKEY_derive(context.get(), reinterpret_cast<unsigned char *>(result.data()), &outputSize) <= 0 ||
        outputSize > static_cast<size_t>(result.size()))
        return {};

    result.resize(static_cast<int>(outputSize));
    return SymmetricKey(result);
}

void DHKey::createPrivate(const DLGroup &domain, bool block)
{
    evp.reset();
    sec = false;

    keymaker    = new DHKeyMaker(domain, !block ? this : nullptr);
    wasBlocking = block;
    if (block) {
        keymaker->run();
        km_finished();
    } else {
        connect(keymaker, &DHKeyMaker::finished, this, &DHKey::km_finished);
        keymaker->start();
    }
}

void DHKey::createPrivate(const DLGroup &domain, const BigInteger &y, const BigInteger &x)
{
    evp.reset();
    sec = false;

    evp.pkey = dhPrivateKey(domain, y, x);
    if (evp.pkey)
        sec = true;
}

void DHKey::createPublic(const DLGroup &domain, const BigInteger &y)
{
    evp.reset();
    sec = false;

    evp.pkey = dhPublicKey(domain, y);
}

DLGroup DHKey::domain() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return DLGroup(pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_FFC_P), pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_FFC_G));
#else
    const DH     *dh = EVP_PKEY_get0_DH(evp.pkey);
    const BIGNUM *p  = nullptr;
    const BIGNUM *g  = nullptr;
    DH_get0_pqg(dh, &p, nullptr, &g);
    return DLGroup(bn2bi(p), bn2bi(g));
#endif
}

BigInteger DHKey::y() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_PUB_KEY);
#else
    const DH     *dh        = EVP_PKEY_get0_DH(evp.pkey);
    const BIGNUM *publicKey = nullptr;
    DH_get0_key(dh, &publicKey, nullptr);
    return bn2bi(publicKey);
#endif
}

BigInteger DHKey::x() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_PRIV_KEY, true);
#else
    const DH     *dh         = EVP_PKEY_get0_DH(evp.pkey);
    const BIGNUM *privateKey = nullptr;
    DH_get0_key(dh, nullptr, &privateKey);
    return bn2bi(privateKey);
#endif
}

void DHKey::km_finished()
{
    EVP_PKEY *pkey = keymaker->takeResult();
    if (wasBlocking)
        delete keymaker;
    else
        keymaker->deleteLater();
    keymaker = nullptr;

    if (pkey) {
        evp.pkey = pkey;
        sec      = true;
    }

    if (!wasBlocking)
        emit finished();
}

} // namespace opensslQCAPlugin

#include "dhkey.moc"
