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

#include "certcontext.h"
#include "dhkey.h"
#include "dsakey.h"
#include "dtlscontext.h"
#include "ed25519key.h"
#include "pkeycontext.h"
#include "rsakey.h"
#include "tlscontext.h"
#include "utils.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QtCrypto>
#include <QtPlugin>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>

#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#ifdef OPENSSL_VERSION_MAJOR
#include <openssl/provider.h>
#endif
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif

#ifndef LIBRESSL_VERSION_NUMBER
#include <openssl/kdf.h>
#endif

#ifndef RSA_F_RSA_OSSL_PRIVATE_DECRYPT
#define RSA_F_RSA_OSSL_PRIVATE_DECRYPT RSA_F_RSA_EAY_PRIVATE_DECRYPT
#endif

using namespace QCA;

namespace opensslQCAPlugin {

bool s_legacyProviderAvailable = false;

//----------------------------------------------------------------------------
// Util
//----------------------------------------------------------------------------

/*static bool is_basic_constraint(const ConstraintType &t)
{
    bool basic = false;
    switch(t.known())
    {
        case DigitalSignature:
        case NonRepudiation:
        case KeyEncipherment:
        case DataEncipherment:
        case KeyAgreement:
        case KeyCertificateSign:
        case CRLSign:
        case EncipherOnly:
        case DecipherOnly:
            basic = true;
            break;

        case ServerAuth:
        case ClientAuth:
        case CodeSigning:
        case EmailProtection:
        case IPSecEndSystem:
        case IPSecTunnel:
        case IPSecUser:
        case TimeStamping:
        case OCSPSigning:
            break;
    }
    return basic;
}

static Constraints basic_only(const Constraints &list)
{
    Constraints out;
    for(int n = 0; n < list.count(); ++n)
    {
        if(is_basic_constraint(list[n]))
            out += list[n];
    }
    return out;
}

static Constraints ext_only(const Constraints &list)
{
    Constraints out;
    for(int n = 0; n < list.count(); ++n)
    {
        if(!is_basic_constraint(list[n]))
            out += list[n];
    }
    return out;
}*/

// logic from Botan
/*static Constraints find_constraints(const PKeyContext &key, const Constraints &orig)
{
    Constraints constraints;

    if(key.key()->type() == PKey::RSA)
        constraints += KeyEncipherment;

    if(key.key()->type() == PKey::DH)
        constraints += KeyAgreement;

    if(key.key()->type() == PKey::RSA || key.key()->type() == PKey::DSA)
    {
        constraints += DigitalSignature;
        constraints += NonRepudiation;
    }

    Constraints limits = basic_only(orig);
    Constraints the_rest = ext_only(orig);

    if(!limits.isEmpty())
    {
        Constraints reduced;
        for(int n = 0; n < constraints.count(); ++n)
        {
            if(limits.contains(constraints[n]))
                reduced += constraints[n];
        }
        constraints = reduced;
    }

    constraints += the_rest;

    return constraints;
}*/

class opensslHashContext : public HashContext
{
    Q_OBJECT
public:
    opensslHashContext(const EVP_MD *algorithm, Provider *p, const QString &type)
        : HashContext(p, type)
    {
        m_algorithm = algorithm;
        m_context   = EVP_MD_CTX_new();
        EVP_DigestInit(m_context, m_algorithm);
    }

    opensslHashContext(const opensslHashContext &other)
        : HashContext(other)
    {
        m_algorithm = other.m_algorithm;
        m_context   = EVP_MD_CTX_new();
        EVP_MD_CTX_copy_ex(m_context, other.m_context);
    }

    ~opensslHashContext() override
    {
        EVP_MD_CTX_free(m_context);
    }

    void clear() override
    {
        EVP_MD_CTX_free(m_context);
        m_context = EVP_MD_CTX_new();
        EVP_DigestInit(m_context, m_algorithm);
    }

    void update(const MemoryRegion &a) override
    {
        EVP_DigestUpdate(m_context, (unsigned char *)a.data(), a.size());
    }

    MemoryRegion final() override
    {
        SecureArray a(EVP_MD_size(m_algorithm));
        EVP_DigestFinal(m_context, (unsigned char *)a.data(), nullptr);
        return a;
    }

    Provider::Context *clone() const override
    {
        return new opensslHashContext(*this);
    }

protected:
    const EVP_MD *m_algorithm;
    EVP_MD_CTX   *m_context;
};

class opensslPbkdf1Context : public KDFContext
{
    Q_OBJECT
public:
    opensslPbkdf1Context(const EVP_MD *algorithm, Provider *p, const QString &type)
        : KDFContext(p, type)
    {
        Q_ASSERT(s_legacyProviderAvailable);
        m_algorithm = algorithm;
        m_context   = EVP_MD_CTX_new();
        EVP_DigestInit(m_context, m_algorithm);
    }

    opensslPbkdf1Context(const opensslPbkdf1Context &other)
        : KDFContext(other)
    {
        m_algorithm = other.m_algorithm;
        m_context   = EVP_MD_CTX_new();
        EVP_MD_CTX_copy(m_context, other.m_context);
    }

    ~opensslPbkdf1Context() override
    {
        EVP_MD_CTX_free(m_context);
    }

    Provider::Context *clone() const override
    {
        return new opensslPbkdf1Context(*this);
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         unsigned int                keyLength,
                         unsigned int                iterationCount) override
    {
        /* from RFC2898:
           Steps:

           1. If dkLen > 16 for MD2 and MD5, or dkLen > 20 for SHA-1, output
           "derived key too long" and stop.
        */
        if (keyLength > (unsigned int)EVP_MD_size(m_algorithm)) {
            std::cout << "derived key too long" << std::endl;
            return SymmetricKey();
        }

        /*
          2. Apply the underlying hash function Hash for c iterations to the
          concatenation of the password P and the salt S, then extract
          the first dkLen octets to produce a derived key DK:

          T_1 = Hash (P || S) ,
          T_2 = Hash (T_1) ,
          ...
          T_c = Hash (T_{c-1}) ,
          DK = Tc<0..dkLen-1>
        */
        // calculate T_1
        EVP_DigestUpdate(m_context, (unsigned char *)secret.data(), secret.size());
        EVP_DigestUpdate(m_context, (unsigned char *)salt.data(), salt.size());
        SecureArray a(EVP_MD_size(m_algorithm));
        EVP_DigestFinal(m_context, (unsigned char *)a.data(), nullptr);

        // calculate T_2 up to T_c
        for (unsigned int i = 2; i <= iterationCount; ++i) {
            EVP_DigestInit(m_context, m_algorithm);
            EVP_DigestUpdate(m_context, (unsigned char *)a.data(), a.size());
            EVP_DigestFinal(m_context, (unsigned char *)a.data(), nullptr);
        }

        // shrink a to become DK, of the required length
        a.resize(keyLength);

        /*
          3. Output the derived key DK.
        */
        return a;
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         unsigned int                keyLength,
                         int                         msecInterval,
                         unsigned int               *iterationCount) override
    {
        Q_ASSERT(iterationCount != nullptr);
        QElapsedTimer timer;

        /* from RFC2898:
           Steps:

           1. If dkLen > 16 for MD2 and MD5, or dkLen > 20 for SHA-1, output
           "derived key too long" and stop.
        */
        if (keyLength > (unsigned int)EVP_MD_size(m_algorithm)) {
            std::cout << "derived key too long" << std::endl;
            return SymmetricKey();
        }

        /*
          2. Apply the underlying hash function Hash for M milliseconds
          to the concatenation of the password P and the salt S, incrementing c,
          then extract the first dkLen octets to produce a derived key DK:

          time from M to 0
          T_1 = Hash (P || S) ,
          T_2 = Hash (T_1) ,
          ...
          T_c = Hash (T_{c-1}) ,
          when time = 0: stop,
          DK = Tc<0..dkLen-1>
        */
        // calculate T_1
        EVP_DigestUpdate(m_context, (unsigned char *)secret.data(), secret.size());
        EVP_DigestUpdate(m_context, (unsigned char *)salt.data(), salt.size());
        SecureArray a(EVP_MD_size(m_algorithm));
        EVP_DigestFinal(m_context, (unsigned char *)a.data(), nullptr);

        // calculate T_2 up to T_c
        *iterationCount = 2 - 1; // <- Have to remove 1, unless it computes one
        timer.start();           // ^  time more than the base function
                                 // ^  with the same iterationCount
        while (timer.elapsed() < msecInterval) {
            EVP_DigestInit(m_context, m_algorithm);
            EVP_DigestUpdate(m_context, (unsigned char *)a.data(), a.size());
            EVP_DigestFinal(m_context, (unsigned char *)a.data(), nullptr);
            ++(*iterationCount);
        }

        // shrink a to become DK, of the required length
        a.resize(keyLength);

        /*
          3. Output the derived key DK.
        */
        return a;
    }

protected:
    const EVP_MD *m_algorithm;
    EVP_MD_CTX   *m_context;
};

class opensslPbkdf2Context : public KDFContext
{
    Q_OBJECT
public:
    opensslPbkdf2Context(const EVP_MD *algorithm, Provider *p, const QString &type)
        : KDFContext(p, type)
        , m_algorithm(algorithm)
    {
    }

    Provider::Context *clone() const override
    {
        return new opensslPbkdf2Context(*this);
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         unsigned int                keyLength,
                         unsigned int                iterationCount) override
    {
        if (!keyLength || keyLength > unsigned(std::numeric_limits<int>::max()) || !iterationCount ||
            iterationCount > unsigned(std::numeric_limits<int>::max()))
            return SymmetricKey();
        SecureArray out(static_cast<int>(keyLength));
        if (PKCS5_PBKDF2_HMAC(secret.constData(),
                              secret.size(),
                              reinterpret_cast<const unsigned char *>(salt.constData()),
                              salt.size(),
                              int(iterationCount),
                              m_algorithm,
                              int(keyLength),
                              reinterpret_cast<unsigned char *>(out.data())) != 1)
            return SymmetricKey();
        return out;
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         unsigned int                keyLength,
                         int                         msecInterval,
                         unsigned int               *iterationCount) override
    {
        if (!iterationCount)
            return SymmetricKey();
        *iterationCount = 0;
        if (!keyLength || keyLength > unsigned(std::numeric_limits<int>::max()))
            return SymmetricKey();
        // Measure actual PBKDF2 work and reuse the final sample, rather than
        // counting one-iteration calls dominated by allocation and setup.
        QElapsedTimer timer;
        timer.start();
        unsigned       count   = 1;
        const unsigned maximum = unsigned(std::numeric_limits<int>::max());
        do {
            auto sample = makeKey(secret, salt, keyLength, count);
            if (sample.size() != int(keyLength))
                return SymmetricKey();
            if (timer.elapsed() >= msecInterval || count == maximum) {
                *iterationCount = count;
                return sample;
            }
            count = count > maximum / 2 ? maximum : count * 2;
        } while (true);
    }

private:
    const EVP_MD *m_algorithm;
};

#ifndef LIBRESSL_VERSION_NUMBER
class opensslHkdfContext : public HKDFContext
{
    Q_OBJECT
public:
    opensslHkdfContext(Provider *p, const QString &type)
        : HKDFContext(p, type)
    {
    }

