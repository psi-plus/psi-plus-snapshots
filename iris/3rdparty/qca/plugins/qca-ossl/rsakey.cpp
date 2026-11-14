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

#include "rsakey.h"
#include "keyutils.h"
#include "utils.h"

#include <limits>
#include <memory>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#endif

namespace opensslQCAPlugin {

extern bool s_legacyProviderAvailable;

//----------------------------------------------------------------------------
// RSAKey
//----------------------------------------------------------------------------
namespace {
static EVP_PKEY *generateRsaKey(int bits, int exp)
{
    if (bits <= 0 || exp <= 0)
        return nullptr;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    PkeyCtxPtr context(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0)
        return nullptr;

    BnClearPtr exponent(BN_new());
    if (!exponent || BN_set_word(exponent.get(), static_cast<BN_ULONG>(exp)) != 1)
        return nullptr;

    const auto keyBits = static_cast<unsigned int>(bits);

    ParamBldPtr builder(OSSL_PARAM_BLD_new());
    if (!builder || OSSL_PARAM_BLD_push_uint(builder.get(), OSSL_PKEY_PARAM_RSA_BITS, keyBits) != 1 ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponent.get()) != 1)
        return nullptr;

    ParamPtr params(OSSL_PARAM_BLD_to_param(builder.get()));
    if (!params || EVP_PKEY_CTX_set_params(context.get(), params.get()) <= 0)
        return nullptr;

    EVP_PKEY *result = nullptr;
    if (EVP_PKEY_keygen(context.get(), &result) <= 0) {
        EVP_PKEY_free(result);
        return nullptr;
    }
    return result;
#else
    struct RsaDeleter
    {
        void operator()(RSA *pointer) const
        {
            RSA_free(pointer);
        }
    };

    std::unique_ptr<RSA, RsaDeleter> rsa(RSA_new());
    if (!rsa)
        return nullptr;

    BnClearPtr exponent(BN_new());
    if (!exponent || BN_set_word(exponent.get(), static_cast<BN_ULONG>(exp)) != 1)
        return nullptr;

    if (RSA_generate_key_ex(rsa.get(), bits, exponent.get(), nullptr) != 1)
        return nullptr;

    EVP_PKEY *result = EVP_PKEY_new();
    if (!result)
        return nullptr;

    if (EVP_PKEY_assign_RSA(result, rsa.get()) != 1) {
        EVP_PKEY_free(result);
        return nullptr;
    }

