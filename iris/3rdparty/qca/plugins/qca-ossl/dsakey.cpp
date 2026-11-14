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

#include "dsakey.h"
#include "keyutils.h"
#include "utils.h"

#include <cstring>
#include <memory>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#endif

namespace opensslQCAPlugin {

extern bool s_legacyProviderAvailable;

namespace {
struct DsaSigDeleter
{
    void operator()(DSA_SIG *pointer) const
    {
        DSA_SIG_free(pointer);
    }
};

using DsaSigPtr = std::unique_ptr<DSA_SIG, DsaSigDeleter>;

// Take the lowest bytes of a BIGNUM and pad with leading zeroes.
static SecureArray bn2fixedbuf(const BIGNUM *number, int size)
{
    if (!number || size < 0)
        return {};

    SecureArray buffer(BN_num_bytes(number));
    if (buffer.size() > 0)
        BN_bn2bin(number, reinterpret_cast<unsigned char *>(buffer.data()));

    SecureArray result(size);
    if (size > 0)
        std::memset(result.data(), 0, static_cast<size_t>(size));

    const int length = qMin(size, buffer.size());
    if (length > 0)
        std::memcpy(result.data() + (size - length), buffer.data(), static_cast<size_t>(length));

    return result;
}

static SecureArray dsasig_der_to_raw(const SecureArray &input)
{
    const unsigned char *data = reinterpret_cast<const unsigned char *>(input.data());
    DsaSigPtr            sig(d2i_DSA_SIG(nullptr, &data, input.size()));
    if (!sig)
        return {};

    const BIGNUM *r = nullptr;
    const BIGNUM *s = nullptr;
    DSA_SIG_get0(sig.get(), &r, &s);
    if (!r || !s)
        return {};

    SecureArray result;
    result.append(bn2fixedbuf(r, 20));
    result.append(bn2fixedbuf(s, 20));
    return result;
}

static SecureArray dsasig_raw_to_der(const SecureArray &input)
{
    if (input.size() != 40)
        return {};

    DsaSigPtr sig(DSA_SIG_new());
    BnPtr     r(BN_bin2bn(reinterpret_cast<const unsigned char *>(input.data()), 20, nullptr));
    BnPtr     s(BN_bin2bn(reinterpret_cast<const unsigned char *>(input.data() + 20), 20, nullptr));
    if (!sig || !r || !s || DSA_SIG_set0(sig.get(), r.get(), s.get()) != 1)
        return {};

    r.release();
    s.release();

    const int length = i2d_DSA_SIG(sig.get(), nullptr);
    if (length <= 0)
        return {};

    SecureArray    result(length);
    unsigned char *data = reinterpret_cast<unsigned char *>(result.data());
    if (i2d_DSA_SIG(sig.get(), &data) != length)
        return {};

    return result;
}

static EVP_PKEY *dsaFromBignums(const BIGNUM *p, const BIGNUM *q, const BIGNUM *g, const BIGNUM *y, const BIGNUM *x)
{
    if (!p || !q || !g || !y)
        return nullptr;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (x) {
        return pkeyFromBnParameters("DSA",
                                    EVP_PKEY_KEYPAIR,
                                    {{OSSL_PKEY_PARAM_FFC_P, p},
                                     {OSSL_PKEY_PARAM_FFC_Q, q},
                                     {OSSL_PKEY_PARAM_FFC_G, g},
                                     {OSSL_PKEY_PARAM_PUB_KEY, y},
                                     {OSSL_PKEY_PARAM_PRIV_KEY, x}});
    }

    return pkeyFromBnParameters("DSA",
                                EVP_PKEY_PUBLIC_KEY,
                                {{OSSL_PKEY_PARAM_FFC_P, p},
                                 {OSSL_PKEY_PARAM_FFC_Q, q},
                                 {OSSL_PKEY_PARAM_FFC_G, g},
                                 {OSSL_PKEY_PARAM_PUB_KEY, y}});
#else
    struct DsaDeleter
    {
        void operator()(DSA *pointer) const
        {
            DSA_free(pointer);
        }
    };

    std::unique_ptr<DSA, DsaDeleter> dsa(DSA_new());
    BnPtr                            pCopy(BN_dup(p));
    BnPtr                            qCopy(BN_dup(q));
    BnPtr                            gCopy(BN_dup(g));
    BnPtr                            yCopy(BN_dup(y));
    BnClearPtr                       xCopy(x ? BN_dup(x) : nullptr);

    if (!dsa || !pCopy || !qCopy || !gCopy || !yCopy || (x && !xCopy))
        return nullptr;

    if (DSA_set0_pqg(dsa.get(), pCopy.get(), qCopy.get(), gCopy.get()) != 1)
        return nullptr;

    pCopy.release();
    qCopy.release();
    gCopy.release();

    if (DSA_set0_key(dsa.get(), yCopy.get(), xCopy.get()) != 1)
        return nullptr;

    yCopy.release();
    if (x)
        xCopy.release();

    EVP_PKEY *result = EVP_PKEY_new();
    if (!result)
        return nullptr;

    if (EVP_PKEY_assign_DSA(result, dsa.get()) != 1) {
        EVP_PKEY_free(result);
        return nullptr;
    }

    dsa.release();
    return result;
#endif
}

static EVP_PKEY *dsaFromParameters(const DLGroup &domain, const BigInteger &y, const BigInteger *x)
{
    BnPtr      p(bi2bn(domain.p()));
    BnPtr      q(bi2bn(domain.q()));
    BnPtr      g(bi2bn(domain.g()));
    BnPtr      publicKey(bi2bn(y));
    BnClearPtr privateKey(x ? bi2bn(*x) : nullptr);

    if (!p || !q || !g || !publicKey || (x && !privateKey))
        return nullptr;

    if (privateKey)
        BN_set_flags(privateKey.get(), BN_FLG_CONSTTIME);

    return dsaFromBignums(p.get(), q.get(), g.get(), publicKey.get(), privateKey.get());
}

static EVP_PKEY *dsaPublicKey(const DLGroup &domain, const BigInteger &y)
{
    return dsaFromParameters(domain, y, nullptr);
}

static EVP_PKEY *dsaPrivateKey(const DLGroup &domain, const BigInteger &y, const BigInteger &x)
{
    return dsaFromParameters(domain, y, &x);
}

static EVP_PKEY *generateDsaKey(const DLGroup &domain)
{
    BnPtr p(bi2bn(domain.p()));
    BnPtr q(bi2bn(domain.q()));
    BnPtr g(bi2bn(domain.g()));
    if (!p || !q || !g || BN_is_negative(p.get()) || BN_is_negative(q.get()) || BN_is_negative(g.get()) ||
        !BN_is_odd(p.get()) || BN_cmp(p.get(), BN_value_one()) <= 0 || BN_cmp(q.get(), BN_value_one()) <= 0 ||
        BN_cmp(g.get(), BN_value_one()) <= 0 || BN_cmp(q.get(), p.get()) >= 0 || BN_cmp(g.get(), p.get()) >= 0)
        return nullptr;

    BnClearPtr privateKey(BN_secure_new());
    BnPtr      publicKey(BN_new());
    BnCtxPtr   context(BN_CTX_secure_new());
    if (!privateKey || !publicKey || !context)
        return nullptr;

    BN_set_flags(privateKey.get(), BN_FLG_CONSTTIME);

    do {
        if (BN_priv_rand_range(privateKey.get(), q.get()) != 1)
            return nullptr;
    } while (BN_is_zero(privateKey.get()));

    if (BN_mod_exp_mont_consttime(publicKey.get(), g.get(), privateKey.get(), p.get(), context.get(), nullptr) != 1)
        return nullptr;

    return dsaFromBignums(p.get(), q.get(), g.get(), publicKey.get(), privateKey.get());
}
} // namespace

//----------------------------------------------------------------------------
// DSAKey
//----------------------------------------------------------------------------
class DSAKeyMaker : public QThread
{
    Q_OBJECT
public:
    DLGroup   domain;
    EVP_PKEY *result;