    Provider::Context *clone() const override
    {
        return new opensslHkdfContext(*this);
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         const InitializationVector &info,
                         unsigned int                keyLength) override
    {
        SecureArray out(keyLength);
#ifdef EVP_PKEY_HKDF
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
        EVP_PKEY_derive_init(pctx);
        EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256());
        EVP_PKEY_CTX_set1_hkdf_salt(pctx, (const unsigned char *)salt.data(), int(salt.size()));
        EVP_PKEY_CTX_set1_hkdf_key(pctx, (const unsigned char *)secret.data(), int(secret.size()));
        EVP_PKEY_CTX_add1_hkdf_info(pctx, (const unsigned char *)info.data(), int(info.size()));
        size_t outlen = out.size();
        EVP_PKEY_derive(pctx, reinterpret_cast<unsigned char *>(out.data()), &outlen);
        EVP_PKEY_CTX_free(pctx);
#else
        unsigned char  prk[EVP_MAX_MD_SIZE];
        unsigned char *ret;
        unsigned int   prk_len;
        HMAC(EVP_sha256(),
             salt.data(),
             salt.size(),
             reinterpret_cast<const unsigned char *>(secret.data()),
             secret.size(),
             prk,
             &prk_len);
        HMAC_CTX      hmac;
        unsigned char prev[EVP_MAX_MD_SIZE];
        size_t        done_len = 0;
        size_t        dig_len  = EVP_MD_size(EVP_sha256());
        size_t        n        = out.size() / dig_len;
        if (out.size() % dig_len)
            ++n;
        HMAC_CTX_init(&hmac);
        HMAC_Init_ex(&hmac, prk, prk_len, EVP_sha256(), nullptr);
        for (unsigned int i = 1; i <= n; ++i) {
            const unsigned char ctr = i;
            if (i > 1) {
                HMAC_Init_ex(&hmac, nullptr, 0, nullptr, nullptr);
                HMAC_Update(&hmac, prev, dig_len);
            }
            HMAC_Update(&hmac, reinterpret_cast<const unsigned char *>(info.data()), info.size());
            HMAC_Update(&hmac, &ctr, 1);
            HMAC_Final(&hmac, prev, nullptr);
            size_t copy_len = (done_len + dig_len > out.size()) ? out.size() - done_len : dig_len;
            memcpy(reinterpret_cast<unsigned char *>(out.data()) + done_len, prev, copy_len);
            done_len += copy_len;
        }
        HMAC_CTX_cleanup(&hmac);
        OPENSSL_cleanse(prk, sizeof prk);
#endif
        return out;
    }
};
#endif // LIBRESSL_VERSION_NUMBER

class opensslHMACContext : public MACContext
{
    Q_OBJECT

public:
    opensslHMACContext(const EVP_MD *algorithm, Provider *p, const QString &type)
        : MACContext(p, type)
        , m_algorithm(algorithm)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        , m_mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr))
        , m_context(m_mac ? EVP_MAC_CTX_new(m_mac) : nullptr)
#else
        , m_context(HMAC_CTX_new())
#endif
    {
    }

    opensslHMACContext(const opensslHMACContext &other)
        : MACContext(other)
        , m_algorithm(other.m_algorithm)
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        , m_mac(other.m_mac)
        , m_context(nullptr)
#else
        , m_context(HMAC_CTX_new())
#endif
        , m_initialized(other.m_initialized)
    {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        if (m_mac && EVP_MAC_up_ref(m_mac) != 1)
            m_mac = nullptr;

        if (m_mac && other.m_context)
            m_context = EVP_MAC_CTX_dup(other.m_context);

        if (other.m_context && !m_context)
            m_initialized = false;
#else
        if (!m_context || !other.m_context || HMAC_CTX_copy(m_context, other.m_context) != 1) {
            HMAC_CTX_free(m_context);
            m_context     = nullptr;
            m_initialized = false;
        }
#endif
    }

    ~opensslHMACContext() override
    {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        EVP_MAC_CTX_free(m_context);
        EVP_MAC_free(m_mac);
#else
        HMAC_CTX_free(m_context);
#endif
    }

    void setup(const SymmetricKey &key) override
    {
        m_initialized = false;

        if (!m_context || !m_algorithm)
            return;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        const char *digestName = EVP_MD_get0_name(m_algorithm);
        if (!digestName) {
            resetContext();
            return;
        }

        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, const_cast<char *>(digestName), 0),
            OSSL_PARAM_construct_end()};

        // EVP_MAC_init() treats nullptr specially. Use a non-null pointer
        // so that a zero-length HMAC key remains a real empty key.
        static const unsigned char emptyKey = 0;

        const auto *keyData = reinterpret_cast<const unsigned char *>(key.data());

        if (!keyData)
            keyData = &emptyKey;

        if (EVP_MAC_init(m_context, keyData, static_cast<size_t>(key.size()), params) != 1) {
            resetContext();
            return;
        }
#else
        if (HMAC_Init_ex(m_context, key.data(), static_cast<int>(key.size()), m_algorithm, nullptr) != 1) {
            HMAC_CTX_reset(m_context);
            return;
        }
#endif

        m_initialized = true;
    }

    KeyLength keyLength() const override
    {
        return anyKeyLength();
    }

    void update(const MemoryRegion &a) override
    {
        if (!m_initialized || !m_context || a.size() == 0)
            return;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        if (EVP_MAC_update(
                m_context, reinterpret_cast<const unsigned char *>(a.data()), static_cast<size_t>(a.size())) != 1) {
            m_initialized = false;
            resetContext();
        }
#else
        if (HMAC_Update(m_context, reinterpret_cast<const unsigned char *>(a.data()), static_cast<size_t>(a.size())) !=
            1) {
            m_initialized = false;
            HMAC_CTX_reset(m_context);
        }
#endif
    }

    void final(MemoryRegion *out) override
    {
        SecureArray result;

        if (!m_initialized || !m_context) {
            *out = result;
            return;
        }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        const size_t maximumSize = EVP_MAC_CTX_get_mac_size(m_context);

        if (maximumSize == 0 || maximumSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            resetContext();
            m_initialized = false;
            *out          = result;
            return;
        }

        result.resize(static_cast<int>(maximumSize));

        size_t actualSize = 0;

        if (EVP_MAC_final(m_context, reinterpret_cast<unsigned char *>(result.data()), &actualSize, maximumSize) == 1) {
            result.resize(static_cast<int>(actualSize));
        } else {
            result = SecureArray();
        }

        // EVP_MAC_CTX has no equivalent of HMAC_CTX_reset().
        // Recreate it so the next setup() starts with a clean context.
        resetContext();
#else
        const int maximumSize = EVP_MD_size(m_algorithm);

        if (maximumSize > 0) {
            result.resize(maximumSize);

            unsigned int actualSize = 0;

            if (HMAC_Final(m_context, reinterpret_cast<unsigned char *>(result.data()), &actualSize) == 1) {
                result.resize(static_cast<int>(actualSize));
            } else {
                result = SecureArray();
            }
        }

        HMAC_CTX_reset(m_context);
#endif

        m_initialized = false;
        *out          = result;
    }

    Provider::Context *clone() const override
    {
        return new opensslHMACContext(*this);
    }

private:
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    void resetContext()
    {
        EVP_MAC_CTX *newContext = m_mac ? EVP_MAC_CTX_new(m_mac) : nullptr;

        EVP_MAC_CTX_free(m_context);
        m_context = newContext;
    }
#endif

    const EVP_MD *m_algorithm = nullptr;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_MAC     *m_mac     = nullptr;
    EVP_MAC_CTX *m_context = nullptr;
#else
    HMAC_CTX *m_context = nullptr;
#endif

    bool m_initialized = false;
};

//----------------------------------------------------------------------------
// MyDLGroup
//----------------------------------------------------------------------------
// clang-format off
// IETF primes from Botan
static const char *IETF_1024_PRIME =
    "FFFFFFFF FFFFFFFF C90FDAA2 2168C234 C4C6628B 80DC1CD1"
    "29024E08 8A67CC74 020BBEA6 3B139B22 514A0879 8E3404DD"
    "EF9519B3 CD3A431B 302B0A6D F25F1437 4FE1356D 6D51C245"
    "E485B576 625E7EC6 F44C42E9 A637ED6B 0BFF5CB6 F406B7ED"
    "EE386BFB 5A899FA5 AE9F2411 7C4B1FE6 49286651 ECE65381"
    "FFFFFFFF FFFFFFFF";

static const char *IETF_2048_PRIME =
    "FFFFFFFF FFFFFFFF C90FDAA2 2168C234 C4C6628B 80DC1CD1"
    "29024E08 8A67CC74 020BBEA6 3B139B22 514A0879 8E3404DD"
    "EF9519B3 CD3A431B 302B0A6D F25F1437 4FE1356D 6D51C245"
    "E485B576 625E7EC6 F44C42E9 A637ED6B 0BFF5CB6 F406B7ED"
    "EE386BFB 5A899FA5 AE9F2411 7C4B1FE6 49286651 ECE45B3D"
    "C2007CB8 A163BF05 98DA4836 1C55D39A 69163FA8 FD24CF5F"
    "83655D23 DCA3AD96 1C62F356 208552BB 9ED52907 7096966D"
    "670C354E 4ABC9804 F1746C08 CA18217C 32905E46 2E36CE3B"
    "E39E772C 180E8603 9B2783A2 EC07A28F B5C55DF0 6F4C52C9"
    "DE2BCBF6 95581718 3995497C EA956AE5 15D22618 98FA0510"
    "15728E5A 8AACAA68 FFFFFFFF FFFFFFFF";

