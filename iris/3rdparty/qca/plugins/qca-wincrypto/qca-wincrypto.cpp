/*
 * Copyright (C) 2008  Michael Leupold <lemma@confuego.org>
 * Copyright (C) 2026  Psi IM Team
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

// QCA provider on the Windows Cryptography API: Next Generation (CNG,
// bcrypt.dll): random numbers, hashes, HMAC, PBKDF2, HKDF and AES ciphers.
// Only CNG functions available since Windows 7 are used; SHA-3 is offered
// when the running Windows has it.

#include "aesmodes.h"
#include "native_keys.h"
#include "native_tls.h"

#include <QtCrypto>
#include <QtPlugin>

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <cstring>
#include <limits>

using namespace QCA;

namespace wincryptoQCAPlugin {

namespace {

bool ok(NTSTATUS status)
{
    return BCRYPT_SUCCESS(status);
}

PUCHAR bytes(const void *data)
{
    // CNG takes non-const pointers even for input.
    return static_cast<PUCHAR>(const_cast<void *>(data));
}

// A valid pointer for empty input: CNG may reject a null one.
UCHAR *emptyInput()
{
    static UCHAR none = 0;
    return &none;
}

}

//----------------------------------------------------------------------------
// Algorithm handles
//----------------------------------------------------------------------------
enum class Algorithm
{
    MD5,
    SHA1,
    SHA256,
    SHA384,
    SHA512,
    SHA3_256,
    SHA3_384,
    SHA3_512,
    HmacMD5,
    HmacSHA1,
    HmacSHA256,
    HmacSHA384,
    HmacSHA512,
    AES,
    AES_GCM,
    Count
};

struct AlgorithmInfo
{
    BCRYPT_ALG_HANDLE handle       = nullptr;
    int               objectLength = 0;
    int               hashLength   = 0;
};

class WinCryptoProvider : public Provider
{
public:
    ~WinCryptoProvider() override
    {
        for (const auto &algorithm : _algorithms)
            if (algorithm.handle)
                BCryptCloseAlgorithmProvider(algorithm.handle, 0);
    }

    int qcaVersion() const override
    {
        return QCA_VERSION;
    }

    QString name() const override
    {
        return QStringLiteral("qca-wincrypto");
    }

    QStringList features() const override;
    Context    *createContext(const QString &type) override;

    // Opened on first use and kept until the provider goes away. CNG
    // algorithm handles may be used from several threads at once.
    AlgorithmInfo algorithmInfo(Algorithm algorithm) const
    {
        QMutexLocker locker(&_mutex);
        const int    index = int(algorithm);
        if (!_opened[index]) {
            _opened[index]    = true;
            auto &info        = _algorithms[index];
            info.handle       = open(algorithm);
            const auto length = [&](const wchar_t *property) {
                DWORD value = 0, written = 0;
                if (!info.handle ||
                    !ok(BCryptGetProperty(
                        info.handle, property, reinterpret_cast<PUCHAR>(&value), sizeof(value), &written, 0)) ||
                    written != sizeof(value) || value > DWORD(std::numeric_limits<int>::max()))
                    return 0;
                return int(value);
            };
            info.objectLength = length(BCRYPT_OBJECT_LENGTH);
            if (algorithm != Algorithm::AES && algorithm != Algorithm::AES_GCM)
                info.hashLength = length(BCRYPT_HASH_LENGTH);
        }
        return _algorithms[index];
    }

    BCRYPT_ALG_HANDLE handle(Algorithm algorithm) const
    {
        return algorithmInfo(algorithm).handle;
    }

    int hashLength(Algorithm algorithm) const
    {
        return algorithmInfo(algorithm).hashLength;
    }

private:
    static BCRYPT_ALG_HANDLE open(Algorithm algorithm)
    {
        const wchar_t *id    = nullptr;
        ULONG          flags = 0;
        switch (algorithm) {
        case Algorithm::MD5:
            id = BCRYPT_MD5_ALGORITHM;
            break;
        case Algorithm::SHA1:
            id = BCRYPT_SHA1_ALGORITHM;
            break;
        case Algorithm::SHA256:
            id = BCRYPT_SHA256_ALGORITHM;
            break;
        case Algorithm::SHA384:
            id = BCRYPT_SHA384_ALGORITHM;
            break;
        case Algorithm::SHA512:
            id = BCRYPT_SHA512_ALGORITHM;
            break;
        // SHA-3 came with Windows 11 24H2; older versions fail to open it.
        case Algorithm::SHA3_256:
            id = L"SHA3-256";
            break;
        case Algorithm::SHA3_384:
            id = L"SHA3-384";
            break;
        case Algorithm::SHA3_512:
            id = L"SHA3-512";
            break;
        case Algorithm::HmacMD5:
            id    = BCRYPT_MD5_ALGORITHM;
            flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
            break;
        case Algorithm::HmacSHA1:
            id    = BCRYPT_SHA1_ALGORITHM;
            flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
            break;
        case Algorithm::HmacSHA256:
            id    = BCRYPT_SHA256_ALGORITHM;
            flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
            break;
        case Algorithm::HmacSHA384:
            id    = BCRYPT_SHA384_ALGORITHM;
            flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
            break;
        case Algorithm::HmacSHA512:
            id    = BCRYPT_SHA512_ALGORITHM;
            flags = BCRYPT_ALG_HANDLE_HMAC_FLAG;
            break;
        case Algorithm::AES:
        case Algorithm::AES_GCM:
            id = BCRYPT_AES_ALGORITHM;
            break;
        case Algorithm::Count:
            return nullptr;
        }
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!ok(BCryptOpenAlgorithmProvider(&alg, id, nullptr, flags)))
            return nullptr;
        if (algorithm == Algorithm::AES || algorithm == Algorithm::AES_GCM) {
            // The modes are implemented in aesmodes.cpp on whole blocks.
            if (!ok(BCryptSetProperty(
                    alg,
                    BCRYPT_CHAINING_MODE,
                    bytes(algorithm == Algorithm::AES ? BCRYPT_CHAIN_MODE_ECB : BCRYPT_CHAIN_MODE_GCM),
                    algorithm == Algorithm::AES ? sizeof(BCRYPT_CHAIN_MODE_ECB) : sizeof(BCRYPT_CHAIN_MODE_GCM),
                    0))) {
                BCryptCloseAlgorithmProvider(alg, 0);
                return nullptr;
            }
        }
        return alg;
    }

    mutable QMutex                                                   _mutex;
    mutable std::array<AlgorithmInfo, std::size_t(Algorithm::Count)> _algorithms {};
    mutable std::array<bool, std::size_t(Algorithm::Count)>          _opened {};
};

namespace {

WinCryptoProvider *cng(Provider *provider)
{
    return static_cast<WinCryptoProvider *>(provider);
}

// CNG keeps pointers into the supplied object buffer. Never share or relocate
// it while a handle is alive; duplicates get a fresh allocation. Match native
// heap alignment. Use SecureArray's allocator directly: native mutable objects
// must have a unique stable address and need no implicit-sharing metadata.
class SecureObject
{
public:
    SecureObject() = default;
    explicit SecureObject(int size)
    {
        resize(size);
    }
    SecureObject(const SecureObject &)            = delete;
    SecureObject &operator=(const SecureObject &) = delete;
    ~SecureObject()
    {
        clear();
    }
    bool resize(int size)
    {
        if (size <= 0 || size > std::numeric_limits<int>::max() - 15 - int(sizeof(int)))
            return false;
        if (size == _size)
            return true;
        clear();
        _storage = static_cast<PUCHAR>(qca_secure_alloc(size + 15));
        if (!_storage)
            return false;
        _size   = size;
        _offset = int((16 - reinterpret_cast<std::uintptr_t>(_storage) % 16) % 16);
        return true;
    }
    PUCHAR data()
    {
        _dirty = true;
        return _storage + _offset;
    }
    int size() const
    {
        return _size;
    }
    void wipe()
    {
        if (_dirty && _storage)
            secureWipe(_storage, std::size_t(_size) + 15);
        _dirty = false;
    }
    void clear()
    {
        wipe();
        if (_storage)
            qca_secure_free(_storage);
        _storage = nullptr;
        _size = _offset = 0;
    }

private:
    PUCHAR _storage = nullptr;
    int    _size = 0, _offset = 0;
    bool   _dirty = false;
};

// A CNG hash or HMAC object, with the data hashed so far.
class HashObject
{
public:
    HashObject()                              = default;
    HashObject(const HashObject &)            = delete;
    HashObject &operator=(const HashObject &) = delete;
    ~HashObject()
    {
        reset();
    }

    bool create(const AlgorithmInfo &algorithm, const SecureArray *secret)
    {
        reset();
        if (!algorithm.handle || !_object.resize(algorithm.objectLength))
            return false;
        PUCHAR key    = nullptr;
        ULONG  length = 0;
        if (secret) {
            // HMAC pads its key with zeros, so an empty key is the same as
            // one zero byte, which CNG accepts.
            key    = secret->isEmpty() ? emptyInput() : bytes(secret->constData());
            length = secret->isEmpty() ? 1 : ULONG(secret->size());
        }
        return accept(
            BCryptCreateHash(algorithm.handle, &_hash, _object.data(), ULONG(_object.size()), key, length, 0));
    }

    bool duplicate(const HashObject &other)
    {
        reset();
        return other._hash && _object.resize(other._object.size()) &&
            accept(BCryptDuplicateHash(other._hash, &_hash, _object.data(), ULONG(_object.size()), 0));
    }

    void update(const char *data, int size)
    {
        if (_hash && size > 0 && !ok(BCryptHashData(_hash, bytes(data), ULONG(size), 0)))
            reset(); // Never return the digest of a silently truncated input.
    }

    SecureArray finish(int length)
    {
        SecureArray result(length);
        if (!_hash || length <= 0 ||
            !ok(BCryptFinishHash(_hash, reinterpret_cast<PUCHAR>(result.data()), ULONG(length), 0)))
            result.clear();
        // A finished CNG hash cannot be used again.
        reset();
        return result;
    }

    bool isValid() const
    {
        return _hash;
    }

private:
    bool accept(NTSTATUS status)
    {
        if (ok(status))
            return true;
        reset();
        _object.clear();
        return false;
    }
    void reset()
    {
        if (_hash) {
            BCryptDestroyHash(_hash);
            _object.wipe();
        }
        _hash = nullptr;
    }

    BCRYPT_HASH_HANDLE _hash = nullptr;
    SecureObject       _object;
};

// HMAC(key, data) in one call, for HKDF.
SecureArray hmac(WinCryptoProvider *provider, Algorithm algorithm, const SecureArray &key, const MemoryRegion &data)
{
    HashObject object;
    const auto info = provider->algorithmInfo(algorithm);
    if (!object.create(info, &key))
        return {};
    object.update(data.constData(), data.size());
    return object.finish(info.hashLength);
}

}

//----------------------------------------------------------------------------
// Random
//----------------------------------------------------------------------------
class WinCryptoRandomContext : public RandomContext
{
    Q_OBJECT
public:
    WinCryptoRandomContext(Provider *p)
        : RandomContext(p)
    {
    }

    Context *clone() const override
    {
        return new WinCryptoRandomContext(*this);
    }

    SecureArray nextBytes(int size) override
    {
        SecureArray buffer(qMax(0, size));
        if (size > 0 &&
            !ok(BCryptGenRandom(
                nullptr, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            buffer.clear();
        return buffer;
    }
};

//----------------------------------------------------------------------------
// Hash
//----------------------------------------------------------------------------
class WinCryptoHashContext : public HashContext
{
    Q_OBJECT
public:
    WinCryptoHashContext(Algorithm algorithm, Provider *p, const QString &type)
        : HashContext(p, type)
        , _algorithm(algorithm)
    {
        clear();
    }

    WinCryptoHashContext(const WinCryptoHashContext &other)
        : HashContext(other)
        , _algorithm(other._algorithm)
    {
        _hash.duplicate(other._hash);
    }

    Context *clone() const override
    {
        return new WinCryptoHashContext(*this);
    }

    void clear() override
    {
        _hash.create(cng(provider())->algorithmInfo(_algorithm), nullptr);
    }

    void update(const MemoryRegion &a) override
    {
        _hash.update(a.constData(), a.size());
    }

    MemoryRegion final() override
    {
        const SecureArray result = _hash.finish(cng(provider())->hashLength(_algorithm));
        clear();
        return result;
    }

private:
    Algorithm  _algorithm;
    HashObject _hash;
};

//----------------------------------------------------------------------------
// HMAC
//----------------------------------------------------------------------------
class WinCryptoHMACContext : public MACContext
{
    Q_OBJECT
public:
    WinCryptoHMACContext(Algorithm algorithm, Provider *p, const QString &type)
        : MACContext(p, type)
        , _algorithm(algorithm)
    {
    }

    WinCryptoHMACContext(const WinCryptoHMACContext &other)
        : MACContext(other)
        , _algorithm(other._algorithm)
    {
        _hash.duplicate(other._hash);
    }

    Context *clone() const override
    {
        return new WinCryptoHMACContext(*this);
    }

    void setup(const SymmetricKey &key) override
    {
        _hash.create(cng(provider())->algorithmInfo(_algorithm), &key);
    }

    KeyLength keyLength() const override
    {
        return anyKeyLength();
    }

    void update(const MemoryRegion &in) override
    {
        _hash.update(in.constData(), in.size());
    }

    void final(MemoryRegion *out) override
    {
        *out = _hash.finish(cng(provider())->hashLength(_algorithm));
        // QCA keeps the key in SecureArray and calls setup() on clear(). Do
        // not retain another key reference or recreate keyed state while idle.
    }

private:
    Algorithm  _algorithm;
    HashObject _hash;
};

//----------------------------------------------------------------------------
// PBKDF2
//----------------------------------------------------------------------------
class WinCryptoPbkdf2Context : public KDFContext
{
    Q_OBJECT
public:
    WinCryptoPbkdf2Context(Algorithm hmacAlgorithm, Provider *p, const QString &type)
        : KDFContext(p, type)
        , _algorithm(hmacAlgorithm)
    {
    }

    Context *clone() const override
    {
        return new WinCryptoPbkdf2Context(*this);
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         unsigned int                keyLength,
                         unsigned int                iterationCount) override
    {
        if (!keyLength || keyLength > unsigned(std::numeric_limits<int>::max()) || !iterationCount)
            return SymmetricKey();
        SecureArray       out(static_cast<int>(keyLength));
        BCRYPT_ALG_HANDLE prf = cng(provider())->handle(_algorithm);
        if (!prf || !keyLength ||
            !ok(BCryptDeriveKeyPBKDF2(prf,
                                      secret.isEmpty() ? emptyInput() : bytes(secret.constData()),
                                      ULONG(secret.size()),
                                      salt.isEmpty() ? emptyInput() : bytes(salt.constData()),
                                      ULONG(salt.size()),
                                      ULONGLONG(iterationCount),
                                      reinterpret_cast<PUCHAR>(out.data()),
                                      ULONG(keyLength),
                                      0)))
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
        // Measure real PBKDF2 work rather than counting one-iteration calls
        // dominated by CNG setup/allocation. Reuse the last derived sample.
        QElapsedTimer timer;
        timer.start();
        unsigned count = 1;
        do {
            auto sample = makeKey(secret, salt, keyLength, count);
            if (sample.size() != int(keyLength))
                return SymmetricKey();
            if (timer.elapsed() >= msecInterval || count == std::numeric_limits<unsigned>::max()) {
                *iterationCount = count;
                return sample;
            }
            count = count > std::numeric_limits<unsigned>::max() / 2 ? std::numeric_limits<unsigned>::max() : count * 2;
        } while (true);
    }

private:
    Algorithm _algorithm;
};

//----------------------------------------------------------------------------
// HKDF (RFC 5869) on CNG's HMAC
//----------------------------------------------------------------------------
class WinCryptoHkdfContext : public HKDFContext
{
    Q_OBJECT
public:
    WinCryptoHkdfContext(Algorithm hmacAlgorithm, Provider *p, const QString &type)
        : HKDFContext(p, type)
        , _algorithm(hmacAlgorithm)
    {
    }

    Context *clone() const override
    {
        return new WinCryptoHkdfContext(*this);
    }

    SymmetricKey makeKey(const SecureArray          &secret,
                         const InitializationVector &salt,
                         const InitializationVector &info,
                         unsigned int                keyLength) override
    {
        WinCryptoProvider *p      = cng(provider());
        const int          length = p->hashLength(_algorithm);
        if (length <= 0 || keyLength == 0 || keyLength > 255u * unsigned(length))
            return SymmetricKey();

        // Extract: a missing salt is a string of zeros as long as the hash.
        const SecureArray prk =
            hmac(p, _algorithm, salt.isEmpty() ? SecureArray(length, 0) : SecureArray(salt), secret);
        if (prk.size() != length)
            return SymmetricKey();

        // Expand.
        SecureArray out(int(keyLength), 0);
        SecureArray previous;
        unsigned    offset = 0;
        HashObject  object;
        const auto  infoAlgorithm = p->algorithmInfo(_algorithm);
        for (unsigned counter = 1; offset < keyLength; ++counter) {
            if (!object.create(infoAlgorithm, &prk))
                return SymmetricKey();
            object.update(previous.constData(), previous.size());
            object.update(info.constData(), info.size());
            const char countByte = char(counter);
            object.update(&countByte, 1);
            previous = object.finish(length);
            if (previous.size() != length)
                return SymmetricKey();
            const auto take = qMin(unsigned(length), keyLength - offset);
            std::memcpy(out.data() + offset, previous.constData(), take);
            offset += take;
        }
        return out;
    }

private:
    Algorithm _algorithm;
};

//----------------------------------------------------------------------------
// AES
//----------------------------------------------------------------------------
namespace {

// AES on whole blocks with a CNG key in ECB mode.
class CngAes final : public BlockCipher
{
public:
    static std::unique_ptr<CngAes> create(const AlgorithmInfo &algorithm, const SecureArray &key)
    {
        if (!algorithm.handle)
            return nullptr;
        auto aes = std::unique_ptr<CngAes>(new CngAes(algorithm.objectLength));
        if (!aes->_object.size() ||
            !ok(BCryptGenerateSymmetricKey(algorithm.handle,
                                           &aes->_key,
                                           aes->_object.data(),
                                           ULONG(aes->_object.size()),
                                           bytes(key.constData()),
                                           ULONG(key.size()),
                                           0)))
            return nullptr;
        return aes;
    }

    ~CngAes() override
    {
        if (_key)
            BCryptDestroyKey(_key);
    }

    std::unique_ptr<BlockCipher> clone() const override
    {
        auto copy = std::unique_ptr<CngAes>(new CngAes(_object.size()));
        if (!copy->_object.size() ||
            !ok(BCryptDuplicateKey(_key, &copy->_key, copy->_object.data(), ULONG(copy->_object.size()), 0)))
            return nullptr;
        return copy;
    }

    bool encrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) override
    {
        ULONG       written = 0;
        const ULONG size    = ULONG(blocks * BlockSize);
        return ok(BCryptEncrypt(_key, bytes(in), size, nullptr, nullptr, 0, out, size, &written, 0)) && written == size;
    }

    bool decrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) override
    {
        ULONG       written = 0;
        const ULONG size    = ULONG(blocks * BlockSize);
        return ok(BCryptDecrypt(_key, bytes(in), size, nullptr, nullptr, 0, out, size, &written, 0)) && written == size;
    }

    bool encryptCbc(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks, std::uint8_t *iv) override
    {
        if (!ok(BCryptSetProperty(
                _key, BCRYPT_CHAINING_MODE, bytes(BCRYPT_CHAIN_MODE_CBC), sizeof(BCRYPT_CHAIN_MODE_CBC), 0)))
            return false;
        ULONG      written = 0;
        const auto size    = ULONG(blocks * BlockSize);
        const bool done =
            ok(BCryptEncrypt(_key, bytes(in), size, nullptr, iv, BlockSize, out, size, &written, 0)) && written == size;
        const bool restored = ok(BCryptSetProperty(
            _key, BCRYPT_CHAINING_MODE, bytes(BCRYPT_CHAIN_MODE_ECB), sizeof(BCRYPT_CHAIN_MODE_ECB), 0));
        return done && restored;
    }

private:
    explicit CngAes(int objectLength)
        : _object(objectLength)
    {
    }

    BCRYPT_KEY_HANDLE _key = nullptr;
    SecureObject      _object;
};

Bytes toBytes(const MemoryRegion &region)
{
    if (region.isEmpty())
        return {};
    const auto *data = reinterpret_cast<const std::uint8_t *>(region.constData());
    return Bytes(data, data + region.size());
}

// The common 96-bit nonce / 12..16-byte tag case is handled entirely by
// CNG, including its hardware GHASH. Encryption streams whole blocks;
// decryption retains ciphertext and authenticates before publishing output.
class CngGcm
{
public:
    CngGcm(const AlgorithmInfo        &algorithm,
           const SymmetricKey         &key,
           const InitializationVector &iv,
           const AuthTag              &tag,
           bool                        encrypt)
        : _object(algorithm.objectLength)
        , _nonce(iv)
        , _tag(tag)
        , _encrypt(encrypt)
    {
        BCRYPT_INIT_AUTH_MODE_INFO(_auth);
        _valid = algorithm.handle && _object.size() &&
            ok(BCryptGenerateSymmetricKey(algorithm.handle,
                                          &_key,
                                          _object.data(),
                                          ULONG(_object.size()),
                                          bytes(key.constData()),
                                          ULONG(key.size()),
                                          0));
    }
    CngGcm(const CngGcm &other)
        : _object(other._object.size())
        , _nonce(other._nonce)
        , _tag(other._tag)
        , _pending(other._pending)
        , _auth(other._auth)
        , _iv(other._iv)
        , _mac(other._mac)
        , _total(other._total)
        , _encrypt(other._encrypt)
    {
        _valid = other._valid && _object.size() &&
            ok(BCryptDuplicateKey(other._key, &_key, _object.data(), ULONG(_object.size()), 0));
    }
    ~CngGcm()
    {
        releaseKey();
    }
    bool update(const SecureArray &in, SecureArray *out)
    {
        out->clear();
        constexpr quint64 maxBytes = (quint64(1) << 36) - 32;
        if (!_valid || quint64(in.size()) > maxBytes - _total)
            return fail(out);
        _total += quint64(in.size());
        const auto *data = reinterpret_cast<const std::uint8_t *>(in.constData());
        auto        size = std::size_t(in.size());
        if (!_encrypt) {
            if (size > std::size_t(std::numeric_limits<int>::max()) - _pending.size())
                return fail(out);
            if (size)
                _pending.insert(_pending.end(), data, data + size);
            return true;
        }
        const auto blocks = (_pending.size() + size) / 16 * 16;
        if (blocks > std::size_t(std::numeric_limits<int>::max()))
            return fail(out);
        out->resize(int(blocks));
        std::size_t offset = 0;
        if (!_pending.empty()) {
            const auto take = std::min(size, 16 - _pending.size());
            if (take) {
                _pending.insert(_pending.end(), data, data + take);
                data += take;
                size -= take;
            }
            if (_pending.size() == 16) {
                if (!crypt(_pending.data(), 16, out->data(), false))
                    return fail(out);
                secureWipe(_pending);
                offset = 16;
            }
        }
        const auto whole = size / 16 * 16;
        if (whole && !crypt(data, ULONG(whole), out->data() + offset, false))
            return fail(out);
        if (size > whole)
            _pending.insert(_pending.end(), data + whole, data + size);
        return true;
    }
    bool final(SecureArray *out)
    {
        out->clear();
        if (!_valid)
            return false;
        out->resize(int(_pending.size()));
        // A non-null output is essential even for an empty message: a null
        // output asks CNG for a size without performing authentication.
        UCHAR      dummy = 0;
        const auto size  = ULONG(_pending.size());
        const bool success =
            crypt(size ? _pending.data() : &dummy, size, size ? out->data() : reinterpret_cast<char *>(&dummy), true);
        secureWipe(_pending);
        _valid = false;
        releaseKey();
        if (!success)
            out->clear();
        return success;
    }
    AuthTag tag() const
    {
        return AuthTag(_tag);
    }

private:
    void releaseKey()
    {
        if (_key)
            BCryptDestroyKey(_key);
        _key = nullptr;
        _object.clear();
        secureWipe(_iv.data(), _iv.size());
        secureWipe(_mac.data(), _mac.size());
    }
    bool fail(SecureArray *out)
    {
        out->clear();
        secureWipe(_pending);
        _valid = false;
        releaseKey();
        return false;
    }
    bool crypt(const std::uint8_t *data, ULONG size, char *out, bool final)
    {
        _auth.pbNonce = bytes(_nonce.constData());
        _auth.cbNonce = ULONG(_nonce.size());
        _auth.pbTag   = reinterpret_cast<PUCHAR>(_tag.data());
        _auth.cbTag   = ULONG(_tag.size());
        // Decryption is a single authenticated operation, encryption is chained.
        _auth.pbMacContext = _encrypt ? _mac.data() : nullptr;
        _auth.cbMacContext = _encrypt ? ULONG(_mac.size()) : 0;
        if (final)
            _auth.dwFlags &= ~BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;
        else
            _auth.dwFlags |= BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG;
        ULONG      written = 0;
        const auto status  = _encrypt
            ? BCryptEncrypt(_key,
                            bytes(data),
                            size,
                            &_auth,
                            _iv.data(),
                            ULONG(_iv.size()),
                            reinterpret_cast<PUCHAR>(out),
                            size,
                            &written,
                            0)
            : BCryptDecrypt(
                  _key, bytes(data), size, &_auth, nullptr, 0, reinterpret_cast<PUCHAR>(out), size, &written, 0);
        return ok(status) && written == size;
    }
    BCRYPT_KEY_HANDLE                     _key = nullptr;
    SecureObject                          _object;
    SecureArray                           _nonce, _tag;
    Bytes                                 _pending;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO _auth {};
    std::array<UCHAR, 16>                 _iv {}, _mac {};
    quint64                               _total   = 0;
    bool                                  _encrypt = false, _valid = false;
};

}

class WinCryptoCipherContext : public CipherContext
{
    Q_OBJECT
public:
    // type: "aes128-cbc-pkcs7" and the like.
    WinCryptoCipherContext(int keyBytes, CipherMode mode, bool pkcs7, Provider *p, const QString &type)
        : CipherContext(p, type)
        , _keyBytes(keyBytes)
        , _mode(mode)
        , _pkcs7(pkcs7)
    {
    }

    WinCryptoCipherContext(const WinCryptoCipherContext &other)
        : CipherContext(other)
        , _keyBytes(other._keyBytes)
        , _mode(other._mode)
        , _pkcs7(other._pkcs7)
        , _engine(other._engine ? new ModeEngine(*other._engine) : nullptr)
        , _gcm(other._gcm ? new CngGcm(*other._gcm) : nullptr)
        , _tag(other._tag)
    {
    }

    Context *clone() const override
    {
        return new WinCryptoCipherContext(*this);
    }

    void setup(Direction dir, const SymmetricKey &key, const InitializationVector &iv, const AuthTag &tag) override
    {
        _tag = tag;
        _engine.reset();
        _gcm.reset();
        if (key.size() != _keyBytes)
            return;
        if (_mode == CipherMode::GCM && iv.size() == 12 && tag.size() >= 12 && tag.size() <= 16) {
            _gcm.reset(new CngGcm(cng(provider())->algorithmInfo(Algorithm::AES_GCM), key, iv, tag, dir == Encode));
            return;
        }
        auto aes = CngAes::create(cng(provider())->algorithmInfo(Algorithm::AES), key);
        if (!aes)
            return;
        Bytes ivBytes  = toBytes(iv);
        Bytes tagBytes = toBytes(tag);
        _engine.reset(new ModeEngine(_mode, _pkcs7, dir == Encode, std::move(aes), ivBytes, tagBytes));
        secureWipe(ivBytes);
        secureWipe(tagBytes);
    }

    KeyLength keyLength() const override
    {
        return KeyLength(_keyBytes, _keyBytes, 1);
    }

    int blockSize() const override
    {
        return int(ModeEngine::blockSize(_mode));
    }

    AuthTag tag() const override
    {
        return _tag;
    }

    bool update(const SecureArray &in, SecureArray *out) override
    {
        out->clear();
        if (_gcm)
            return _gcm->update(in, out);
        if (!_engine)
            return false;
        Bytes      result;
        const bool done = _engine->update(
            reinterpret_cast<const std::uint8_t *>(in.constData()), std::size_t(qMax(0, in.size())), result);
        out->clear();
        if (done && !result.empty()) {
            out->resize(int(result.size()));
            std::memcpy(out->data(), result.data(), result.size());
        }
        secureWipe(result);
        return done;
    }

    bool final(SecureArray *out) override
    {
        out->clear();
        if (_gcm) {
            const bool done = _gcm->final(out);
            if (done)
                _tag = _gcm->tag();
            return done;
        }
        if (!_engine)
            return false;
        Bytes      result;
        const bool done = _engine->final(result);
        out->clear();
        if (done && !result.empty()) {
            out->resize(int(result.size()));
            std::memcpy(out->data(), result.data(), result.size());
        }
        secureWipe(result);
        if (done && _mode == CipherMode::GCM && !_engine->tag().empty())
            _tag = AuthTag(SecureArray(
                QByteArray(reinterpret_cast<const char *>(_engine->tag().data()), int(_engine->tag().size()))));
        return done;
    }

private:
    int                         _keyBytes;
    CipherMode                  _mode;
    bool                        _pkcs7;
    std::unique_ptr<ModeEngine> _engine;
    std::unique_ptr<CngGcm>     _gcm;
    AuthTag                     _tag;
};

//----------------------------------------------------------------------------
// Provider
//----------------------------------------------------------------------------
namespace {

struct HashType
{
    const char *name;
    Algorithm   hash;
    Algorithm   hmac;
};

// HMAC and PBKDF2 are offered for the hashes that have a CNG HMAC handle.
const HashType hashTypes[] = {
    {"md5", Algorithm::MD5, Algorithm::HmacMD5},
    {"sha1", Algorithm::SHA1, Algorithm::HmacSHA1},
    {"sha256", Algorithm::SHA256, Algorithm::HmacSHA256},
    {"sha384", Algorithm::SHA384, Algorithm::HmacSHA384},
    {"sha512", Algorithm::SHA512, Algorithm::HmacSHA512},
};

struct Sha3Type
{
    const char *name;
    Algorithm   hash;
};

const Sha3Type sha3Types[] = {
    {"sha3_256", Algorithm::SHA3_256},
    {"sha3_384", Algorithm::SHA3_384},
    {"sha3_512", Algorithm::SHA3_512},
};

struct ModeType
{
    const char *name;
    CipherMode  mode;
    bool        pkcs7;
};

const ModeType modeTypes[] = {
    {"ecb", CipherMode::ECB, false},
    {"cbc", CipherMode::CBC, false},
    {"cbc-pkcs7", CipherMode::CBC, true},
    {"cfb", CipherMode::CFB, false},
    {"ofb", CipherMode::OFB, false},
    {"ctr", CipherMode::CTR, false},
    {"gcm", CipherMode::GCM, false},
};

const int aesKeyBits[] = {128, 192, 256};

QString aesType(int bits, const ModeType &mode)
{
    return QStringLiteral("aes%1-%2").arg(bits).arg(QLatin1String(mode.name));
}

}

QStringList WinCryptoProvider::features() const
{
    QStringList list;
    list += QStringLiteral("random");
    list += QStringLiteral("cert");
    list += QStringLiteral("tls");
    list += QStringLiteral("dtls");
    list += QStringLiteral("dtls-srtp");
    list += QStringLiteral("rsa");
    list += QStringLiteral("pkey");
    for (const auto &type : hashTypes)
        list += QLatin1String(type.name);
    for (const auto &type : sha3Types)
        if (handle(type.hash))
            list += QLatin1String(type.name);
    for (const auto &type : hashTypes)
        list += QStringLiteral("hmac(%1)").arg(QLatin1String(type.name));
    list += QStringLiteral("pbkdf2(sha1)");
    list += QStringLiteral("pbkdf2(sha256)");
    list += QStringLiteral("pbkdf2(sha384)");
    list += QStringLiteral("pbkdf2(sha512)");
    list += QStringLiteral("hkdf(sha256)");
    for (int bits : aesKeyBits)
        for (const auto &mode : modeTypes)
            list += aesType(bits, mode);
    return list;
}

Provider::Context *WinCryptoProvider::createContext(const QString &type)
{
    if (type == QLatin1String("rsa"))
        return createRSAContext(this);
    if (type == QLatin1String("pkey"))
        return createPKeyContext(this);
    if (type == QLatin1String("cert"))
        return createCertificateContext(this);
    if (type == QLatin1String("tls"))
        return createTLSContext(this);
    if (type == QLatin1String("dtls"))
        return createTLSContext(this, true);
    if (type == QLatin1String("random"))
        return new WinCryptoRandomContext(this);
    for (const auto &hash : hashTypes) {
        if (type == QLatin1String(hash.name))
            return new WinCryptoHashContext(hash.hash, this, type);
        if (type == QStringLiteral("hmac(%1)").arg(QLatin1String(hash.name)))
            return new WinCryptoHMACContext(hash.hmac, this, type);
    }
    for (const auto &sha3 : sha3Types)
        if (type == QLatin1String(sha3.name) && handle(sha3.hash))
            return new WinCryptoHashContext(sha3.hash, this, type);
    if (type == QLatin1String("pbkdf2(sha1)"))
        return new WinCryptoPbkdf2Context(Algorithm::HmacSHA1, this, type);
    if (type == QLatin1String("pbkdf2(sha256)"))
        return new WinCryptoPbkdf2Context(Algorithm::HmacSHA256, this, type);
    if (type == QLatin1String("pbkdf2(sha384)"))
        return new WinCryptoPbkdf2Context(Algorithm::HmacSHA384, this, type);
    if (type == QLatin1String("pbkdf2(sha512)"))
        return new WinCryptoPbkdf2Context(Algorithm::HmacSHA512, this, type);
    if (type == QLatin1String("hkdf(sha256)"))
        return new WinCryptoHkdfContext(Algorithm::HmacSHA256, this, type);
    for (int bits : aesKeyBits)
        for (const auto &mode : modeTypes)
            if (type == aesType(bits, mode))
                return new WinCryptoCipherContext(bits / 8, mode.mode, mode.pkcs7, this, type);
    return nullptr;
}

}

//----------------------------------------------------------------------------
// Plugin
//----------------------------------------------------------------------------
class qca_wincrypto : public QObject, public QCAPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.affinix.qca.Plugin/1.0")
    Q_INTERFACES(QCAPlugin)
public:
    Provider *createProvider() override
    {
        return new wincryptoQCAPlugin::WinCryptoProvider;
    }
};

#include "qca-wincrypto.moc"
