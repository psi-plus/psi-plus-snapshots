/*
 * Copyright (C) 2026 QCA contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef QCA_CYRUS_SASL_CRYPTO_H
#define QCA_CYRUS_SASL_CRYPTO_H

#include <QtCrypto>
#include <algorithm>
#include <climits>
#include <cstring>
#include <new>
extern "C" {
#include <sasl/sasl.h>
}
#ifndef _WIN32
#include <sys/uio.h>
#endif

namespace saslQCAPlugin {
namespace Crypto {
using namespace QCA;

struct Algorithm
{
    const char *saslName;
    const char *qcaName;
    unsigned    size;
};

template<typename Function> int invoke(Function function)
{
#ifndef QT_NO_EXCEPTIONS
    try {
#endif
        return function();
#ifndef QT_NO_EXCEPTIONS
    } catch (const std::bad_alloc &) {
        return SASL_NOMEM;
    } catch (...) {
        return SASL_FAIL;
    }
#endif
}

inline bool validOutput(const Algorithm *algorithm, unsigned char *output, unsigned length)
{
    return algorithm && output && length == algorithm->size;
}

inline void update(BufferedComputation &computation, const unsigned char *input, size_t length)
{
    while (length) {
        const int chunk = int(std::min(length, size_t(INT_MAX)));
        computation.update(QByteArray::fromRawData(reinterpret_cast<const char *>(input), chunk));
        input += chunk;
        length -= size_t(chunk);
    }
}

inline SecureArray secure(const unsigned char *input, size_t length)
{
    return SecureArray(QByteArray::fromRawData(reinterpret_cast<const char *>(input), int(length)));
}

inline int copyResult(const MemoryRegion &result, unsigned char *output, unsigned length)
{
    if (result.size() != int(length))
        return SASL_FAIL;
    std::memcpy(output, result.constData(), length);
    return SASL_OK;
}

inline int hash(void *context, const struct iovec *input, unsigned count, unsigned char *output, unsigned length)
{
    const auto *algorithm = static_cast<const Algorithm *>(context);
    if (!validOutput(algorithm, output, length) || (count && !input))
        return SASL_BADPARAM;
    return invoke([&] {
        Hash computation(QString::fromLatin1(algorithm->qcaName));
        if (!computation.context())
            return SASL_NOMECH;
        for (unsigned i = 0; i < count; ++i) {
#ifdef _WIN32
            if (input[i].iov_len < 0)
                return SASL_BADPARAM;
#endif
            if (input[i].iov_len && !input[i].iov_base)
                return SASL_BADPARAM;
            update(computation, reinterpret_cast<const unsigned char *>(input[i].iov_base), size_t(input[i].iov_len));
        }
        return copyResult(computation.final(), output, length);
    });
}

inline int hmac(void                *context,
                const unsigned char *key,
                size_t               keyLength,
                const unsigned char *input,
                size_t               inputLength,
                unsigned char       *output,
                unsigned             length)
{
    const auto *algorithm = static_cast<const Algorithm *>(context);
    if (!validOutput(algorithm, output, length) || keyLength > INT_MAX || (keyLength && !key) ||
        (inputLength && !input))
        return SASL_BADPARAM;
    return invoke([&] {
        const QString             name = QStringLiteral("hmac(%1)").arg(QString::fromLatin1(algorithm->qcaName));
        MessageAuthenticationCode computation(name, SymmetricKey(secure(key, keyLength)));
        if (!computation.context())
            return SASL_NOMECH;
        update(computation, input, inputLength);
        return copyResult(computation.final(), output, length);
    });
}

inline int pbkdf2(void                *context,
                  const unsigned char *password,
                  size_t               passwordLength,
                  const unsigned char *salt,
                  size_t               saltLength,
                  unsigned             iterations,
                  unsigned char       *output,
                  unsigned             length)
{
    const auto *algorithm = static_cast<const Algorithm *>(context);
    if (!validOutput(algorithm, output, length) || passwordLength > INT_MAX || saltLength > INT_MAX || !iterations ||
        (passwordLength && !password) || (saltLength && !salt))
        return SASL_BADPARAM;
    return invoke([&] {
        PBKDF2 computation(QString::fromLatin1(algorithm->qcaName));
        if (!computation.context())
            return SASL_NOMECH;
        return copyResult(
            computation.makeKey(
                secure(password, passwordLength), InitializationVector(secure(salt, saltLength)), length, iterations),
            output,
            length);
    });
}

inline int select(void *, const char *name, const sasl_crypto_t **operations, void **context)
{
    if (!name || !operations || !context)
        return SASL_BADPARAM;
    *operations = nullptr;
    *context    = nullptr;
    return invoke([&] {
        static const Algorithm     algorithms[] = {{"MD5", "md5", 16},
                                                   {"SHA1", "sha1", 20},
                                                   {"SHA224", "sha224", 28},
                                                   {"SHA256", "sha256", 32},
                                                   {"SHA384", "sha384", 48},
                                                   {"SHA512", "sha512", 64}};
        static const sasl_crypto_t tables[]     = {{SASL_CRYPTO_VERSION, hash, nullptr, nullptr},
                                                   {SASL_CRYPTO_VERSION, hash, hmac, nullptr},
                                                   {SASL_CRYPTO_VERSION, hash, nullptr, pbkdf2},
                                                   {SASL_CRYPTO_VERSION, hash, hmac, pbkdf2}};
        for (const Algorithm &algorithm : algorithms) {
            if (std::strcmp(name, algorithm.saslName))
                continue;
            const QString hashName = QString::fromLatin1(algorithm.qcaName);
            if (!isSupported(algorithm.qcaName))
                return SASL_NOMECH;
            const bool haveMac = isSupported(QStringList {QStringLiteral("hmac(%1)").arg(hashName)});
            const bool haveKdf = isSupported(QStringList {QStringLiteral("pbkdf2(%1)").arg(hashName)});
            *operations        = &tables[(haveMac ? 1 : 0) | (haveKdf ? 2 : 0)];
            *context           = const_cast<Algorithm *>(&algorithm);
            return SASL_OK;
        }
        return SASL_NOMECH;
    });
}

} // namespace Crypto
} // namespace saslQCAPlugin
#endif