static const char *IETF_4096_PRIME =
    "FFFFFFFF FFFFFFFF C90FDAA2 2168C234 C4C6628B 80DC1CD1"
    "29024E08 8A67CC74 020BBEA6 3B139B22 514A0879 8E3404DD"
    "EF9519B3 CD3A431B 302B0A6D F25F1437 4FE1356D 6D51C245"
    "E485B576 625E7EC6 F44C42E9 A637ED6B 0BFF5CB6 F406B7ED"
    "EE386BFB 5A899FA5 AE9F2411 7C4B1FE6 49286651 ECE45B3D"
    "C2007CB8 A163BF05 98DA4836 1C55D39A 69163FA8 FD24CF5F"
    "83655D23 DCA3AD96 1C62F356 208552BB 9ED52907 7096966D"
    "670C354E 4ABC9804 F1746C08 CA18217C 32905E46 2E36CE3B"
    "E39E772C 180E8603 9B2783A2 EC07A28F B5C55DF0 6F4C52C9"
    "DE2BCBF6 95581718 3995497C EA956AE5 15D22618 98FA0510"
    "15728E5A 8AAAC42D AD33170D 04507A33 A85521AB DF1CBA64"
    "ECFB8504 58DBEF0A 8AEA7157 5D060C7D B3970F85 A6E1E4C7"
    "ABF5AE8C DB0933D7 1E8C94E0 4A25619D CEE3D226 1AD2EE6B"
    "F12FFA06 D98A0864 D8760273 3EC86A64 521F2B18 177B200C"
    "BBE11757 7A615D6C 770988C0 BAD946E2 08E24FA0 74E5AB31"
    "43DB5BFC E0FD108E 4B82D120 A9210801 1A723C12 A787E6D7"
    "88719A10 BDBA5B26 99C32718 6AF4E23C 1A946834 B6150BDA"
    "2583E9CA 2AD44CE8 DBBBC2DB 04DE8EF9 2E8EFC14 1FBECAA6"
    "287C5947 4E6BC05D 99B2964F A090C3A2 233BA186 515BE7ED"
    "1F612970 CEE2D7AF B81BDD76 2170481C D0069127 D5B05AA9"
    "93B4EA98 8D8FDDC1 86FFB7DC 90A6C08F 4DF435C9 34063199"
    "FFFFFFFF FFFFFFFF";
// clang-format on

#ifndef OPENSSL_FIPS
// JCE DSA-512 parameters derived using FIPS 186-2.
// Seed: B869C82B35D70E1B1FF91B28E37A62ECDC34409B
// Counter: 123
static const char *JCE_512_P =
    "FCA682CE 8E12CABA 26EFCCF7 110E526D"
    "B078B05E DECBCD1E B4A208F3 AE1617AE"
    "01F35B91 A47E6DF6 3413C5E1 2ED0899B"
    "CD132ACD 50D99151 BDC43EE7 37592E17";

static const char *JCE_512_Q = "962EDDCC 369CBA8E BB260EE6 B6A126D9 346E38C5";

static const char *JCE_512_G =
    "678471B2 7A9CF44E E91A49C5 147DB1A9"
    "AAF244F0 5A434D64 86931D2D 14271B9E"
    "35030B71 FD73DA17 9069B32E 2935630E"
    "1C206235 4D0DA20A 6C416E50 BE794CA4";

// JCE DSA-768 parameters derived using FIPS 186-2.
// Seed: 77D0F8C4DAD15EB8C4F2F8D6726CEFD96D5BB399
// Counter: 263
static const char *JCE_768_P =
    "E9E64259 9D355F37 C97FFD35 67120B8E"
    "25C9CD43 E927B3A9 670FBEC5 D8901419"
    "22D2C3B3 AD248009 3799869D 1E846AAB"
    "49FAB0AD 26D2CE6A 22219D47 0BCE7D77"
    "7D4A21FB E9C270B5 7F607002 F3CEF839"
    "3694CF45 EE3688C1 1A8C56AB 127A3DAF";

static const char *JCE_768_Q = "9CDBD84C 9F1AC2F3 8D0F80F4 2AB952E7 338BF511";

static const char *JCE_768_G =
    "30470AD5 A005FB14 CE2D9DCD 87E38BC7"
    "D1B1C5FA CBAECBE9 5F190AA7 A31D23C4"
    "DBBCBE06 17454440 1A5B2C02 0965D8C2"
    "BD2171D3 66844577 1F74BA08 4D2029D8"
    "3C1C1585 47F3A9F1 A2715BE2 3D51AE4D"
    "3E5A1F6A 7064F316 933A346D 3F529252";

// JCE DSA-1024 parameters derived using FIPS 186-2.
// Seed: 8D5155894229D5E689EE01E6018A237E2CAE64CD
// Counter: 92
static const char *JCE_1024_P =
    "FD7F5381 1D751229 52DF4A9C 2EECE4E7"
    "F611B752 3CEF4400 C31E3F80 B6512669"
    "455D4022 51FB593D 8D58FABF C5F5BA30"
    "F6CB9B55 6CD7813B 801D346F F26660B7"
    "6B9950A5 A49F9FE8 047B1022 C24FBBA9"
    "D7FEB7C6 1BF83B57 E7C6A8A6 150F04FB"
    "83F6D3C5 1EC30235 54135A16 9132F675"
    "F3AE2B61 D72AEFF2 2203199D D14801C7";

static const char *JCE_1024_Q = "9760508F 15230BCC B292B982 A2EB840B F0581CF5";

static const char *JCE_1024_G =
    "F7E1A085 D69B3DDE CBBCAB5C 36B857B9"
    "7994AFBB FA3AEA82 F9574C0B 3D078267"
    "5159578E BAD4594F E6710710 8180B449"
    "167123E8 4C281613 B7CF0932 8CC8A6E1"
    "3C167A8B 547C8D28 E0A3AE1E 2BB3A675"
    "916EA37F 0BFA2135 62F1FB62 7A01243B"
    "CCA4F1BE A8519089 A883DFE1 5AE59F06"
    "928B665E 807B5525 64014C3B FECF492A";
#endif

static QByteArray dehex(const QByteArray &hex)
{
    QString str;
    for (const char c : hex) {
        if (c != ' ')
            str += QLatin1Char(c);
    }
    return hexToArray(str);
}

static BigInteger decode(const QByteArray &prime)
{
    QByteArray a(1, 0); // 1 byte of zero padding
    a.append(dehex(prime));
    return BigInteger(SecureArray(a));
}

class DLParams
{
public:
    BigInteger p, q, g;
};

#ifndef OPENSSL_FIPS
static bool get_dsa_dlgroup(const QByteArray &p, const QByteArray &q, const QByteArray &g, DLParams *params)
{
    if (!params)
        return false;

    params->p = decode(p);
    params->q = decode(q);
    params->g = decode(g);

    return true;
}
#endif

static bool get_dlgroup(const BigInteger &p, const BigInteger &g, DLParams *params)
{
    params->p = p;
    params->q = BigInteger(0);
    params->g = g;
    return true;
}

class DLGroupMaker : public QThread
{
    Q_OBJECT
public:
    DLGroupSet set;
    bool       ok;
    DLParams   params;

    DLGroupMaker(DLGroupSet _set)
    {
        set = _set;
    }

    ~DLGroupMaker() override
    {
        wait();
    }

    void run() override
    {
        switch (set) {
#ifndef OPENSSL_FIPS
        case DSA_512:
            ok = get_dsa_dlgroup(JCE_512_P, JCE_512_Q, JCE_512_G, &params);
            break;

        case DSA_768:
            ok = get_dsa_dlgroup(JCE_768_P, JCE_768_Q, JCE_768_G, &params);
            break;

        case DSA_1024:
            ok = get_dsa_dlgroup(JCE_1024_P, JCE_1024_Q, JCE_1024_G, &params);
            break;
#endif

        case IETF_1024:
            ok = get_dlgroup(decode(IETF_1024_PRIME), 2, &params);
            break;

        case IETF_2048:
            ok = get_dlgroup(decode(IETF_2048_PRIME), 2, &params);
            break;

        case IETF_4096:
            ok = get_dlgroup(decode(IETF_4096_PRIME), 2, &params);
            break;

        default:
            ok = false;
            break;
        }
    }
};

class MyDLGroup : public DLGroupContext
{
    Q_OBJECT
public:
    DLGroupMaker *gm;
    bool          wasBlocking;
    DLParams      params;
    bool          empty;

    MyDLGroup(Provider *p)
        : DLGroupContext(p)
    {
        gm    = nullptr;
        empty = true;
    }

    MyDLGroup(const MyDLGroup &from)
        : DLGroupContext(from.provider())
    {
        gm    = nullptr;
        empty = true;
    }

    ~MyDLGroup() override
    {
        delete gm;
    }

    Provider::Context *clone() const override
    {
        return new MyDLGroup(*this);
    }

    QList<DLGroupSet> supportedGroupSets() const override
    {
        QList<DLGroupSet> list;

        // DSA_* was removed in FIPS specification
        // https://bugzilla.redhat.com/show_bug.cgi?id=1144655
#ifndef OPENSSL_FIPS
        list += DSA_512;
        list += DSA_768;
        list += DSA_1024;
#endif
        list += IETF_1024;
        list += IETF_2048;
        list += IETF_4096;
        return list;
    }

    bool isNull() const override
    {
        return empty;
    }

    void fetchGroup(DLGroupSet set, bool block) override
    {
        params = DLParams();
        empty  = true;

        gm          = new DLGroupMaker(set);
        wasBlocking = block;
        if (block) {
            gm->run();
            gm_finished();
        } else {
            connect(gm, &DLGroupMaker::finished, this, &MyDLGroup::gm_finished);
            gm->start();
        }
    }

    void getResult(BigInteger *p, BigInteger *q, BigInteger *g) const override
    {
        *p = params.p;
        *q = params.q;
        *g = params.g;
    }

private Q_SLOTS:
    void gm_finished()
    {
        bool ok = gm->ok;
        if (ok) {
            params = gm->params;
            empty  = false;
        }

        if (wasBlocking)
            delete gm;
        else
            gm->deleteLater();
        gm = nullptr;

        if (!wasBlocking)
            emit finished();
    }
};

//----------------------------------------------------------------------------
// MyCertCollectionContext
//----------------------------------------------------------------------------
class MyCertCollectionContext : public CertCollectionContext
{
    Q_OBJECT
public:
    MyCertCollectionContext(Provider *p)
        : CertCollectionContext(p)
    {
    }

    Provider::Context *clone() const override
    {
        return new MyCertCollectionContext(*this);
    }

    QByteArray toPKCS7(const QList<CertContext *> &certs, const QList<CRLContext *> &crls) const override
    {
        // TODO: implement
        Q_UNUSED(certs);
        Q_UNUSED(crls);
        return QByteArray();
    }