    rsa.release();
    return result;
#endif
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static EVP_PKEY *rsaFromParameters(const BigInteger &n,
                                   const BigInteger &e,
                                   const BigInteger *p,
                                   const BigInteger *q,
                                   const BigInteger *d)
{
    const bool privateKey = p && q && d;

    BnClearPtr bnn(bi2bn(n));
    BnClearPtr bne(bi2bn(e));
    BnClearPtr bnp(privateKey ? bi2bn(*p) : nullptr);
    BnClearPtr bnq(privateKey ? bi2bn(*q) : nullptr);
    BnClearPtr bnd(privateKey ? bi2bn(*d) : nullptr);

    if (!bnn || !bne || (privateKey && (!bnp || !bnq || !bnd)))
        return nullptr;

    if (privateKey) {
        return pkeyFromBnParameters("RSA",
                                    EVP_PKEY_KEYPAIR,
                                    {{OSSL_PKEY_PARAM_RSA_N, bnn.get()},
                                     {OSSL_PKEY_PARAM_RSA_E, bne.get()},
                                     {OSSL_PKEY_PARAM_RSA_D, bnd.get()},
                                     {OSSL_PKEY_PARAM_RSA_FACTOR1, bnp.get()},
                                     {OSSL_PKEY_PARAM_RSA_FACTOR2, bnq.get()}});
    }

    return pkeyFromBnParameters(
        "RSA", EVP_PKEY_PUBLIC_KEY, {{OSSL_PKEY_PARAM_RSA_N, bnn.get()}, {OSSL_PKEY_PARAM_RSA_E, bne.get()}});
}
#else
static EVP_PKEY *rsaFromParameters(const BigInteger &n,
                                   const BigInteger &e,
                                   const BigInteger *p,
                                   const BigInteger *q,
                                   const BigInteger *d)
{
    const bool privateKey = p && q && d;

    struct RsaDeleter
    {
        void operator()(RSA *pointer) const
        {
            RSA_free(pointer);
        }
    };

    std::unique_ptr<RSA, RsaDeleter> rsa(RSA_new());
    if (!rsa)
        return nullptr;

    BnClearPtr bnn(bi2bn(n));
    BnClearPtr bne(bi2bn(e));
    BnClearPtr bnd(privateKey ? bi2bn(*d) : nullptr);

    if (!bnn || !bne || (privateKey && !bnd))
        return nullptr;

    if (RSA_set0_key(rsa.get(), bnn.get(), bne.get(), privateKey ? bnd.get() : nullptr) != 1)
        return nullptr;

    bnn.release();
    bne.release();
    if (privateKey)
        bnd.release();

    if (privateKey) {
        BnClearPtr bnp(bi2bn(*p));
        BnClearPtr bnq(bi2bn(*q));
        if (!bnp || !bnq || RSA_set0_factors(rsa.get(), bnp.get(), bnq.get()) != 1)
            return nullptr;
        bnp.release();
        bnq.release();

        // Legacy OpenSSL can operate with incomplete private components only
        // when RSA blinding is disabled.
        if (e == BigInteger(0) || *d == BigInteger(0))
            RSA_blinding_off(rsa.get());
    }

    EVP_PKEY *result = EVP_PKEY_new();
    if (!result)
        return nullptr;

    if (EVP_PKEY_assign_RSA(result, rsa.get()) != 1) {
        EVP_PKEY_free(result);
        return nullptr;
    }

    rsa.release();
    return result;
}
#endif

static EVP_PKEY *rsaPublicKey(const BigInteger &n, const BigInteger &e)
{
    return rsaFromParameters(n, e, nullptr, nullptr, nullptr);
}

static EVP_PKEY *
rsaPrivateKey(const BigInteger &n, const BigInteger &e, const BigInteger &p, const BigInteger &q, const BigInteger &d)
{
    return rsaFromParameters(n, e, &p, &q, &d);
}

enum class RsaOperation
{
    PublicEncrypt,
    PrivateDecrypt,
    PrivateEncrypt,
    PublicDecrypt
};

static int initializeRsaOperation(EVP_PKEY_CTX *context, RsaOperation operation)
{
    switch (operation) {
    case RsaOperation::PublicEncrypt:
        return EVP_PKEY_encrypt_init(context);
    case RsaOperation::PrivateDecrypt:
        return EVP_PKEY_decrypt_init(context);
    case RsaOperation::PrivateEncrypt:
        return EVP_PKEY_sign_init(context);
    case RsaOperation::PublicDecrypt:
        return EVP_PKEY_verify_recover_init(context);
    }
    return 0;
}

static int executeRsaOperation(EVP_PKEY_CTX        *context,
                               RsaOperation         operation,
                               unsigned char       *output,
                               size_t              *outputSize,
                               const unsigned char *input,
                               size_t               inputSize)
{
    switch (operation) {
    case RsaOperation::PublicEncrypt:
        return EVP_PKEY_encrypt(context, output, outputSize, input, inputSize);
    case RsaOperation::PrivateDecrypt:
        return EVP_PKEY_decrypt(context, output, outputSize, input, inputSize);
    case RsaOperation::PrivateEncrypt:
        return EVP_PKEY_sign(context, output, outputSize, input, inputSize);
    case RsaOperation::PublicDecrypt:
        return EVP_PKEY_verify_recover(context, output, outputSize, input, inputSize);
    }
    return 0;
}

static bool
rsaOperation(EVP_PKEY *pkey, RsaOperation operation, const SecureArray &input, int padding, SecureArray *output)
{
    if (!pkey || !output)
        return false;

    PkeyCtxPtr context(newPkeyContext(pkey));
    if (!context || initializeRsaOperation(context.get(), operation) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(context.get(), padding) <= 0)
        return false;

    if (padding == RSA_PKCS1_OAEP_PADDING &&
        (operation == RsaOperation::PublicEncrypt || operation == RsaOperation::PrivateDecrypt)) {
        if (EVP_PKEY_CTX_set_rsa_oaep_md(context.get(), EVP_sha1()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(), EVP_sha1()) <= 0)
            return false;
    }

    const auto *inputData = reinterpret_cast<const unsigned char *>(input.data());
    const auto  inputSize = static_cast<size_t>(input.size());

    size_t outputSize = 0;
    if (executeRsaOperation(context.get(), operation, nullptr, &outputSize, inputData, inputSize) <= 0 ||
        outputSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    SecureArray result(static_cast<int>(outputSize));
    if (executeRsaOperation(context.get(),
                            operation,
                            reinterpret_cast<unsigned char *>(result.data()),
                            &outputSize,
                            inputData,
                            inputSize) <= 0 ||
        outputSize > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;

    result.resize(static_cast<int>(outputSize));
    *output = result;
    return true;
}
} // end of anonymous namespace

class RSAKeyMaker : public QThread
{
    Q_OBJECT
public:
    EVP_PKEY *result;
    int       bits, exp;

    RSAKeyMaker(int _bits, int _exp, QObject *parent = nullptr)
        : QThread(parent)
        , result(nullptr)
        , bits(_bits)
        , exp(_exp)
    {
    }

    ~RSAKeyMaker() override
    {
        wait();
        EVP_PKEY_free(result);
    }

    void run() override
    {
        result = generateRsaKey(bits, exp);
    }

    EVP_PKEY *takeResult()
    {
        EVP_PKEY *pkey = result;
        result         = nullptr;
        return pkey;
    }
};

RSAKey::RSAKey(Provider *p)
    : RSAContext(p)
{
    keymaker = nullptr;
    sec      = false;
}

RSAKey::RSAKey(const RSAKey &from)
    : RSAContext(from.provider())
    , evp(from.evp)
{
    keymaker = nullptr;
    sec      = from.sec;
}

RSAKey::~RSAKey()
{
    delete keymaker;
}

Provider::Context *RSAKey::clone() const
{
    return new RSAKey(*this);
}

bool RSAKey::isNull() const
{
    return (evp.pkey ? false : true);
}

PKey::Type RSAKey::type() const
{
    return PKey::RSA;
}

bool RSAKey::isPrivate() const
{
    return sec;
}

bool RSAKey::canExport() const
{
    return true;
}

void RSAKey::convertToPublic()
{
    if (!sec)
        return;

    EVP_PKEY *publicKey = rsaPublicKey(n(), e());
    if (!publicKey)
        return;

    evp.reset();
    evp.pkey = publicKey;
    sec      = false;
}

int RSAKey::bits() const
{
    return EVP_PKEY_bits(evp.pkey);
}

int RSAKey::maximumEncryptSize(EncryptionAlgorithm alg) const
{
    const int keySize = evp.pkey ? EVP_PKEY_size(evp.pkey) : 0;
    if (keySize <= 0)
        return 0;

    switch (alg) {
    case EME_PKCS1v15:
        return keySize - 11 - 1;
    case EME_PKCS1_OAEP:
        return keySize - 41 - 1;
    case EME_PKCS1v15_SSL:
        return keySize - 11 - 1;
    case EME_NO_PADDING:
        return keySize - 1;
    }

    return 0;
}

SecureArray RSAKey::encrypt(const SecureArray &in, EncryptionAlgorithm alg)
{
    SecureArray buf = in;
    const int   max = maximumEncryptSize(alg);

    if (max < 0)
        return {};
    if (buf.size() > max)
        buf.resize(max);

    int padding = 0;
    switch (alg) {
    case EME_PKCS1v15:
        padding = RSA_PKCS1_PADDING;
        break;
    case EME_PKCS1_OAEP:
        padding = RSA_PKCS1_OAEP_PADDING;
        break;
#ifdef RSA_SSLV23_PADDING
    case EME_PKCS1v15_SSL:
        padding = RSA_SSLV23_PADDING;
        break;
#endif
    case EME_NO_PADDING:
        padding = RSA_NO_PADDING;
        break;
    default:
        return {};
    }

    SecureArray        result;
    const RsaOperation operation = isPrivate() ? RsaOperation::PrivateEncrypt : RsaOperation::PublicEncrypt;
    if (!rsaOperation(evp.pkey, operation, buf, padding, &result))
        return {};

    return result;
}

bool RSAKey::decrypt(const SecureArray &in, SecureArray *out, EncryptionAlgorithm alg)
{
    int padding = 0;
    switch (alg) {
    case EME_PKCS1v15:
        padding = RSA_PKCS1_PADDING;
        break;
    case EME_PKCS1_OAEP:
        padding = RSA_PKCS1_OAEP_PADDING;
        break;
#ifdef RSA_SSLV23_PADDING
    case EME_PKCS1v15_SSL:
        padding = RSA_SSLV23_PADDING;
        break;
#endif
    case EME_NO_PADDING:
        padding = RSA_NO_PADDING;
        break;
    default:
        return false;
    }

    const RsaOperation operation = isPrivate() ? RsaOperation::PrivateDecrypt : RsaOperation::PublicDecrypt;
    return rsaOperation(evp.pkey, operation, in, padding, out);
}

void RSAKey::startSign(SignatureAlgorithm alg, SignatureFormat)
{
    if (alg.scheme != SignatureScheme::RSA_PKCS1v15) {
        evp.startSignError();
        return;
    }

    if (alg.digest == SignatureDigest::None) {
        evp.startSign(nullptr);
        return;
    }

    const EVP_MD *md = signatureDigestToEvp(alg.digest, s_legacyProviderAvailable);
    if (!md) {
        evp.startSignError();
        return;
    }
    evp.startSign(md);
}

void RSAKey::startVerify(SignatureAlgorithm alg, SignatureFormat)
{
    if (alg.scheme != SignatureScheme::RSA_PKCS1v15) {
        evp.startVerifyError();
        return;
    }

    if (alg.digest == SignatureDigest::None) {
        evp.startVerify(nullptr);
        return;
    }

    const EVP_MD *md = signatureDigestToEvp(alg.digest, s_legacyProviderAvailable);
    if (!md) {
        evp.startVerifyError();
        return;
    }
    evp.startVerify(md);
}

void RSAKey::update(const MemoryRegion &in)
{
    evp.update(in);
}

QByteArray RSAKey::endSign()
{
    return evp.endSign().toByteArray();
}

bool RSAKey::endVerify(const QByteArray &sig)
{
    return evp.endVerify(sig);
}

void RSAKey::createPrivate(int bits, int exp, bool block)
{
    evp.reset();
    sec = false;

    keymaker    = new RSAKeyMaker(bits, exp, !block ? this : nullptr);
    wasBlocking = block;
    if (block) {
        keymaker->run();
        km_finished();
    } else {
        connect(keymaker, &RSAKeyMaker::finished, this, &RSAKey::km_finished);
        keymaker->start();
    }
}

void RSAKey::createPrivate(const BigInteger &n,
                           const BigInteger &e,
                           const BigInteger &p,
                           const BigInteger &q,
                           const BigInteger &d)
{
    evp.reset();
    sec = false;

    evp.pkey = rsaPrivateKey(n, e, p, q, d);
    if (evp.pkey)
        sec = true;
}

void RSAKey::createPublic(const BigInteger &n, const BigInteger &e)
{
    evp.reset();
    sec = false;

    evp.pkey = rsaPublicKey(n, e);
}

BigInteger RSAKey::n() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_RSA_N, false);
#else
    const RSA    *rsa = EVP_PKEY_get0_RSA(evp.pkey);
    const BIGNUM *bnn = nullptr;
    RSA_get0_key(rsa, &bnn, nullptr, nullptr);
    return bn2bi(bnn);
#endif
}

BigInteger RSAKey::e() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_RSA_E, false);
#else
    const RSA    *rsa = EVP_PKEY_get0_RSA(evp.pkey);
    const BIGNUM *bne = nullptr;
    RSA_get0_key(rsa, nullptr, &bne, nullptr);
    return bn2bi(bne);
#endif
}

BigInteger RSAKey::p() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_RSA_FACTOR1, true);
#else
    const RSA    *rsa = EVP_PKEY_get0_RSA(evp.pkey);
    const BIGNUM *bnp = nullptr;
    RSA_get0_factors(rsa, &bnp, nullptr);
    return bn2bi(bnp);
#endif
}

BigInteger RSAKey::q() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_RSA_FACTOR2, true);
#else
    const RSA    *rsa = EVP_PKEY_get0_RSA(evp.pkey);
    const BIGNUM *bnq = nullptr;
    RSA_get0_factors(rsa, nullptr, &bnq);
    return bn2bi(bnq);
#endif
}

BigInteger RSAKey::d() const
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return pkeyBnParameter(evp.pkey, OSSL_PKEY_PARAM_RSA_D, true);
#else
    const RSA    *rsa = EVP_PKEY_get0_RSA(evp.pkey);
    const BIGNUM *bnd = nullptr;
    RSA_get0_key(rsa, nullptr, nullptr, &bnd);
    return bn2bi(bnd);
#endif
}

void RSAKey::km_finished()
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

}

#include "rsakey.moc"