    DSAKeyMaker(const DLGroup &_domain, QObject *parent = nullptr)
        : QThread(parent)
        , domain(_domain)
        , result(nullptr)
    {
    }

    ~DSAKeyMaker() override
    {
        wait();
        EVP_PKEY_free(result);
    }

    void run() override
    {
        result = generateDsaKey(domain);
    }

    EVP_PKEY *takeResult()
    {
        EVP_PKEY *pkey = result;
        result         = nullptr;
        return pkey;
    }
};

DSAKey::DSAKey(Provider *p)
    : DSAContext(p)
{
    keymaker = nullptr;
    sec      = false;
}

DSAKey::DSAKey(const DSAKey &from)
    : DSAContext(from.provider())
    , evp(from.evp)
{
    keymaker = nullptr;
    sec      = from.sec;
}

DSAKey::~DSAKey()
{
    delete keymaker;
}

Provider::Context *DSAKey::clone() const
{
    return new DSAKey(*this);
}

bool DSAKey::isNull() const
{
    return !evp.pkey;
}

PKey::Type DSAKey::type() const
{
    return PKey::DSA;
}

bool DSAKey::isPrivate() const
{
    return sec;
}

bool DSAKey::canExport() const
{
    return true;
}

void DSAKey::convertToPublic()
{
    if (!sec)
        return;

    EVP_PKEY *publicKey = dsaPublicKey(domain(), y());
    if (!publicKey)
        return;

    evp.reset();
    evp.pkey = publicKey;
    sec      = false;
}

int DSAKey::bits() const
{
    return EVP_PKEY_bits(evp.pkey);
}

void DSAKey::startSign(SignatureAlgorithm alg, SignatureFormat format)
{
    // OpenSSL native format is DER, so transform otherwise.
    transformsig = format != DERSequence;
    if (alg.scheme != SignatureScheme::DSA || alg.digest == SignatureDigest::None) {
        evp.startSignError();
        return;
    }

    const EVP_MD *md = signatureDigestToEvp(alg.digest, s_legacyProviderAvailable);
    if (!md) {
        evp.startSignError();
        return;
    }
    evp.startSign(md);
}

void DSAKey::startVerify(SignatureAlgorithm alg, SignatureFormat format)
{
    // OpenSSL native format is DER, so transform otherwise.
    transformsig = format != DERSequence;
    if (alg.scheme != SignatureScheme::DSA || alg.digest == SignatureDigest::None) {
        evp.startVerifyError();
        return;
    }

    const EVP_MD *md = signatureDigestToEvp(alg.digest, s_legacyProviderAvailable);
    if (!md) {
        evp.startVerifyError();
        return;
    }
    evp.startVerify(md);
}

void DSAKey::update(const MemoryRegion &in)
{
    evp.update(in);
}

QByteArray DSAKey::endSign()
{
    const SecureArray output = evp.endSign();
    return transformsig ? dsasig_der_to_raw(output).toByteArray() : output.toByteArray();
}

bool DSAKey::endVerify(const QByteArray &sig)
{
    SecureArray input;
    if (transformsig)
        input = dsasig_raw_to_der(sig);
    else
        input = sig;
    return evp.endVerify(input);
}

void DSAKey::createPrivate(const DLGroup &domain, bool block)
{
    evp.reset();
    sec = false;

    keymaker    = new DSAKeyMaker(domain, !block ? this : nullptr);
    wasBlocking = block;
    if (block) {
        keymaker->run();
        km_finished();
    } else {
        connect(keymaker, &DSAKeyMaker::finished, this, &DSAKey::km_finished);
        keymaker->start();
    }
}

void DSAKey::createPrivate(const DLGroup &domain, const BigInteger &y, const BigInteger &x)
{
    evp.reset();
    sec = false;

    evp.pkey = dsaPrivateKey(domain, y, x);
    if (evp.pkey)
        sec = true;
}

void DSAKey::createPublic(const DLGroup &domain, const BigInteger &y)
{
    evp.reset();
    sec = false;

    evp.pkey = dsaPublicKey(domain, y);
}

DLGroup DSAKey::domain() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return DLGroup(pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_FFC_P),
                   pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_FFC_Q),
                   pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_FFC_G));
#else
    const DSA    *dsa = EVP_PKEY_get0_DSA(evp.pkey);
    const BIGNUM *p   = nullptr;
    const BIGNUM *q   = nullptr;
    const BIGNUM *g   = nullptr;
    DSA_get0_pqg(dsa, &p, &q, &g);
    return DLGroup(bn2bi(p), bn2bi(q), bn2bi(g));
#endif
}

BigInteger DSAKey::y() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_PUB_KEY);
#else
    const DSA    *dsa       = EVP_PKEY_get0_DSA(evp.pkey);
    const BIGNUM *publicKey = nullptr;
    DSA_get0_key(dsa, &publicKey, nullptr);
    return bn2bi(publicKey);
#endif
}

BigInteger DSAKey::x() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_PRIV_KEY, true);
#else
    const DSA    *dsa        = EVP_PKEY_get0_DSA(evp.pkey);
    const BIGNUM *privateKey = nullptr;
    DSA_get0_key(dsa, nullptr, &privateKey);
    return bn2bi(privateKey);
#endif
}

void DSAKey::km_finished()
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

#include "dsakey.moc"