    ConvertResult fromPKCS7(const QByteArray &a, QList<CertContext *> *certs, QList<CRLContext *> *crls) const override
    {
        BIO *bi = BIO_new(BIO_s_mem());
        BIO_write(bi, a.data(), a.size());
        PKCS7 *p7 = d2i_PKCS7_bio(bi, nullptr);
        BIO_free(bi);
        if (!p7)
            return ErrorDecode;

        STACK_OF(X509) *xcerts    = nullptr;
        STACK_OF(X509_CRL) *xcrls = nullptr;

        int i = OBJ_obj2nid(p7->type);
        if (i == NID_pkcs7_signed) {
            xcerts = p7->d.sign->cert;
            xcrls  = p7->d.sign->crl;
        } else if (i == NID_pkcs7_signedAndEnveloped) {
            xcerts = p7->d.signed_and_enveloped->cert;
            xcrls  = p7->d.signed_and_enveloped->crl;
        }

        QList<CertContext *> _certs;
        QList<CRLContext *>  _crls;

        if (xcerts) {
            for (int n = 0; n < sk_X509_num(xcerts); ++n) {
                MyCertContext *cc = new MyCertContext(provider());
                cc->fromX509(sk_X509_value(xcerts, n));
                _certs += cc;
            }
        }
        if (xcrls) {
            for (int n = 0; n < sk_X509_CRL_num(xcrls); ++n) {
                MyCRLContext *cc = new MyCRLContext(provider());
                cc->fromX509(sk_X509_CRL_value(xcrls, n));
                _crls += cc;
            }
        }

        PKCS7_free(p7);

        *certs = _certs;
        *crls  = _crls;

        return ConvertGood;
    }
};

class MyPKCS12Context : public PKCS12Context
{
    Q_OBJECT
public:
    MyPKCS12Context(Provider *p)
        : PKCS12Context(p)
    {
    }

    ~MyPKCS12Context() override
    {
    }

    Provider::Context *clone() const override
    {
        return nullptr;
    }

    QByteArray toPKCS12(const QString                    &name,
                        const QList<const CertContext *> &chain,
                        const PKeyContext                &priv,
                        const SecureArray                &passphrase) const override
    {
        if (chain.count() < 1)
            return QByteArray();

        X509 *cert         = static_cast<const MyCertContext *>(chain[0])->item.cert;
        STACK_OF(X509) *ca = sk_X509_new_null();
        if (chain.count() > 1) {
            for (int n = 1; n < chain.count(); ++n) {
                X509 *x = static_cast<const MyCertContext *>(chain[n])->item.cert;
                X509_up_ref(x);
                sk_X509_push(ca, x);
            }
        }
        const MyPKeyContext &pk  = static_cast<const MyPKeyContext &>(priv);
        PKCS12              *p12 = PKCS12_create(
            (char *)passphrase.data(), (char *)name.toLatin1().data(), pk.get_pkey(), cert, ca, 0, 0, 0, 0, 0);
        sk_X509_pop_free(ca, X509_free);

        if (!p12)
            return QByteArray();

        BIO *bo = BIO_new(BIO_s_mem());
        i2d_PKCS12_bio(bo, p12);
        const QByteArray out = bio2ba(bo);
        return out;
    }

    ConvertResult fromPKCS12(const QByteArray     &in,
                             const SecureArray    &passphrase,
                             QString              *name,
                             QList<CertContext *> *chain,
                             PKeyContext         **priv) const override
    {
        BIO *bi = BIO_new(BIO_s_mem());
        BIO_write(bi, in.data(), in.size());
        PKCS12 *p12 = d2i_PKCS12_bio(bi, nullptr);
        BIO_free(bi);
        if (!p12)
            return ErrorDecode;

        EVP_PKEY *pkey;
        X509     *cert;
        STACK_OF(X509) *ca = nullptr;
        if (!PKCS12_parse(p12, passphrase.data(), &pkey, &cert, &ca)) {
            PKCS12_free(p12);
            return ErrorDecode;
        }
        PKCS12_free(p12);

        // require private key
        if (!pkey) {
            if (cert)
                X509_free(cert);
            if (ca)
                sk_X509_pop_free(ca, X509_free);
            return ErrorDecode;
        }

        // TODO: require cert

        int   aliasLength;
        char *aliasData = (char *)X509_alias_get0(cert, &aliasLength);
        *name           = QString::fromLatin1(aliasData, aliasLength);

        MyPKeyContext *pk = new MyPKeyContext(provider());
        PKeyBase      *k  = pk->pkeyToBase(pkey, true); // does an EVP_PKEY_free()
        if (!k) {
            delete pk;
            if (cert)
                X509_free(cert);
            if (ca)
                sk_X509_pop_free(ca, X509_free);
            return ErrorDecode;
        }
        pk->k = k;
        *priv = pk;

        QList<CertContext *> certs;
        if (cert) {
            MyCertContext *cc = new MyCertContext(provider());
            cc->fromX509(cert);
            certs.append(cc);
            X509_free(cert);
        }
        if (ca) {
            // TODO: reorder in chain-order?
            // TODO: throw out certs that don't fit the chain?
            for (int n = 0; n < sk_X509_num(ca); ++n) {
                MyCertContext *cc = new MyCertContext(provider());
                cc->fromX509(sk_X509_value(ca, n));
                certs.append(cc);
            }
            sk_X509_pop_free(ca, X509_free);
        }

        // reorder, throw out
        QCA::CertificateChain ch;
        for (int n = 0; n < certs.count(); ++n) {
            QCA::Certificate cert;
            cert.change(certs[n]);
            ch += cert;
        }
        certs.clear();
        ch = ch.complete(QList<QCA::Certificate>());
        for (int n = 0; n < ch.count(); ++n) {
            MyCertContext *cc = (MyCertContext *)ch[n].context();
            certs += (new MyCertContext(*cc));
        }
        ch.clear();

        *chain = certs;
        return ConvertGood;
    }
};

class CMSContext : public SMSContext
{
    Q_OBJECT
public:
    CertificateCollection   trustedCerts;
    CertificateCollection   untrustedCerts;
    QList<SecureMessageKey> privateKeys;

    CMSContext(Provider *p)
        : SMSContext(p, QStringLiteral("cms"))
    {
    }

    ~CMSContext() override
    {
    }

    Provider::Context *clone() const override
    {
        return nullptr;
    }

    void setTrustedCertificates(const CertificateCollection &trusted) override
    {
        trustedCerts = trusted;
    }

    void setUntrustedCertificates(const CertificateCollection &untrusted) override
    {
        untrustedCerts = untrusted;
    }

    void setPrivateKeys(const QList<SecureMessageKey> &keys) override
    {
        privateKeys = keys;
    }

    MessageContext *createMessage() override;
};

STACK_OF(X509) * get_pk7_certs(PKCS7 *p7)
{
    int i = OBJ_obj2nid(p7->type);
    if (i == NID_pkcs7_signed)
        return p7->d.sign->cert;
    else if (i == NID_pkcs7_signedAndEnveloped)
        return p7->d.signed_and_enveloped->cert;
    else
        return nullptr;
}

class MyMessageContextThread : public QThread
{
    Q_OBJECT
public:
    SecureMessage::Format   format;
    SecureMessage::SignMode signMode;
    Certificate             cert;
    PrivateKey              key;
    STACK_OF(X509) * other_certs;
    BIO       *bi;
    int        flags;
    PKCS7     *p7;
    bool       ok;
    QByteArray out, sig;

    MyMessageContextThread(QObject *parent = nullptr)
        : QThread(parent)
        , ok(false)
    {
    }

protected:
    static int ssl_error_callback(const char *message, size_t len, void *user_data)
    {
        Q_UNUSED(len)
        auto context = reinterpret_cast<MyMessageContextThread *>(user_data);
        qDebug() << "MyMessageContextThread:" << context << " " << message;
        return 1;
    }

    void run() override
    {
        MyCertContext *cc = static_cast<MyCertContext *>(cert.context());
        MyPKeyContext *kc = static_cast<MyPKeyContext *>(key.context());
        X509          *cx = cc->item.cert;
        EVP_PKEY      *kx = kc->get_pkey();

        p7 = PKCS7_sign(cx, kx, other_certs, bi, flags);

        BIO_free(bi);
        sk_X509_pop_free(other_certs, X509_free);

        if (p7) {
            // printf("good\n");
            BIO *bo;

            // BIO *bo = BIO_new(BIO_s_mem());
            // i2d_PKCS7_bio(bo, p7);
            // PEM_write_bio_PKCS7(bo, p7);
            // SecureArray buf = bio2buf(bo);
            // printf("[%s]\n", buf.data());

            bo = BIO_new(BIO_s_mem());
            if (format == SecureMessage::Binary)
                i2d_PKCS7_bio(bo, p7);
            else // Ascii
                PEM_write_bio_PKCS7(bo, p7);

            if (SecureMessage::Detached == signMode)
                sig = bio2ba(bo);
            else
                out = bio2ba(bo);

            ok = true;
        } else {
            printf("bad here\n");
            ERR_print_errors_cb(&MyMessageContextThread::ssl_error_callback, this);
        }
    }
};

class MyMessageContext : public MessageContext
{
    Q_OBJECT
public:
    CMSContext             *cms;
    SecureMessageKey        signer;
    SecureMessageKeyList    to;
    SecureMessage::SignMode signMode;
    bool                    bundleSigner;
    bool                    smime;
    SecureMessage::Format   format;

    Operation op;
    bool      _finished;

    QByteArray in, out;
    QByteArray sig;
    int        total;

    CertificateChain signerChain;
    int              ver_ret;

    MyMessageContextThread *thread;

    MyMessageContext(CMSContext *_cms, Provider *p)
        : MessageContext(p, QStringLiteral("cmsmsg"))
    {
        cms = _cms;

        total = 0;

        ver_ret = 0;

        thread = nullptr;
    }

    ~MyMessageContext() override
    {
    }

    Provider::Context *clone() const override
    {
        return nullptr;
    }

    bool canSignMultiple() const override
    {
        return false;
    }

    SecureMessage::Type type() const override
    {
        return SecureMessage::CMS;
    }

    void reset() override
    {
    }

    void setupEncrypt(const SecureMessageKeyList &keys) override
    {
        to = keys;
    }

    void setupSign(const SecureMessageKeyList &keys, SecureMessage::SignMode m, bool bundleSigner, bool smime) override
    {
        signer             = keys.first();
        signMode           = m;
        this->bundleSigner = bundleSigner;
        this->smime        = smime;
    }

    void setupVerify(const QByteArray &detachedSig) override
    {
        // TODO
        sig = detachedSig;
    }

    void start(SecureMessage::Format f, Operation op) override
    {
        format    = f;
        _finished = false;

        // TODO: other operations
        // if(op == Sign)
        //{
        this->op = op;
        //}
        // else if(op == Encrypt)
        //{
        //    this->op = op;
        //}
    }

    void update(const QByteArray &in) override
    {
        this->in.append(in);
        total += in.size();
        QMetaObject::invokeMethod(this, "updated", Qt::QueuedConnection);
    }

    QByteArray read() override
    {
        return out;
    }

    int written() override
    {
        int x = total;
        total = 0;
        return x;
    }

    void end() override
    {
        _finished = true;

        // sign
        if (op == Sign) {
            const CertificateChain chain = signer.x509CertificateChain();
            Certificate            cert  = chain.primary();
            QList<Certificate>     nonroots;
            if (chain.count() > 1) {
                for (int n = 1; n < chain.count(); ++n)
                    nonroots.append(chain[n]);
            }
            PrivateKey key = signer.x509PrivateKey();

            const PKeyContext *tmp_kc = static_cast<const PKeyContext *>(key.context());

            if (!tmp_kc->sameProvider(this)) {
                // fprintf(stderr, "experimental: private key supplied by a different provider\n");

                // make a pkey pointing to the existing private key
                EVP_PKEY *pkey = createPkeyFromExisting(key.toRSA());
                if (!pkey) {
                    printf("createPkeyFromExisting failed\n");
                    return;
                }

                // make a new private key object to hold it
                MyPKeyContext *pk = new MyPKeyContext(provider());
                PKeyBase      *k  = pk->pkeyToBase(pkey, true); // does an EVP_PKEY_free()
                pk->k             = k;
                key.change(pk);
            }

            // allow different cert provider.  this is just a
            //   quick hack, enough to please qca-test
            if (!cert.context()->sameProvider(this)) {
                // fprintf(stderr, "experimental: cert supplied by a different provider\n");
                cert = Certificate::fromDER(cert.toDER());
                if (cert.isNull() || !cert.context()->sameProvider(this)) {
                    // fprintf(stderr, "error converting cert\n");
                }
            }

            // MyCertContext *cc = static_cast<MyCertContext *>(cert.context());
            // MyPKeyContext *kc = static_cast<MyPKeyContext *>(key.context());

            // X509 *cx = cc->item.cert;
            // EVP_PKEY *kx = kc->get_pkey();

            STACK_OF(X509) * other_certs;
            BIO *bi;
            int  flags;
            // PKCS7 *p7;

            // nonroots
            other_certs = sk_X509_new_null();
            for (int n = 0; n < nonroots.count(); ++n) {
                X509 *x = static_cast<MyCertContext *>(nonroots[n].context())->item.cert;
                X509_up_ref(x);
                sk_X509_push(other_certs, x);
            }

            // printf("bundling %d other_certs\n", sk_X509_num(other_certs));

            bi = BIO_new(BIO_s_mem());
            BIO_write(bi, in.data(), in.size());

            flags = 0;
            flags |= PKCS7_BINARY;
            if (SecureMessage::Detached == signMode) {
                flags |= PKCS7_DETACHED;
            }
            if (false == bundleSigner)
                flags |= PKCS7_NOCERTS;

            if (thread)
                delete thread;
            thread              = new MyMessageContextThread(this);
            thread->format      = format;
            thread->signMode    = signMode;
            thread->cert        = cert;
            thread->key         = key;
            thread->other_certs = other_certs;
            thread->bi          = bi;
            thread->flags       = flags;
            connect(thread, &MyMessageContextThread::finished, this, &MyMessageContext::thread_finished);
            thread->start();
        } else if (op == Encrypt) {
            // TODO: support multiple recipients
            Certificate target = to.first().x509CertificateChain().primary();

            STACK_OF(X509) * other_certs;
            BIO   *bi;
            int    flags;
            PKCS7 *p7;

            other_certs = sk_X509_new_null();
            X509 *x     = static_cast<MyCertContext *>(target.context())->item.cert;
            X509_up_ref(x);
            sk_X509_push(other_certs, x);

            bi = BIO_new(BIO_s_mem());
            BIO_write(bi, in.data(), in.size());

            flags = 0;
            flags |= PKCS7_BINARY;
            p7 = PKCS7_encrypt(other_certs, bi, EVP_des_ede3_cbc(), flags); // TODO: cipher?

            BIO_free(bi);
            sk_X509_pop_free(other_certs, X509_free);

            if (p7) {
                // FIXME: format
                BIO *bo = BIO_new(BIO_s_mem());
                i2d_PKCS7_bio(bo, p7);
                // PEM_write_bio_PKCS7(bo, p7);
                out = bio2ba(bo);
                PKCS7_free(p7);
            } else {
                printf("bad\n");
                return;
            }
        } else if (op == Verify) {
            // TODO: support non-detached sigs

            BIO *out = BIO_new(BIO_s_mem());
            BIO *bi  = BIO_new(BIO_s_mem());
            if (false == sig.isEmpty()) {
                // We have detached signature
                BIO_write(bi, sig.data(), sig.size());
            } else {
                BIO_write(bi, in.data(), in.size());
            }
            PKCS7 *p7;
            if (format == SecureMessage::Binary)
                p7 = d2i_PKCS7_bio(bi, nullptr);
            else // Ascii
                p7 = PEM_read_bio_PKCS7(bi, nullptr, passphrase_cb, nullptr);
            BIO_free(bi);

            if (!p7) {
                // TODO
                printf("bad1\n");
                QMetaObject::invokeMethod(this, "updated", Qt::QueuedConnection);
                return;
            }

            // intermediates/signers that may not be in the blob
            STACK_OF(X509) *other_certs       = sk_X509_new_null();
            QList<Certificate> untrusted_list = cms->untrustedCerts.certificates();
            const QList<CRL>   untrusted_crls = cms->untrustedCerts.crls(); // we'll use the crls later
            for (int n = 0; n < untrusted_list.count(); ++n) {
                X509 *x = static_cast<MyCertContext *>(untrusted_list[n].context())->item.cert;
                X509_up_ref(x);
                sk_X509_push(other_certs, x);
            }

            // get the possible message signers
            QList<Certificate> signers;
            STACK_OF(X509) *xs = PKCS7_get0_signers(p7, other_certs, 0);
            if (xs) {
                for (int n = 0; n < sk_X509_num(xs); ++n) {
                    MyCertContext *cc = new MyCertContext(provider());
                    cc->fromX509(sk_X509_value(xs, n));
                    Certificate cert;
                    cert.change(cc);
                    // printf("signer: [%s]\n", qPrintable(cert.commonName()));
                    signers.append(cert);
                }
                sk_X509_free(xs);
            }

            // get the rest of the certificates lying around
            QList<Certificate> others;
            xs = get_pk7_certs(p7); // don't free
            if (xs) {
                for (int n = 0; n < sk_X509_num(xs); ++n) {
                    MyCertContext *cc = new MyCertContext(provider());
                    cc->fromX509(sk_X509_value(xs, n));
                    Certificate cert;
                    cert.change(cc);
                    others.append(cert);
                    // printf("other: [%s]\n", qPrintable(cert.commonName()));
                }
            }

            // signer needs to be supplied in the message itself
            //   or via cms->untrustedCerts
            if (signers.isEmpty()) {
                QMetaObject::invokeMethod(this, "updated", Qt::QueuedConnection);
                return;
            }

            // FIXME: handle more than one signer
            CertificateChain chain;
            chain += signers[0];

            // build chain
            chain = chain.complete(others);

            signerChain = chain;

            X509_STORE              *store     = X509_STORE_new();
            const QList<Certificate> cert_list = cms->trustedCerts.certificates();
            QList<CRL>               crl_list  = cms->trustedCerts.crls();
            for (int n = 0; n < cert_list.count(); ++n) {
                // printf("trusted: [%s]\n", qPrintable(cert_list[n].commonName()));
                const MyCertContext *cc = static_cast<const MyCertContext *>(cert_list[n].context());
                X509                *x  = cc->item.cert;
                // CRYPTO_add(&x->references, 1, CRYPTO_LOCK_X509);
                X509_STORE_add_cert(store, x);
            }
            for (int n = 0; n < crl_list.count(); ++n) {
                const MyCRLContext *cc = static_cast<const MyCRLContext *>(crl_list[n].context());
                X509_CRL           *x  = cc->item.crl;
                // CRYPTO_add(&x->references, 1, CRYPTO_LOCK_X509_CRL);
                X509_STORE_add_crl(store, x);
            }
            // add these crls also
            crl_list = untrusted_crls;
            for (int n = 0; n < crl_list.count(); ++n) {
                const MyCRLContext *cc = static_cast<const MyCRLContext *>(crl_list[n].context());
                X509_CRL           *x  = cc->item.crl;
                // CRYPTO_add(&x->references, 1, CRYPTO_LOCK_X509_CRL);
                X509_STORE_add_crl(store, x);
            }

            int ret;
            if (!sig.isEmpty()) {
                // Detached signMode
                bi = BIO_new(BIO_s_mem());
                BIO_write(bi, in.data(), in.size());
                ret = PKCS7_verify(p7, other_certs, store, bi, nullptr, 0);
                BIO_free(bi);
            } else {
                ret = PKCS7_verify(p7, other_certs, store, nullptr, out, 0);
                // qDebug() << "Verify: " << ret;
            }
            // if(!ret)
            //    ERR_print_errors_fp(stdout);
            sk_X509_pop_free(other_certs, X509_free);
            X509_STORE_free(store);
            PKCS7_free(p7);

            ver_ret = ret;
            // TODO

            QMetaObject::invokeMethod(this, "updated", Qt::QueuedConnection);
        } else if (op == Decrypt) {
            bool ok = false;
            for (int n = 0; n < cms->privateKeys.count(); ++n) {
                CertificateChain chain = cms->privateKeys[n].x509CertificateChain();
                Certificate      cert  = chain.primary();
                PrivateKey       key   = cms->privateKeys[n].x509PrivateKey();

                MyCertContext *cc = static_cast<MyCertContext *>(cert.context());
                MyPKeyContext *kc = static_cast<MyPKeyContext *>(key.context());

                X509     *cx = cc->item.cert;
                EVP_PKEY *kx = kc->get_pkey();

                BIO *bi = BIO_new(BIO_s_mem());
                BIO_write(bi, in.data(), in.size());
                PKCS7 *p7 = d2i_PKCS7_bio(bi, nullptr);
                BIO_free(bi);

                if (!p7) {
                    // TODO
                    printf("bad1\n");
                    return;
                }

                BIO *bo  = BIO_new(BIO_s_mem());
                int  ret = PKCS7_decrypt(p7, kx, cx, bo, 0);
                PKCS7_free(p7);
                if (!ret)
                    continue;

                ok  = true;
                out = bio2ba(bo);
                break;
            }

            if (!ok) {
                // TODO
                printf("bad2\n");
                return;
            }
        }
    }

    bool finished() const override
    {
        return _finished;
    }

    bool waitForFinished(int msecs) override
    {
        // TODO
        Q_UNUSED(msecs);

        if (thread) {
            thread->wait();
            getresults();
        }
        return true;
    }

    bool success() const override
    {
        // TODO
        return true;
    }

    SecureMessage::Error errorCode() const override
    {
        // TODO
        return SecureMessage::ErrorUnknown;
    }

    QByteArray signature() const override
    {
        return sig;
    }

    QString hashName() const override
    {
        // TODO
        return QStringLiteral("sha1");
    }

    SecureMessageSignatureList signers() const override
    {
        // only report signers for verify
        if (op != Verify)
            return SecureMessageSignatureList();

        SecureMessageKey key;
        if (!signerChain.isEmpty())
            key.setX509CertificateChain(signerChain);

        // TODO/FIXME !!! InvalidSignature might be used here even
        //   if the signature is just fine, and the key is invalid
        //   (we need to use InvalidKey instead).

        Validity vr = ErrorValidityUnknown;
        if (!signerChain.isEmpty())
            vr = signerChain.validate(cms->trustedCerts, cms->untrustedCerts.crls());

        SecureMessageSignature::IdentityResult ir;
        if (vr == ValidityGood)
            ir = SecureMessageSignature::Valid;
        else
            ir = SecureMessageSignature::InvalidKey;

        if (!ver_ret)
            ir = SecureMessageSignature::InvalidSignature;

        SecureMessageSignature s(ir, vr, key, QDateTime::currentDateTime());

        // TODO
        return SecureMessageSignatureList() << s;
    }

    void getresults()
    {
        sig = thread->sig;
        out = thread->out;
    }

private Q_SLOTS:
    void thread_finished()
    {
        getresults();
        emit updated();
    }
};

MessageContext *CMSContext::createMessage()
{
    return new MyMessageContext(this, provider());
}

class opensslCipherContext : public CipherContext
{
    Q_OBJECT
public:
    opensslCipherContext(const EVP_CIPHER *algorithm, const int pad, Provider *p, const QString &type)
        : CipherContext(p, type)
    {
        m_cryptoAlgorithm = algorithm;
        m_context         = EVP_CIPHER_CTX_new();
        EVP_CIPHER_CTX_reset(m_context);
        m_pad  = pad;
        m_type = type;
    }

    opensslCipherContext(const opensslCipherContext &other)
        : CipherContext(other)
    {
        m_cryptoAlgorithm = other.m_cryptoAlgorithm;
        m_context         = EVP_CIPHER_CTX_new();
        EVP_CIPHER_CTX_copy(m_context, other.m_context);
        m_direction = other.m_direction;
        m_pad       = other.m_pad;
        m_type      = other.m_type;
        m_tag       = other.m_tag;
    }

    ~opensslCipherContext() override
    {
        EVP_CIPHER_CTX_reset(m_context);
        EVP_CIPHER_CTX_free(m_context);
    }

    void setup(Direction dir, const SymmetricKey &key, const InitializationVector &iv, const AuthTag &tag) override
    {
        m_tag       = tag;
        m_direction = dir;
        if ((m_cryptoAlgorithm == EVP_des_ede3()) && (key.size() == 16)) {
            // this is really a two key version of triple DES.
            m_cryptoAlgorithm = EVP_des_ede();
        }
        if (Encode == m_direction) {
            EVP_EncryptInit_ex(m_context, m_cryptoAlgorithm, nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_set_key_length(m_context, key.size());
            if (m_type.endsWith(QLatin1String("gcm")) || m_type.endsWith(QLatin1String("ccm"))) {
                int parameter = m_type.endsWith(QLatin1String("gcm")) ? EVP_CTRL_GCM_SET_IVLEN : EVP_CTRL_CCM_SET_IVLEN;
                EVP_CIPHER_CTX_ctrl(m_context, parameter, iv.size(), nullptr);
            }
            EVP_EncryptInit_ex(
                m_context, nullptr, nullptr, (const unsigned char *)(key.data()), (const unsigned char *)(iv.data()));
        } else {
            EVP_DecryptInit_ex(m_context, m_cryptoAlgorithm, nullptr, nullptr, nullptr);
            EVP_CIPHER_CTX_set_key_length(m_context, key.size());
            if (m_type.endsWith(QLatin1String("gcm")) || m_type.endsWith(QLatin1String("ccm"))) {
                int parameter = m_type.endsWith(QLatin1String("gcm")) ? EVP_CTRL_GCM_SET_IVLEN : EVP_CTRL_CCM_SET_IVLEN;
                EVP_CIPHER_CTX_ctrl(m_context, parameter, iv.size(), nullptr);
            }
            EVP_DecryptInit_ex(
                m_context, nullptr, nullptr, (const unsigned char *)(key.data()), (const unsigned char *)(iv.data()));
        }

        EVP_CIPHER_CTX_set_padding(m_context, m_pad);
    }

    Provider::Context *clone() const override
    {
        return new opensslCipherContext(*this);
    }

    int blockSize() const override
    {
        return EVP_CIPHER_CTX_block_size(m_context);
    }

    AuthTag tag() const override
    {
        return m_tag;
    }

    bool update(const SecureArray &in, SecureArray *out) override
    {
        // This works around a problem in OpenSSL, where it asserts if
        // there is nothing to encrypt.
        if (0 == in.size())
            return true;

        out->resize(in.size() + blockSize());
        int resultLength;
        if (Encode == m_direction) {
            if (0 ==
                EVP_EncryptUpdate(
                    m_context, (unsigned char *)out->data(), &resultLength, (unsigned char *)in.data(), in.size())) {
                return false;
            }
        } else {
            if (0 ==
                EVP_DecryptUpdate(
                    m_context, (unsigned char *)out->data(), &resultLength, (unsigned char *)in.data(), in.size())) {
                return false;
            }
        }
        out->resize(resultLength);
        return true;
    }

    bool final(SecureArray *out) override
    {
        out->resize(blockSize());
        int resultLength;
        if (Encode == m_direction) {
            if (0 == EVP_EncryptFinal_ex(m_context, (unsigned char *)out->data(), &resultLength)) {
                return false;
            }
            if (m_tag.size() && (m_type.endsWith(QLatin1String("gcm")) || m_type.endsWith(QLatin1String("ccm")))) {
                int parameter = m_type.endsWith(QLatin1String("gcm")) ? EVP_CTRL_GCM_GET_TAG : EVP_CTRL_CCM_GET_TAG;
                if (0 == EVP_CIPHER_CTX_ctrl(m_context, parameter, m_tag.size(), (unsigned char *)m_tag.data())) {
                    return false;
                }
            }
        } else {
            if (m_tag.size() && (m_type.endsWith(QLatin1String("gcm")) || m_type.endsWith(QLatin1String("ccm")))) {
                int parameter = m_type.endsWith(QLatin1String("gcm")) ? EVP_CTRL_GCM_SET_TAG : EVP_CTRL_CCM_SET_TAG;
                if (0 == EVP_CIPHER_CTX_ctrl(m_context, parameter, m_tag.size(), m_tag.data())) {
                    return false;
                }
            }
            if (0 == EVP_DecryptFinal_ex(m_context, (unsigned char *)out->data(), &resultLength)) {
                return false;
            }
        }
        out->resize(resultLength);
        return true;
    }

    // Change cipher names
    KeyLength keyLength() const override
    {
        if (s_legacyProviderAvailable) {
            if (m_type.left(4) == QLatin1String("des-")) {
                return KeyLength(8, 8, 1);
            } else if (m_type.left(5) == QLatin1String("cast5")) {
                return KeyLength(5, 16, 1);
            } else if (m_type.left(8) == QLatin1String("blowfish")) {
                // Don't know - TODO
                return KeyLength(1, 32, 1);
            }
        }
        if (m_type.left(6) == QLatin1String("aes128")) {
            return KeyLength(16, 16, 1);
        } else if (m_type.left(6) == QLatin1String("aes192")) {
            return KeyLength(24, 24, 1);
        } else if (m_type.left(6) == QLatin1String("aes256")) {
            return KeyLength(32, 32, 1);
        } else if (m_type.left(9) == QLatin1String("tripledes")) {
            return KeyLength(16, 24, 1);
        }
        return KeyLength(0, 1, 1);
    }

protected:
    EVP_CIPHER_CTX   *m_context;
    const EVP_CIPHER *m_cryptoAlgorithm;
    Direction         m_direction;
    int               m_pad;
    QString           m_type;
    AuthTag           m_tag;
};

static QStringList all_hash_types()
{
    QStringList list;
    list += QStringLiteral("sha1");
#ifdef HAVE_OPENSSL_SHA0
    list += QStringLiteral("sha0");
#endif
    list += QStringLiteral("md5");
#ifdef SHA224_DIGEST_LENGTH
    list += QStringLiteral("sha224");
#endif
#ifdef SHA256_DIGEST_LENGTH
    list += QStringLiteral("sha256");
#endif
#ifdef SHA384_DIGEST_LENGTH
    list += QStringLiteral("sha384");
#endif
#ifdef SHA512_DIGEST_LENGTH
    list += QStringLiteral("sha512");
#endif
#ifdef HAVE_OPENSSL_SHA3_224
    list += QStringLiteral("sha3_224");
#endif
#ifdef HAVE_OPENSSL_SHA3_256
    list += QStringLiteral("sha3_256");
#endif
#ifdef HAVE_OPENSSL_SHA3_384
    list += QStringLiteral("sha3_384");
#endif
#ifdef HAVE_OPENSSL_SHA3_512
    list += QStringLiteral("sha3_512");
#endif
#ifdef HAVE_OPENSSL_BLAKE2B_512
    list += QStringLiteral("blake2b_512");
#endif
    if (s_legacyProviderAvailable) {
        list += QStringLiteral("ripemd160");
#ifdef HAVE_OPENSSL_MD2
        list += QStringLiteral("md2");
#endif
        list += QStringLiteral("md4");
#if defined(OBJ_whirlpool) && !defined(OPENSSL_NO_WHIRLPOOL)
        list += QStringLiteral("whirlpool");
#endif
    }

    return list;
}

static QStringList all_cipher_types()
{
    QStringList list;
    list += QStringLiteral("aes128-ecb");
    list += QStringLiteral("aes128-cfb");
    list += QStringLiteral("aes128-cbc");
    list += QStringLiteral("aes128-cbc-pkcs7");
    list += QStringLiteral("aes128-ofb");
#ifdef HAVE_OPENSSL_AES_CTR
    list += QStringLiteral("aes128-ctr");
#endif
#ifdef HAVE_OPENSSL_AES_GCM
    list += QStringLiteral("aes128-gcm");
#endif
#ifdef HAVE_OPENSSL_AES_CCM
    list += QStringLiteral("aes128-ccm");
#endif
    list += QStringLiteral("aes192-ecb");
    list += QStringLiteral("aes192-cfb");
    list += QStringLiteral("aes192-cbc");
    list += QStringLiteral("aes192-cbc-pkcs7");
    list += QStringLiteral("aes192-ofb");
#ifdef HAVE_OPENSSL_AES_CTR
    list += QStringLiteral("aes192-ctr");
#endif
#ifdef HAVE_OPENSSL_AES_GCM
    list += QStringLiteral("aes192-gcm");
#endif
#ifdef HAVE_OPENSSL_AES_CCM
    list += QStringLiteral("aes192-ccm");
#endif
    list += QStringLiteral("aes256-ecb");
    list += QStringLiteral("aes256-cbc");
    list += QStringLiteral("aes256-cbc-pkcs7");
    list += QStringLiteral("aes256-cfb");
    list += QStringLiteral("aes256-ofb");
#ifdef HAVE_OPENSSL_AES_CTR
    list += QStringLiteral("aes256-ctr");
#endif
#ifdef HAVE_OPENSSL_AES_GCM
    list += QStringLiteral("aes256-gcm");
#endif
#ifdef HAVE_OPENSSL_AES_CCM
    list += QStringLiteral("aes256-ccm");
#endif
    list += QStringLiteral("tripledes-ecb");
    list += QStringLiteral("tripledes-cbc");
    if (s_legacyProviderAvailable) {
        list += QStringLiteral("blowfish-ecb");
        list += QStringLiteral("blowfish-cbc-pkcs7");
        list += QStringLiteral("blowfish-cbc");
        list += QStringLiteral("blowfish-cfb");
        list += QStringLiteral("blowfish-ofb");
        list += QStringLiteral("des-ecb");
        list += QStringLiteral("des-ecb-pkcs7");
        list += QStringLiteral("des-cbc");
        list += QStringLiteral("des-cbc-pkcs7");
        list += QStringLiteral("des-cfb");
        list += QStringLiteral("des-ofb");
#ifndef OPENSSL_NO_CAST
        list += QStringLiteral("cast5-ecb");
        list += QStringLiteral("cast5-cbc");
        list += QStringLiteral("cast5-cbc-pkcs7");
        list += QStringLiteral("cast5-cfb");
        list += QStringLiteral("cast5-ofb");
#endif
    }
    return list;
}

static QStringList all_mac_types()
{
    QStringList list;
    list += QStringLiteral("hmac(md5)");
    list += QStringLiteral("hmac(sha1)");
#ifdef SHA224_DIGEST_LENGTH
    list += QStringLiteral("hmac(sha224)");
#endif
#ifdef SHA256_DIGEST_LENGTH
    list += QStringLiteral("hmac(sha256)");
#endif
#ifdef SHA384_DIGEST_LENGTH
    list += QStringLiteral("hmac(sha384)");
#endif
#ifdef SHA512_DIGEST_LENGTH
    list += QStringLiteral("hmac(sha512)");
#endif
    if (s_legacyProviderAvailable) {
        list += QStringLiteral("hmac(ripemd160)");
    }
    return list;
}

class opensslInfoContext : public InfoContext
{
    Q_OBJECT
public:
    opensslInfoContext(Provider *p)
        : InfoContext(p)
    {
    }

    Provider::Context *clone() const override
    {
        return new opensslInfoContext(*this);
    }

    QStringList supportedHashTypes() const override
    {
        return all_hash_types();
    }

    QStringList supportedCipherTypes() const override
    {
        return all_cipher_types();
    }

    QStringList supportedMACTypes() const override
    {
        return all_mac_types();
    }
};

class opensslRandomContext : public RandomContext
{
    Q_OBJECT
public:
    opensslRandomContext(QCA::Provider *p)
        : RandomContext(p)
    {
    }

    Context *clone() const override
    {
        return new opensslRandomContext(*this);
    }

    QCA::SecureArray nextBytes(int size) override
    {
        QCA::SecureArray buf(size);
        int              r;
        // FIXME: loop while we don't have enough random bytes.
        while (true) {
            r = RAND_bytes((unsigned char *)(buf.data()), size);
            if (r == 1)
                break; // success
        }
        return buf;
    }
};

} // namespace opensslQCAPlugin

using namespace opensslQCAPlugin;

class opensslProvider : public Provider
{
public:
    bool openssl_initted;

    opensslProvider()
    {
        openssl_initted = false;
// OPENSSL_VERSION_MAJOR is only defined in openssl3
#ifdef OPENSSL_VERSION_MAJOR
        /* Load the legacy providers into the default (NULL) library context */
        if (OSSL_PROVIDER_try_load(nullptr, "legacy", 1)) {
            s_legacyProviderAvailable = true;
        }
#else
        s_legacyProviderAvailable = true;
#endif
    }

    void init() override
    {
        // seed the RNG if it's not seeded yet
        if (RAND_status() == 0) {
            char buf[128];
            auto rg = QRandomGenerator::securelySeeded();
            for (char &n : buf)
                n = static_cast<char>(rg.bounded(256));
            RAND_seed(buf, 128);
        }

        openssl_initted = true;
    }

    ~opensslProvider() override
    {
        // FIXME: ?  for now we never deinit, in case other libs/code
        //   are using openssl
        /*if(!openssl_initted)
            return;
        // todo: any other shutdown?
        EVP_cleanup();
        //ENGINE_cleanup();
        CRYPTO_cleanup_all_ex_data();
        ERR_remove_state(0);
        ERR_free_strings();*/
    }

    int qcaVersion() const override
    {
        return QCA_VERSION;
    }

    QString name() const override
    {
        return QStringLiteral("qca-ossl");
    }

    QString credit() const override
    {
        return QStringLiteral(
            "This product includes cryptographic software "
            "written by Eric Young (eay@cryptsoft.com)");
    }

    QStringList features() const override
    {
        QStringList list;
        list += QStringLiteral("random");
        list += all_hash_types();
        list += all_mac_types();
        list += all_cipher_types();
        if (s_legacyProviderAvailable) {
#ifdef HAVE_OPENSSL_MD2
            list += QStringLiteral("pbkdf1(md2)");
#endif
            list += QStringLiteral("pbkdf1(sha1)");
        }
        list += QStringLiteral("pkcs12");
        list += QStringLiteral("pbkdf2(sha1)");
#ifdef SHA224_DIGEST_LENGTH
        list += QStringLiteral("pbkdf2(sha224)");
#endif
#ifdef SHA256_DIGEST_LENGTH
        list += QStringLiteral("pbkdf2(sha256)");
#endif
#ifdef SHA384_DIGEST_LENGTH
        list += QStringLiteral("pbkdf2(sha384)");
#endif
#ifdef SHA512_DIGEST_LENGTH
        list += QStringLiteral("pbkdf2(sha512)");
#endif
#ifndef LIBRESSL_VERSION_NUMBER
        list += QStringLiteral("hkdf(sha256)");
#endif // LIBRESSL_VERSION_NUMBER
        list += QStringLiteral("pkey");
#ifdef EVP_PKEY_ED25519
        list += QStringLiteral("ed25519");
#endif
        list += QStringLiteral("dlgroup");
        list += QStringLiteral("rsa");
        list += QStringLiteral("dsa");
        list += QStringLiteral("dh");
        list += QStringLiteral("cert");
        list += QStringLiteral("csr");
        list += QStringLiteral("crl");
        list += QStringLiteral("certcollection");
        list += QStringLiteral("tls");
        list += QStringLiteral("dtls");
        if (!supportedOsslSRTPProfiles().isEmpty())
            list += QStringLiteral("dtls-srtp");
        list += QStringLiteral("cms");
        list += QStringLiteral("ca");

        return list;
    }

    Context *createContext(const QString &type) override
    {
        // OpenSSL_add_all_digests();
        if (type == QLatin1String("random"))
            return new opensslRandomContext(this);
        else if (type == QLatin1String("info"))
            return new opensslInfoContext(this);
        else if (type == QLatin1String("sha1"))
            return new opensslHashContext(EVP_sha1(), this, type);
#ifdef HAVE_OPENSSL_SHA0
        else if (type == QLatin1String("sha0"))
            return new opensslHashContext(EVP_sha(), this, type);
#endif
        else if (type == QLatin1String("md5"))
            return new opensslHashContext(EVP_md5(), this, type);
#ifdef SHA224_DIGEST_LENGTH
        else if (type == QLatin1String("sha224"))
            return new opensslHashContext(EVP_sha224(), this, type);
#endif
#ifdef SHA256_DIGEST_LENGTH
        else if (type == QLatin1String("sha256"))
            return new opensslHashContext(EVP_sha256(), this, type);
#endif
#ifdef SHA384_DIGEST_LENGTH
        else if (type == QLatin1String("sha384"))
            return new opensslHashContext(EVP_sha384(), this, type);
#endif
#ifdef SHA512_DIGEST_LENGTH
        else if (type == QLatin1String("sha512"))
            return new opensslHashContext(EVP_sha512(), this, type);
#endif
#ifdef HAVE_OPENSSL_SHA3_224
        else if (type == QLatin1String("sha3_224"))
            return new opensslHashContext(EVP_sha3_224(), this, type);
#endif
#ifdef HAVE_OPENSSL_SHA3_256
        else if (type == QLatin1String("sha3_256"))
            return new opensslHashContext(EVP_sha3_256(), this, type);
#endif
#ifdef HAVE_OPENSSL_SHA3_384
        else if (type == QLatin1String("sha3_384"))
            return new opensslHashContext(EVP_sha3_384(), this, type);
#endif
#ifdef HAVE_OPENSSL_SHA3_512
        else if (type == QLatin1String("sha3_512"))
            return new opensslHashContext(EVP_sha3_512(), this, type);
#endif
#ifdef HAVE_OPENSSL_BLAKE2B_512
        else if (type == QLatin1String("blake2b_512"))
            return new opensslHashContext(EVP_blake2b512(), this, type);
#endif
        else if (type == QLatin1String("pbkdf2(sha1)"))
            return new opensslPbkdf2Context(EVP_sha1(), this, type);
#ifdef SHA224_DIGEST_LENGTH
        else if (type == QLatin1String("pbkdf2(sha224)"))
            return new opensslPbkdf2Context(EVP_sha224(), this, type);
#endif
#ifdef SHA256_DIGEST_LENGTH
        else if (type == QLatin1String("pbkdf2(sha256)"))
            return new opensslPbkdf2Context(EVP_sha256(), this, type);
#endif
#ifdef SHA384_DIGEST_LENGTH
        else if (type == QLatin1String("pbkdf2(sha384)"))
            return new opensslPbkdf2Context(EVP_sha384(), this, type);
#endif
#ifdef SHA512_DIGEST_LENGTH
        else if (type == QLatin1String("pbkdf2(sha512)"))
            return new opensslPbkdf2Context(EVP_sha512(), this, type);
#endif
#ifndef LIBRESSL_VERSION_NUMBER
        else if (type == QLatin1String("hkdf(sha256)"))
            return new opensslHkdfContext(this, type);
#endif // LIBRESSL_VERSION_NUMBER
        else if (type == QLatin1String("hmac(md5)"))
            return new opensslHMACContext(EVP_md5(), this, type);
        else if (type == QLatin1String("hmac(sha1)"))
            return new opensslHMACContext(EVP_sha1(), this, type);
#ifdef SHA224_DIGEST_LENGTH
        else if (type == QLatin1String("hmac(sha224)"))
            return new opensslHMACContext(EVP_sha224(), this, type);
#endif
#ifdef SHA256_DIGEST_LENGTH
        else if (type == QLatin1String("hmac(sha256)"))
            return new opensslHMACContext(EVP_sha256(), this, type);
#endif
#ifdef SHA384_DIGEST_LENGTH
        else if (type == QLatin1String("hmac(sha384)"))
            return new opensslHMACContext(EVP_sha384(), this, type);
#endif
#ifdef SHA512_DIGEST_LENGTH
        else if (type == QLatin1String("hmac(sha512)"))
            return new opensslHMACContext(EVP_sha512(), this, type);
#endif
        else if (type == QLatin1String("aes128-ecb"))
            return new opensslCipherContext(EVP_aes_128_ecb(), 0, this, type);
        else if (type == QLatin1String("aes128-cfb"))
            return new opensslCipherContext(EVP_aes_128_cfb(), 0, this, type);
        else if (type == QLatin1String("aes128-cbc"))
            return new opensslCipherContext(EVP_aes_128_cbc(), 0, this, type);
        else if (type == QLatin1String("aes128-cbc-pkcs7"))
            return new opensslCipherContext(EVP_aes_128_cbc(), 1, this, type);
        else if (type == QLatin1String("aes128-ofb"))
            return new opensslCipherContext(EVP_aes_128_ofb(), 0, this, type);
#ifdef HAVE_OPENSSL_AES_CTR
        else if (type == QLatin1String("aes128-ctr"))
            return new opensslCipherContext(EVP_aes_128_ctr(), 0, this, type);
#endif
#ifdef HAVE_OPENSSL_AES_GCM
        else if (type == QLatin1String("aes128-gcm"))
            return new opensslCipherContext(EVP_aes_128_gcm(), 0, this, type);
#endif
#ifdef HAVE_OPENSSL_AES_CCM
        else if (type == QLatin1String("aes128-ccm"))
            return new opensslCipherContext(EVP_aes_128_ccm(), 0, this, type);
#endif
        else if (type == QLatin1String("aes192-ecb"))
            return new opensslCipherContext(EVP_aes_192_ecb(), 0, this, type);
        else if (type == QLatin1String("aes192-cfb"))
            return new opensslCipherContext(EVP_aes_192_cfb(), 0, this, type);
        else if (type == QLatin1String("aes192-cbc"))
            return new opensslCipherContext(EVP_aes_192_cbc(), 0, this, type);
        else if (type == QLatin1String("aes192-cbc-pkcs7"))
            return new opensslCipherContext(EVP_aes_192_cbc(), 1, this, type);
        else if (type == QLatin1String("aes192-ofb"))
            return new opensslCipherContext(EVP_aes_192_ofb(), 0, this, type);
#ifdef HAVE_OPENSSL_AES_CTR
        else if (type == QLatin1String("aes192-ctr"))
            return new opensslCipherContext(EVP_aes_192_ctr(), 0, this, type);
#endif
#ifdef HAVE_OPENSSL_AES_GCM
        else if (type == QLatin1String("aes192-gcm"))
            return new opensslCipherContext(EVP_aes_192_gcm(), 0, this, type);
#endif
#ifdef HAVE_OPENSSL_AES_CCM
        else if (type == QLatin1String("aes192-ccm"))
            return new opensslCipherContext(EVP_aes_192_ccm(), 0, this, type);
#endif
        else if (type == QLatin1String("aes256-ecb"))
            return new opensslCipherContext(EVP_aes_256_ecb(), 0, this, type);
        else if (type == QLatin1String("aes256-cfb"))
            return new opensslCipherContext(EVP_aes_256_cfb(), 0, this, type);
        else if (type == QLatin1String("aes256-cbc"))
            return new opensslCipherContext(EVP_aes_256_cbc(), 0, this, type);
        else if (type == QLatin1String("aes256-cbc-pkcs7"))
            return new opensslCipherContext(EVP_aes_256_cbc(), 1, this, type);
        else if (type == QLatin1String("aes256-ofb"))
            return new opensslCipherContext(EVP_aes_256_ofb(), 0, this, type);
#ifdef HAVE_OPENSSL_AES_CTR
        else if (type == QLatin1String("aes256-ctr"))
            return new opensslCipherContext(EVP_aes_256_ctr(), 0, this, type);
#endif
#ifdef HAVE_OPENSSL_AES_GCM
        else if (type == QLatin1String("aes256-gcm"))
            return new opensslCipherContext(EVP_aes_256_gcm(), 0, this, type);
#endif
#ifdef HAVE_OPENSSL_AES_CCM
        else if (type == QLatin1String("aes256-ccm"))
            return new opensslCipherContext(EVP_aes_256_ccm(), 0, this, type);
#endif
        else if (type == QLatin1String("pkey"))
            return new MyPKeyContext(this);
        else if (type == QLatin1String("dlgroup"))
            return new MyDLGroup(this);
        else if (type == QLatin1String("rsa"))
            return new RSAKey(this);
        else if (type == QLatin1String("dsa"))
            return new DSAKey(this);
#ifdef EVP_PKEY_ED25519
        else if (type == QLatin1String("ed25519"))
            return new Ed25519Key(this, nullptr, false);
#endif
        else if (type == QLatin1String("dh"))
            return new DHKey(this);
        else if (type == QLatin1String("cert"))
            return new MyCertContext(this);
        else if (type == QLatin1String("csr"))
            return new MyCSRContext(this);
        else if (type == QLatin1String("crl"))
            return new MyCRLContext(this);
        else if (type == QLatin1String("certcollection"))
            return new MyCertCollectionContext(this);
        else if (type == QLatin1String("tls"))
            return new OsslTLSContext(this);
        else if (type == QLatin1String("dtls"))
            return new OsslDTLSContext(this);
        else if (type == QLatin1String("cms"))
            return new CMSContext(this);
        else if (type == QLatin1String("ca"))
            return new MyCAContext(this);
        else if (type == QLatin1String("tripledes-ecb"))
            return new opensslCipherContext(EVP_des_ede3(), 0, this, type);
        else if (type == QLatin1String("tripledes-cbc"))
            return new opensslCipherContext(EVP_des_ede3_cbc(), 0, this, type);
        else if (type == QLatin1String("pkcs12"))
            return new MyPKCS12Context(this);

        else if (s_legacyProviderAvailable) {
            if (type == QLatin1String("blowfish-ecb"))
                return new opensslCipherContext(EVP_bf_ecb(), 0, this, type);
            else if (type == QLatin1String("blowfish-cfb"))
                return new opensslCipherContext(EVP_bf_cfb(), 0, this, type);
            else if (type == QLatin1String("blowfish-ofb"))
                return new opensslCipherContext(EVP_bf_ofb(), 0, this, type);
            else if (type == QLatin1String("blowfish-cbc"))
                return new opensslCipherContext(EVP_bf_cbc(), 0, this, type);
            else if (type == QLatin1String("blowfish-cbc-pkcs7"))
                return new opensslCipherContext(EVP_bf_cbc(), 1, this, type);
            else if (type == QLatin1String("des-ecb"))
                return new opensslCipherContext(EVP_des_ecb(), 0, this, type);
            else if (type == QLatin1String("des-ecb-pkcs7"))
                return new opensslCipherContext(EVP_des_ecb(), 1, this, type);
            else if (type == QLatin1String("des-cbc"))
                return new opensslCipherContext(EVP_des_cbc(), 0, this, type);
            else if (type == QLatin1String("des-cbc-pkcs7"))
                return new opensslCipherContext(EVP_des_cbc(), 1, this, type);
            else if (type == QLatin1String("des-cfb"))
                return new opensslCipherContext(EVP_des_cfb(), 0, this, type);
            else if (type == QLatin1String("des-ofb"))
                return new opensslCipherContext(EVP_des_ofb(), 0, this, type);
#ifndef OPENSSL_NO_CAST
            else if (type == QLatin1String("cast5-ecb"))
                return new opensslCipherContext(EVP_cast5_ecb(), 0, this, type);
            else if (type == QLatin1String("cast5-cbc"))
                return new opensslCipherContext(EVP_cast5_cbc(), 0, this, type);
            else if (type == QLatin1String("cast5-cbc-pkcs7"))
                return new opensslCipherContext(EVP_cast5_cbc(), 1, this, type);
            else if (type == QLatin1String("cast5-cfb"))
                return new opensslCipherContext(EVP_cast5_cfb(), 0, this, type);
            else if (type == QLatin1String("cast5-ofb"))
                return new opensslCipherContext(EVP_cast5_ofb(), 0, this, type);
#endif
            else if (type == QLatin1String("hmac(ripemd160)"))
                return new opensslHMACContext(EVP_ripemd160(), this, type);
            else if (type == QLatin1String("ripemd160"))
                return new opensslHashContext(EVP_ripemd160(), this, type);
#ifdef HAVE_OPENSSL_MD2
            else if (type == QLatin1String("md2"))
                return new opensslHashContext(EVP_md2(), this, type);
            else if (type == QLatin1String("pbkdf1(md2)"))
                return new opensslPbkdf1Context(EVP_md2(), this, type);
#endif
            else if (type == QLatin1String("md4"))
                return new opensslHashContext(EVP_md4(), this, type);
#if defined(OBJ_whirlpool) && !defined(OPENSSL_NO_WHIRLPOOL)
            else if (type == QLatin1String("whirlpool"))
                return new opensslHashContext(EVP_whirlpool(), this, type);
#endif
            else if (type == QLatin1String("pbkdf1(sha1)"))
                return new opensslPbkdf1Context(EVP_sha1(), this, type);
        }

        return nullptr;
    }
};

class qca_ossl : public QObject, public QCAPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.affinix.qca.Plugin/1.0")
    Q_INTERFACES(QCAPlugin)
public:
    Provider *createProvider() override
    {
        return new opensslProvider;
    }
};

#include "qca-ossl.moc"
