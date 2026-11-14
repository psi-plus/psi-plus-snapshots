// SPDX-License-Identifier: LGPL-2.1-or-later
#include "native_keys.h"
#include "aesmodes.h"
#include "rsa_codec.h"
#include <QCoreApplication>
#include <QMetaObject>
#include <QUuid>
#include <algorithm>
#include <bcrypt.h>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <wincrypt.h>

using namespace QCA;
namespace wincryptoQCAPlugin {
namespace {
BigInteger operator-(BigInteger a, const BigInteger &b)
{
    return a -= b;
}
BigInteger operator*(BigInteger a, const BigInteger &b)
{
    return a *= b;
}
BigInteger operator/(BigInteger a, const BigInteger &b)
{
    return a /= b;
}
BigInteger operator%(BigInteger a, const BigInteger &b)
{
    return a %= b;
}
struct NativeKey
{
    NCRYPT_KEY_HANDLE value = 0;
    ~NativeKey()
    {
        if (value)
            NCryptFreeObject(value);
    }
};
SecureArray copyBytes(const char *data, int size)
{
    SecureArray result(size);
    if (size)
        std::memcpy(result.data(), data, std::size_t(size));
    return result;
}
SecureArray exportNative(NCRYPT_KEY_HANDLE key, LPCWSTR format)
{
    DWORD size = 0;
    if (!key || NCryptExportKey(key, 0, format, nullptr, nullptr, 0, &size, 0) != ERROR_SUCCESS || !size ||
        size > 65536)
        return {};
    SecureArray result {int(size)};
    if (NCryptExportKey(key, 0, format, nullptr, reinterpret_cast<PBYTE>(result.data()), size, &size, 0) !=
            ERROR_SUCCESS ||
        size > DWORD(result.size()))
        return {};
    result.resize(int(size));
    return result;
}
std::shared_ptr<NativeKey> importNative(const SecureArray &blob, LPCWSTR format, bool privateKey)
{
    if (blob.isEmpty() || blob.size() > 65536)
        return {};
    NCRYPT_PROV_HANDLE provider = 0;
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
        return {};
    auto  key    = std::make_shared<NativeKey>();
    auto  status = NCryptImportKey(provider,
                                   0,
                                   format,
                                   nullptr,
                                   &key->value,
                                   reinterpret_cast<PBYTE>(const_cast<char *>(blob.constData())),
                                   DWORD(blob.size()),
                                   NCRYPT_SILENT_FLAG | (privateKey ? NCRYPT_DO_NOT_FINALIZE_FLAG : 0));
    DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    if (!status && privateKey)
        status = NCryptSetProperty(
            key->value, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), 0);
    if (!status && privateKey)
        status = NCryptFinalizeKey(key->value, NCRYPT_SILENT_FLAG);
    NCryptFreeObject(provider);
    return status ? std::shared_ptr<NativeKey>() : key;
}
SecureArray encryptEmpty(NCRYPT_KEY_HANDLE key, int size, bool oaep)
{
    struct Handles
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key       = nullptr;
        ~Handles()
        {
            if (key)
                BCryptDestroyKey(key);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } handles;
    const auto blob = exportNative(key, BCRYPT_RSAPUBLIC_BLOB);
    if (blob.isEmpty() || BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0 ||
        BCryptImportKeyPair(handles.algorithm,
                            nullptr,
                            BCRYPT_RSAPUBLIC_BLOB,
                            &handles.key,
                            reinterpret_cast<PUCHAR>(const_cast<char *>(blob.constData())),
                            ULONG(blob.size()),
                            0) < 0)
        return {};
    // NCrypt rejects zero-byte input, whereas BCrypt implements the standard
    // empty-message encryption. Only public material crosses this bridge.
    BYTE                     empty   = 0;
    ULONG                    written = 0;
    SecureArray              output(size);
    BCRYPT_OAEP_PADDING_INFO padding {BCRYPT_SHA1_ALGORITHM, nullptr, 0};
    if (BCryptEncrypt(handles.key,
                      &empty,
                      0,
                      oaep ? &padding : nullptr,
                      nullptr,
                      0,
                      reinterpret_cast<PUCHAR>(output.data()),
                      ULONG(size),
                      &written,
                      oaep ? BCRYPT_PAD_OAEP : BCRYPT_PAD_PKCS1) < 0 ||
        written != ULONG(size))
        return {};
    return output;
}
BigInteger positiveInteger(const SecureArray &value)
{
    SecureArray positive(value.size() + 1);
    positive[0] = 0;
    if (!value.isEmpty())
        std::memcpy(positive.data() + 1, value.constData(), std::size_t(value.size()));
    return BigInteger(positive);
}
SecureArray unsignedInteger(const BigInteger &value)
{
    if (value <= BigInteger(0))
        return {};
    const auto bytes  = value.toArray();
    int        offset = 0;
    while (offset + 1 < bytes.size() && bytes[offset] == 0)
        ++offset;
    return copyBytes(bytes.constData() + offset, bytes.size() - offset);
}
bool nativeParts(NCRYPT_KEY_HANDLE key, bool privateKey, RsaParts *parts)
{
    const auto blob = exportNative(key, privateKey ? BCRYPT_RSAFULLPRIVATE_BLOB : BCRYPT_RSAPUBLIC_BLOB);
    if (blob.size() < int(sizeof(BCRYPT_RSAKEY_BLOB)))
        return false;
    BCRYPT_RSAKEY_BLOB header;
    std::memcpy(&header, blob.constData(), sizeof(header));
    if (header.Magic != (privateKey ? BCRYPT_RSAFULLPRIVATE_MAGIC : BCRYPT_RSAPUBLIC_MAGIC))
        return false;
    std::size_t offset = sizeof(header);
    const auto  take   = [&](ULONG size, SecureArray *out) {
        if (size > std::size_t(blob.size()) - offset)
            return false;
        *out = copyBytes(blob.constData() + offset, int(size));
        offset += size;
        return true;
    };
    RsaParts result;
    if (!take(header.cbPublicExp, &result.e) || !take(header.cbModulus, &result.n))
        return false;
    if (privateKey &&
        (!take(header.cbPrime1, &result.p) || !take(header.cbPrime2, &result.q) || !take(header.cbPrime1, &result.dp) ||
         !take(header.cbPrime2, &result.dq) || !take(header.cbPrime1, &result.qi) ||
         !take(header.cbModulus, &result.d)))
        return false;
    if (offset != std::size_t(blob.size()))
        return false;
    *parts = result;
    return true;
}
BigInteger gcd(BigInteger a, BigInteger b)
{
    while (b != BigInteger(0)) {
        const auto remainder = a % b;
        a                    = b;
        b                    = remainder;
    }
    return a;
}
std::shared_ptr<NativeKey> importParts(const RsaParts &parts, bool privateKey)
{
    if (parts.n.size() < 64 || parts.n.size() > 2048 || parts.e.isEmpty() || parts.e.size() > 8)
        return {};
    const auto n = positiveInteger(parts.n), e = positiveInteger(parts.e);
    if (e <= BigInteger(1) || e >= n || e % BigInteger(2) == BigInteger(0) || n % BigInteger(2) == BigInteger(0))
        return {};
    if (privateKey) {
        if (parts.p.isEmpty() || parts.q.isEmpty() || parts.d.isEmpty() || parts.p.size() > parts.n.size() ||
            parts.q.size() > parts.n.size() || parts.d.size() > parts.n.size())
            return {};
        const auto p = positiveInteger(parts.p), q = positiveInteger(parts.q), d = positiveInteger(parts.d);
        if (p <= BigInteger(2) || q <= BigInteger(2) || p == q || p * q != n || d >= n)
            return {};
        const auto p1 = p - BigInteger(1), q1 = q - BigInteger(1);
        const auto lambda = p1 / gcd(p1, q1) * q1;
        if (e * d % lambda != BigInteger(1))
            return {};
        if ((!parts.dp.isEmpty() && positiveInteger(parts.dp) != d % p1) ||
            (!parts.dq.isEmpty() && positiveInteger(parts.dq) != d % q1) ||
            (!parts.qi.isEmpty() &&
             (positiveInteger(parts.qi) >= p || positiveInteger(parts.qi) * q % p != BigInteger(1))))
            return {};
    }
    // CNG derives CRT values from n/e/p/q; retain its validated representation.
    const auto size = sizeof(BCRYPT_RSAKEY_BLOB) + parts.e.size() + parts.n.size() +
        (privateKey ? parts.p.size() + parts.q.size() : 0);
    SecureArray blob {int(size)};
    unsigned    first   = uchar(parts.n[0]);
    int         leading = 0;
    while (leading < 8 && !(first & (0x80u >> leading)))
        ++leading;
    if (parts.n[0] == 0 || parts.n.size() * 8 - leading < 512)
        return {};
    BCRYPT_RSAKEY_BLOB header {ULONG(privateKey ? BCRYPT_RSAPRIVATE_MAGIC : BCRYPT_RSAPUBLIC_MAGIC),
                               ULONG(parts.n.size() * 8 - leading),
                               ULONG(parts.e.size()),
                               ULONG(parts.n.size()),
                               privateKey ? ULONG(parts.p.size()) : 0,
                               privateKey ? ULONG(parts.q.size()) : 0};
    std::memcpy(blob.data(), &header, sizeof(header));
    int offset = sizeof(header);
    for (const auto *part : {&parts.e, &parts.n, &parts.p, &parts.q}) {
        if (!privateKey && part != &parts.e && part != &parts.n)
            break;
        if (!part->isEmpty())
            std::memcpy(blob.data() + offset, part->constData(), std::size_t(part->size()));
        offset += part->size();
    }
    return importNative(blob, privateKey ? BCRYPT_RSAPRIVATE_BLOB : BCRYPT_RSAPUBLIC_BLOB, privateKey);
}
std::shared_ptr<NativeKey> generateRSA(int bits, int exponent)
{
    if (bits < 2048 || bits > 8192 || bits % 64 || exponent != 65537)
        return {};
    NCRYPT_PROV_HANDLE provider = 0;
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
        return {};
    auto  key    = std::make_shared<NativeKey>();
    auto  status = NCryptCreatePersistedKey(provider, &key->value, NCRYPT_RSA_ALGORITHM, nullptr, 0, 0);
    DWORD length = DWORD(bits);
    // This unnamed key stays in memory. QCA serialization and the Schannel
    // credential bridge export into protected temporary buffers.
    DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    if (!status)
        status =
            NCryptSetProperty(key->value, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&length), sizeof(length), 0);
    if (!status)
        status = NCryptSetProperty(
            key->value, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), 0);
    if (!status)
        status = NCryptFinalizeKey(key->value, NCRYPT_SILENT_FLAG);
    NCryptFreeObject(provider);
    return status ? std::shared_ptr<NativeKey>() : key;
}

class WinRSA : public RSAContext
{
    enum class Operation
    {
        Idle,
        Sign,
        Verify
    };

public:
    explicit WinRSA(Provider *provider)
        : RSAContext(provider)
    {
    }
    WinRSA(const WinRSA &other)
        : RSAContext(other)
        , _key(other._key)
        , _public(other._public)
        , _hash(other._hash ? std::make_unique<Hash>(*other._hash) : nullptr)
        , _raw(other._raw)
        , _signature(other._signature)
        , _operation(other._operation)
    {
    }
    ~WinRSA() override
    {
        if (_worker.joinable())
            _worker.join();
    }
    Context *clone() const override
    {
        return new WinRSA(*this);
    }
    bool isNull() const override
    {
        return !_key;
    }
    PKey::Type type() const override
    {
        return PKey::RSA;
    }
    bool isPrivate() const override
    {
        return _key && !_public;
    }
    bool canExport() const override
    {
        return bool(_key);
    }
    void convertToPublic() override
    {
        clearOperation();
        if (_public || !_key)
            return;
        _key    = importNative(exportNative(_key->value, BCRYPT_RSAPUBLIC_BLOB), BCRYPT_RSAPUBLIC_BLOB, false);
        _public = true;
    }
    int bits() const override
    {
        DWORD value = 0, written = 0;
        return _key &&
                NCryptGetProperty(
                    _key->value, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&value), sizeof(value), &written, 0) ==
                    ERROR_SUCCESS
            ? int(value)
            : 0;
    }
    void createPrivate(int bits, int exponent, bool block) override
    {
        clearKey();
        if (block)
            _key = generateRSA(bits, exponent);
        else
            _worker = std::thread([this, bits, exponent] {
                _key = generateRSA(bits, exponent);
                QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
            });
    }
    void createPrivate(const BigInteger &n,
                       const BigInteger &e,
                       const BigInteger &p,
                       const BigInteger &q,
                       const BigInteger &d) override
    {
        clearKey();
        RsaParts parts;
        parts.n = unsignedInteger(n);
        parts.e = unsignedInteger(e);
        parts.p = unsignedInteger(p);
        parts.q = unsignedInteger(q);
        parts.d = unsignedInteger(d);
        _key    = importParts(parts, true);
    }
    void createPublic(const BigInteger &n, const BigInteger &e) override
    {
        clearKey();
        RsaParts parts;
        parts.n = unsignedInteger(n);
        parts.e = unsignedInteger(e);
        _key    = importParts(parts, false);
        _public = true;
    }
    BigInteger n() const override
    {
        return component(0);
    }
    BigInteger e() const override
    {
        return component(1);
    }
    BigInteger p() const override
    {
        return component(2);
    }
    BigInteger q() const override
    {
        return component(3);
    }
    BigInteger d() const override
    {
        return component(4);
    }
    int maximumEncryptSize(EncryptionAlgorithm algorithm) const override
    {
        const int size = (bits() + 7) / 8;
        if (!size)
            return 0;
        switch (algorithm) {
        case EME_PKCS1v15:
            return std::max(0, size - 11);
        case EME_PKCS1_OAEP:
            return isPrivate() ? 0 : std::max(0, size - 42);
        case EME_NO_PADDING:
            return size;
        default:
            return 0;
        }
    }
    SecureArray encrypt(const SecureArray &input, EncryptionAlgorithm algorithm) override
    {
        const int size = (bits() + 7) / 8;
        if (!_key || input.size() > maximumEncryptSize(algorithm) ||
            (algorithm == EME_NO_PADDING && input.size() != size))
            return {};
        if (algorithm == EME_PKCS1v15 && isPrivate())
            return sign(input, nullptr, 0, false);
        if (algorithm != EME_PKCS1v15 && algorithm != EME_PKCS1_OAEP && algorithm != EME_NO_PADDING)
            return {};
        if (algorithm == EME_PKCS1_OAEP && isPrivate())
            return {};
        if (input.isEmpty())
            return encryptEmpty(_key->value, size, algorithm == EME_PKCS1_OAEP);
        BCRYPT_OAEP_PADDING_INFO padding {BCRYPT_SHA1_ALGORITHM, nullptr, 0};
        const DWORD              flags = algorithm == EME_PKCS1v15 ? NCRYPT_PAD_PKCS1_FLAG
            : algorithm == EME_PKCS1_OAEP                          ? NCRYPT_PAD_OAEP_FLAG
                                                                   : NCRYPT_NO_PADDING_FLAG;
        SecureArray              output(size);
        DWORD                    written   = 0;
        const auto               operation = algorithm == EME_NO_PADDING && isPrivate() ? NCryptDecrypt : NCryptEncrypt;
        if (operation(_key->value,
                      mutableBytes(input),
                      DWORD(input.size()),
                      algorithm == EME_PKCS1_OAEP ? &padding : nullptr,
                      reinterpret_cast<PBYTE>(output.data()),
                      DWORD(size),
                      &written,
                      flags | NCRYPT_SILENT_FLAG) != ERROR_SUCCESS ||
            written != DWORD(size))
            return {};
        return output;
    }
    bool decrypt(const SecureArray &input, SecureArray *output, EncryptionAlgorithm algorithm) override
    {
        if (!output)
            return false;
        // Keep the input alive if the caller uses the same SecureArray for both.
        const SecureArray saved = input;
        output->clear();
        const int size = (bits() + 7) / 8;
        if (!_key || saved.size() != size ||
            (algorithm != EME_PKCS1v15 && algorithm != EME_PKCS1_OAEP && algorithm != EME_NO_PADDING) ||
            (algorithm == EME_PKCS1_OAEP && !isPrivate()))
            return false;
        BCRYPT_OAEP_PADDING_INFO padding {BCRYPT_SHA1_ALGORITHM, nullptr, 0};
        const DWORD              flags = !isPrivate() || algorithm == EME_NO_PADDING ? NCRYPT_NO_PADDING_FLAG
            : algorithm == EME_PKCS1v15                                              ? NCRYPT_PAD_PKCS1_FLAG
                                                                                     : NCRYPT_PAD_OAEP_FLAG;
        SecureArray              plain(size);
        DWORD                    written   = 0;
        const auto               operation = isPrivate() ? NCryptDecrypt : NCryptEncrypt;
        if (operation(_key->value,
                      mutableBytes(saved),
                      DWORD(saved.size()),
                      algorithm == EME_PKCS1_OAEP ? &padding : nullptr,
                      reinterpret_cast<PBYTE>(plain.data()),
                      DWORD(size),
                      &written,
                      flags | NCRYPT_SILENT_FLAG) != ERROR_SUCCESS ||
            written > DWORD(size))
            return false;
        plain.resize(int(written));
        if (!isPrivate() && algorithm == EME_PKCS1v15) {
            // QCA's legacy public "decrypt" recovers a type-1 signature block.
            // It contains public data; private type-2 unpadding stays in CNG.
            if (plain.size() < 11 || plain[0] || plain[1] != 1)
                return false;
            int offset = 2;
            while (offset < plain.size() && uchar(plain[offset]) == 0xff)
                ++offset;
            if (offset < 10 || offset >= plain.size() || plain[offset])
                return false;
            plain = copyBytes(plain.constData() + offset + 1, plain.size() - offset - 1);
        }
        *output = plain;
        return true;
    }
    void startSign(SignatureAlgorithm algorithm, SignatureFormat) override
    {
        start(algorithm, Operation::Sign);
    }
    void startVerify(SignatureAlgorithm algorithm, SignatureFormat) override
    {
        start(algorithm, Operation::Verify);
    }
    void update(const MemoryRegion &input) override
    {
        if (_operation == Operation::Idle)
            return;
        if (_hash)
            _hash->update(input);
        else {
            const auto limit = std::size_t(maximumEncryptSize(EME_PKCS1v15));
            if (std::size_t(input.size()) > limit - _raw.size()) {
                clearOperation();
                return;
            }
            if (input.size())
                _raw.insert(_raw.end(), input.constData(), input.constData() + input.size());
        }
    }
    QByteArray endSign() override
    {
        if (_operation != Operation::Sign) {
            clearOperation();
            return {};
        }
        const auto digest = finalDigest();
        const auto result =
            sign(digest, digestName(_signature.digest), saltLength(), _signature.scheme == SignatureScheme::RSA_PSS);
        clearOperation();
        return result.toByteArray();
    }
    bool endVerify(const QByteArray &signature) override
    {
        if (_operation != Operation::Verify || signature.size() != (bits() + 7) / 8) {
            clearOperation();
            return false;
        }
        const auto digest = finalDigest();
        if (_signature.digest == SignatureDigest::None && digest.isEmpty()) {
            // NCrypt's raw signature API rejects zero bytes. Compare the
            // standard empty type-1 block using the native public primitive.
            SecureArray block((bits() + 7) / 8);
            DWORD       written = 0;
            const bool  ok      = NCryptEncrypt(_key->value,
                                                reinterpret_cast<PBYTE>(const_cast<char *>(signature.constData())),
                                                DWORD(signature.size()),
                                                nullptr,
                                                reinterpret_cast<PBYTE>(block.data()),
                                                DWORD(block.size()),
                                                &written,
                                                NCRYPT_NO_PADDING_FLAG | NCRYPT_SILENT_FLAG) == ERROR_SUCCESS &&
                written == DWORD(block.size());
            const auto expected   = emptySignatureBlock(block.size());
            unsigned   difference = 0;
            for (int i = 0; i < block.size(); ++i)
                difference |= uchar(block[i]) ^ uchar(expected[i]);
            clearOperation();
            return ok && !difference;
        }
        BCRYPT_PKCS1_PADDING_INFO pkcs {digestName(_signature.digest)};
        BCRYPT_PSS_PADDING_INFO   pss {digestName(_signature.digest), DWORD(saltLength())};
        const bool                usePss = _signature.scheme == SignatureScheme::RSA_PSS;
        const auto                status =
            NCryptVerifySignature(_key->value,
                                  usePss ? static_cast<void *>(&pss) : &pkcs,
                                  mutableBytes(digest),
                                  DWORD(digest.size()),
                                  reinterpret_cast<PBYTE>(const_cast<char *>(signature.constData())),
                                  DWORD(signature.size()),
                                  (usePss ? NCRYPT_PAD_PSS_FLAG : NCRYPT_PAD_PKCS1_FLAG) | NCRYPT_SILENT_FLAG);
        clearOperation();
        return status == ERROR_SUCCESS;
    }
    bool setParts(const RsaParts &parts, bool privateKey)
    {
        clearKey();
        _key    = importParts(parts, privateKey);
        _public = !privateKey;
        return bool(_key);
    }
    NCRYPT_KEY_HANDLE nativeHandle() const
    {
        return _key ? _key->value : 0;
    }
    NCRYPT_KEY_HANDLE handle() const
    {
        return isPrivate() ? _key->value : 0;
    }

private:
    static PBYTE mutableBytes(const SecureArray &value)
    {
        // Some native entry points require a valid address for an empty input.
        static BYTE empty = 0;
        return value.isEmpty() ? &empty : reinterpret_cast<PBYTE>(const_cast<char *>(value.constData()));
    }
    static LPCWSTR digestName(SignatureDigest digest)
    {
        switch (digest) {
        case SignatureDigest::MD5:
            return BCRYPT_MD5_ALGORITHM;
        case SignatureDigest::SHA1:
            return BCRYPT_SHA1_ALGORITHM;
        case SignatureDigest::SHA256:
            return BCRYPT_SHA256_ALGORITHM;
        case SignatureDigest::SHA384:
            return BCRYPT_SHA384_ALGORITHM;
        case SignatureDigest::SHA512:
            return BCRYPT_SHA512_ALGORITHM;
        default:
            return nullptr;
        }
    }
    static QString hashName(SignatureDigest digest)
    {
        switch (digest) {
        case SignatureDigest::MD5:
            return QStringLiteral("md5");
        case SignatureDigest::SHA1:
            return QStringLiteral("sha1");
        case SignatureDigest::SHA256:
            return QStringLiteral("sha256");
        case SignatureDigest::SHA384:
            return QStringLiteral("sha384");
        case SignatureDigest::SHA512:
            return QStringLiteral("sha512");
        default:
            return {};
        }
    }
    static int digestLength(SignatureDigest digest)
    {
        switch (digest) {
        case SignatureDigest::MD5:
            return 16;
        case SignatureDigest::SHA1:
            return 20;
        case SignatureDigest::SHA256:
            return 32;
        case SignatureDigest::SHA384:
            return 48;
        case SignatureDigest::SHA512:
            return 64;
        default:
            return 0;
        }
    }
    int saltLength() const
    {
        return _signature.saltLength < 0 ? digestLength(_signature.digest) : _signature.saltLength;
    }
    void start(SignatureAlgorithm algorithm, Operation operation)
    {
        clearOperation();
        if (!_key || (operation == Operation::Sign && !isPrivate()) ||
            (algorithm.scheme != SignatureScheme::RSA_PKCS1v15 && algorithm.scheme != SignatureScheme::RSA_PSS))
            return;
        if (algorithm.digest != SignatureDigest::None && !digestName(algorithm.digest))
            return;
        if (algorithm.scheme == SignatureScheme::RSA_PKCS1v15 &&
            (algorithm.mgfDigest != SignatureDigest::Unknown || algorithm.saltLength != -1 ||
             algorithm.trailerField != -1))
            return;
        if (algorithm.scheme == SignatureScheme::RSA_PSS &&
            (algorithm.digest == SignatureDigest::None || algorithm.digest == SignatureDigest::MD5 ||
             algorithm.saltLength < -1 || (algorithm.trailerField != -1 && algorithm.trailerField != 1) ||
             (algorithm.mgfDigest != SignatureDigest::Unknown && algorithm.mgfDigest != algorithm.digest)))
            return;
        _signature = algorithm;
        if (algorithm.digest != SignatureDigest::None) {
            const auto name = hashName(algorithm.digest);
            if (!isSupported(QStringList {name}, provider()->name()))
                return;
            _hash = std::make_unique<Hash>(name, provider()->name());
        }
        if (algorithm.scheme == SignatureScheme::RSA_PSS &&
            saltLength() > (bits() + 6) / 8 - digestLength(algorithm.digest) - 2) {
            clearOperation();
            return;
        }
        _operation = operation;
    }
    SecureArray finalDigest()
    {
        return _hash ? SecureArray(_hash->final())
                     : copyBytes(reinterpret_cast<const char *>(_raw.data()), int(_raw.size()));
    }
    SecureArray sign(const SecureArray &digest, LPCWSTR algorithm, int salt, bool pss) const
    {
        if (!isPrivate())
            return {};
        if (!pss && !algorithm && digest.isEmpty()) {
            const auto  block = emptySignatureBlock((bits() + 7) / 8);
            SecureArray output(block.size());
            DWORD       written = 0;
            if (NCryptDecrypt(_key->value,
                              mutableBytes(block),
                              DWORD(block.size()),
                              nullptr,
                              reinterpret_cast<PBYTE>(output.data()),
                              DWORD(output.size()),
                              &written,
                              NCRYPT_NO_PADDING_FLAG | NCRYPT_SILENT_FLAG) != ERROR_SUCCESS ||
                written != DWORD(output.size()))
                return {};
            return output;
        }
        BCRYPT_PKCS1_PADDING_INFO pkcs {algorithm};
        BCRYPT_PSS_PADDING_INFO   padding {algorithm, DWORD(salt)};
        SecureArray               output((bits() + 7) / 8);
        DWORD                     written = 0;
        if (NCryptSignHash(_key->value,
                           pss ? static_cast<void *>(&padding) : &pkcs,
                           mutableBytes(digest),
                           DWORD(digest.size()),
                           reinterpret_cast<PBYTE>(output.data()),
                           DWORD(output.size()),
                           &written,
                           (pss ? NCRYPT_PAD_PSS_FLAG : NCRYPT_PAD_PKCS1_FLAG) | NCRYPT_SILENT_FLAG) != ERROR_SUCCESS ||
            written != DWORD(output.size()))
            return {};
        return output;
    }
    static SecureArray emptySignatureBlock(int size)
    {
        SecureArray block(size, char(0xff));
        block[0]        = 0;
        block[1]        = 1;
        block[size - 1] = 0;
        return block;
    }
    void clearOperation()
    {
        _hash.reset();
        secureWipe(_raw);
        _operation = Operation::Idle;
    }
    void clearKey()
    {
        if (_worker.joinable())
            _worker.join();
        _key.reset();
        _public = false;
        clearOperation();
    }
    BigInteger component(int index) const
    {
        if (!_key || (index >= 2 && !isPrivate()))
            return {};
        RsaParts parts;
        if (!nativeParts(_key->value, index >= 2, &parts))
            return {};
        const SecureArray *components[] {&parts.n, &parts.e, &parts.p, &parts.q, &parts.d};
        return positiveInteger(*components[index]);
    }
    std::shared_ptr<NativeKey> _key;
    bool                       _public = false;
    std::thread                _worker;
    std::unique_ptr<Hash>      _hash;
    Bytes                      _raw;
    SignatureAlgorithm         _signature;
    Operation                  _operation = Operation::Idle;
};

class WinPKey : public PKeyContext
{
public:
    explicit WinPKey(Provider *provider)
        : PKeyContext(provider)
    {
    }
    WinPKey(const WinPKey &other)
        : PKeyContext(other)
        , _key(other._key ? static_cast<PKeyBase *>(other._key->clone()) : nullptr)
    {
    }
    Context *clone() const override
    {
        return new WinPKey(*this);
    }
    QList<PKey::Type> supportedTypes() const override
    {
        return {PKey::RSA};
    }
    QList<PKey::Type> supportedIOTypes() const override
    {
        return {PKey::RSA};
    }
    QList<PBEAlgorithm> supportedPBEAlgorithms() const override
    {
        return {PBES2_TripleDES_SHA1, PBES2_AES128_SHA1, PBES2_AES192_SHA1, PBES2_AES256_SHA1};
    }
    PKeyBase *key() override
    {
        return _key.get();
    }
    const PKeyBase *key() const override
    {
        return _key.get();
    }
    void setKey(PKeyBase *key) override
    {
        _key.reset(key);
    }
    bool importKey(const PKeyBase *key) override
    {
        if (key && key == _key.get())
            return !key->isNull();
        _key.reset();
        if (!key || key->isNull() || key->type() != PKey::RSA || !key->canExport())
            return false;
        const auto *native = dynamic_cast<const WinRSA *>(key);
        if (native)
            _key.reset(static_cast<PKeyBase *>(native->clone()));
        else {
            const auto *rsa = dynamic_cast<const RSAContext *>(key);
            if (!rsa)
                return false;
            auto result = std::make_unique<WinRSA>(provider());
            if (rsa->isPrivate())
                result->createPrivate(rsa->n(), rsa->e(), rsa->p(), rsa->q(), rsa->d());
            else
                result->createPublic(rsa->n(), rsa->e());
            _key = std::move(result);
        }
        return !_key->isNull();
    }
    QByteArray publicToDER() const override
    {
        return publicDER().toByteArray();
    }
    QString publicToPEM() const override
    {
        return encodeKeyPEM(publicDER(), QStringLiteral("PUBLIC KEY"));
    }
    ConvertResult publicFromDER(const QByteArray &der) override
    {
        _key.reset();
        RsaParts parts;
        return decodePublicRSA(MemoryRegion(der), &parts) && install(parts, false) ? ConvertGood : ErrorDecode;
    }
    ConvertResult publicFromPEM(const QString &pem) override
    {
        _key.reset();
        RsaParts    parts;
        SecureArray der;
        return decodeKeyPEM(pem, false, &der) && decodePublicRSA(der, &parts) && install(parts, false) ? ConvertGood
                                                                                                       : ErrorDecode;
    }
    SecureArray privateToDER(const SecureArray &passphrase, PBEAlgorithm algorithm) const override
    {
        const auto *native = dynamic_cast<const WinRSA *>(_key.get());
        if (!native || !native->isPrivate())
            return {};
        return protectPrivateRSA(
            exportNative(native->handle(), NCRYPT_PKCS8_PRIVATE_KEY_BLOB), passphrase, algorithm, provider()->name());
    }
    QString privateToPEM(const SecureArray &passphrase, PBEAlgorithm algorithm) const override
    {
        return encodeKeyPEM(privateToDER(passphrase, algorithm),
                            passphrase.isEmpty() ? QStringLiteral("PRIVATE KEY")
                                                 : QStringLiteral("ENCRYPTED PRIVATE KEY"));
    }
    ConvertResult privateFromDER(const SecureArray &der, const SecureArray &passphrase) override
    {
        _key.reset();
        RsaParts   parts;
        const auto result = decodePrivateRSA(der, passphrase, provider()->name(), &parts);
        if (result != ConvertGood)
            return result;
        return install(parts, true) ? ConvertGood : ErrorDecode;
    }
    ConvertResult privateFromPEM(const QString &pem, const SecureArray &passphrase) override
    {
        _key.reset();
        SecureArray der;
        return decodeKeyPEM(pem, true, &der) ? privateFromDER(der, passphrase) : ErrorDecode;
    }

private:
    SecureArray publicDER() const
    {
        const auto *native = dynamic_cast<const WinRSA *>(_key.get());
        RsaParts    parts;
        return native && nativeParts(native->nativeHandle(), false, &parts) ? encodePublicRSA(parts) : SecureArray();
    }
    bool install(const RsaParts &parts, bool privateKey)
    {
        auto key = std::make_unique<WinRSA>(provider());
        if (!key->setParts(parts, privateKey))
            return false;
        _key = std::move(key);
        return true;
    }
    std::unique_ptr<PKeyBase> _key;
};

QByteArray encode(LPCSTR type, const void *value)
{
    DWORD length = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, nullptr, &length) || length > 1024 * 1024)
        return {};
    QByteArray result(int(length), 0);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, result.data(), &length))
        return {};
    result.resize(int(length));
    return result;
}
FILETIME fileTime(const QDateTime &date)
{
    ULARGE_INTEGER value;
    value.QuadPart = quint64(date.toMSecsSinceEpoch() + 11644473600000LL) * 10000;
    return {value.LowPart, value.HighPart};
}
} // namespace

Provider::Context *createRSAContext(Provider *provider)
{
    return new WinRSA(provider);
}
Provider::Context *createPKeyContext(Provider *provider)
{
    return new WinPKey(provider);
}
NCRYPT_KEY_HANDLE nativePrivateKey(const PKeyContext &key)
{
    const auto *rsa = dynamic_cast<const WinRSA *>(key.key());
    return rsa ? rsa->handle() : 0;
}

CredentialCertificate::~CredentialCertificate()
{
    if (certificate)
        CertFreeCertificateContext(certificate);
    if (store)
        CertCloseStore(store, 0);
    if (!removeTemporaryKey())
        NCryptFreeObject(temporaryKey);
}
bool CredentialCertificate::removeTemporaryKey()
{
    if (!temporaryKey)
        return true;
    if (NCryptDeleteKey(temporaryKey, NCRYPT_SILENT_FLAG) != ERROR_SUCCESS)
        return false;
    temporaryKey = 0;
    return true;
}
std::unique_ptr<CredentialCertificate> createCredentialCertificate(const CertificateChain &chain, const PrivateKey &key)
{
    if (chain.isEmpty() || key.isNull())
        return {};
    const auto handle = nativePrivateKey(*static_cast<const PKeyContext *>(key.context()));
    if (!handle)
        return {};
    auto identity   = std::make_unique<CredentialCertificate>();
    identity->store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!identity->store)
        return {};
    for (int i = 0; i < chain.size(); ++i) {
        const auto der = chain[i].toDER();
        if (!CertAddEncodedCertificateToStore(identity->store,
                                              X509_ASN_ENCODING,
                                              reinterpret_cast<const BYTE *>(der.constData()),
                                              DWORD(der.size()),
                                              CERT_STORE_ADD_ALWAYS,
                                              i == 0 ? &identity->certificate : nullptr))
            return {};
    }
    DWORD size = 0;
    if (!CryptExportPublicKeyInfo(handle, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, &size) || size > 65536)
        return {};
    std::vector<BYTE> pub(size);
    if (!CryptExportPublicKeyInfo(handle,
                                  CERT_NCRYPT_KEY_SPEC,
                                  X509_ASN_ENCODING,
                                  reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(pub.data()),
                                  &size) ||
        !CertComparePublicKeyInfo(X509_ASN_ENCODING,
                                  reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(pub.data()),
                                  &identity->certificate->pCertInfo->SubjectPublicKeyInfo))
        return {};
    if (NCryptExportKey(handle, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0, &size, 0) != ERROR_SUCCESS ||
        size > 65536)
        return {};
    SecureArray exported {int(size)};
    if (NCryptExportKey(handle,
                        0,
                        NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
                        nullptr,
                        reinterpret_cast<PBYTE>(exported.data()),
                        size,
                        &size,
                        0) != ERROR_SUCCESS)
        return {};
    NCRYPT_PROV_HANDLE provider = 0;
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
        return {};
    const auto   name = QStringLiteral("qca-wincrypto-%1-%2")
                            .arg(QCoreApplication::applicationPid())
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    NCryptBuffer buffer {
        DWORD((name.size() + 1) * sizeof(wchar_t)), NCRYPTBUFFER_PKCS_KEY_NAME, const_cast<ushort *>(name.utf16())};
    NCryptBufferDesc parameters {NCRYPTBUFFER_VERSION, 1, &buffer};
    auto             status = NCryptImportKey(provider,
                                              0,
                                              NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
                                              &parameters,
                                              &identity->temporaryKey,
                                              reinterpret_cast<PBYTE>(exported.data()),
                                              size,
                                              NCRYPT_DO_NOT_FINALIZE_FLAG | NCRYPT_SILENT_FLAG);
    if (!status)
        status = NCryptFinalizeKey(identity->temporaryKey, NCRYPT_SILENT_FLAG);
    NCryptFreeObject(provider);
    exported.clear();
    if (status)
        return {};
    CRYPT_KEY_PROV_INFO info {};
    info.pwszContainerName = const_cast<LPWSTR>(reinterpret_cast<const wchar_t *>(name.utf16()));
    info.pwszProvName      = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    info.dwFlags           = NCRYPT_SILENT_FLAG;
    if (!CertSetCertificateContextProperty(identity->certificate, CERT_KEY_PROV_INFO_PROP_ID, 0, &info))
        return {};
    return identity;
}

QByteArray createSelfSignedCertificate(const CertificateOptions &opts, const PKeyContext &key)
{
    const auto handle = nativePrivateKey(key);
    if (!handle || !opts.notValidBefore().isValid() || !opts.notValidAfter().isValid() ||
        opts.notValidAfter() <= opts.notValidBefore() ||
        opts.notValidBefore().toMSecsSinceEpoch() < -11644473600000LL || opts.serialNumber() <= BigInteger(0) ||
        !opts.policies().isEmpty() || !opts.crlLocations().isEmpty() || !opts.issuerLocations().isEmpty() ||
        !opts.ocspLocations().isEmpty())
        return {};
    const auto                 info = opts.infoOrdered().dnOnly();
    std::vector<QByteArray>    names, values;
    std::vector<CERT_RDN_ATTR> attrs;
    std::vector<CERT_RDN>      rdns;
    names.reserve(info.size());
    values.reserve(info.size());
    attrs.reserve(info.size());
    rdns.reserve(info.size());
    for (const auto &item : info) {
        if (item.value().contains(QChar(0)))
            return {};
        names.push_back(item.type().id().toLatin1());
        const auto text = item.value();
        values.emplace_back(reinterpret_cast<const char *>(text.utf16()), int(text.size() * sizeof(wchar_t)));
        attrs.push_back({names.back().data(),
                         CERT_RDN_UTF8_STRING,
                         {DWORD(values.back().size()), reinterpret_cast<PBYTE>(values.back().data())}});
        rdns.push_back({1, &attrs.back()});
    }
    CERT_NAME_INFO name {DWORD(rdns.size()), rdns.data()};
    auto           subject = encode(X509_UNICODE_NAME, &name);
    if (subject.isEmpty())
        return {};
    auto       serialSecure = opts.serialNumber().toArray();
    QByteArray serial(serialSecure.constData(), serialSecure.size());
    while (serial.size() > 1 && serial[0] == 0)
        serial.remove(0, 1);
    if (serial.size() > 20)
        return {};
    std::reverse(serial.begin(), serial.end());
    DWORD pubSize = 0;
    if (!CryptExportPublicKeyInfo(handle, CERT_NCRYPT_KEY_SPEC, X509_ASN_ENCODING, nullptr, &pubSize) ||
        pubSize > 65536)
        return {};
    std::vector<BYTE> publicInfo(pubSize);
    if (!CryptExportPublicKeyInfo(handle,
                                  CERT_NCRYPT_KEY_SPEC,
                                  X509_ASN_ENCODING,
                                  reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(publicInfo.data()),
                                  &pubSize))
        return {};
    std::vector<CERT_EXTENSION> extensions;
    std::vector<QByteArray>     extensionValues;
    extensionValues.reserve(4);
    extensions.reserve(4);
    const auto add = [&](const char *oid, LPCSTR type, const void *data, bool critical) {
        auto encoded = encode(type, data);
        if (encoded.isEmpty())
            return false;
        extensionValues.push_back(encoded);
        extensions.push_back({const_cast<char *>(oid),
                              critical,
                              {DWORD(encoded.size()), reinterpret_cast<PBYTE>(extensionValues.back().data())}});
        return true;
    };
    CERT_BASIC_CONSTRAINTS2_INFO basic {
        opts.isCA(), opts.isCA() && opts.pathLimit() >= 0, DWORD(std::max(0, opts.pathLimit()))};
    if (!add(szOID_BASIC_CONSTRAINTS2, X509_BASIC_CONSTRAINTS2, &basic, true))
        return {};
    BYTE                    usageBytes[2] {};
    std::vector<QByteArray> ekuNames;
    for (const auto &constraint : opts.constraints()) {
        if (constraint.section() == ConstraintType::ExtendedKeyUsage)
            ekuNames.push_back(constraint.id().toLatin1());
        else
            switch (constraint.known()) {
            case DigitalSignature:
                usageBytes[0] |= CERT_DIGITAL_SIGNATURE_KEY_USAGE;
                break;
            case NonRepudiation:
                usageBytes[0] |= CERT_NON_REPUDIATION_KEY_USAGE;
                break;
            case KeyEncipherment:
                usageBytes[0] |= CERT_KEY_ENCIPHERMENT_KEY_USAGE;
                break;
            case DataEncipherment:
                usageBytes[0] |= CERT_DATA_ENCIPHERMENT_KEY_USAGE;
                break;
            case KeyAgreement:
                usageBytes[0] |= CERT_KEY_AGREEMENT_KEY_USAGE;
                break;
            case KeyCertificateSign:
                usageBytes[0] |= CERT_KEY_CERT_SIGN_KEY_USAGE;
                break;
            case CRLSign:
                usageBytes[0] |= CERT_CRL_SIGN_KEY_USAGE;
                break;
            default:
                return {};
            }
    }
    if (usageBytes[0]) {
        CRYPT_BIT_BLOB usage {1, usageBytes, 0};
        if (!add(szOID_KEY_USAGE, X509_KEY_USAGE, &usage, true))
            return {};
    }
    if (!ekuNames.empty()) {
        std::vector<LPSTR> ids;
        for (auto &id : ekuNames)
            ids.push_back(id.data());
        CERT_ENHKEY_USAGE eku {DWORD(ids.size()), ids.data()};
        if (!add(szOID_ENHANCED_KEY_USAGE, X509_ENHANCED_KEY_USAGE, &eku, false))
            return {};
    }
    std::vector<CERT_ALT_NAME_ENTRY> alternatives;
    std::vector<QString>             dnsNames;
    std::vector<QByteArray>          xmppNames;
    std::vector<CERT_OTHER_NAME>     otherNames;
    const auto                       allInfo = opts.infoOrdered();
    alternatives.reserve(allInfo.size());
    dnsNames.reserve(allInfo.size());
    xmppNames.reserve(allInfo.size());
    otherNames.reserve(allInfo.size());
    for (const auto &item : allInfo) {
        if (item.type().section() == CertificateInfoType::DN)
            continue;
        CERT_ALT_NAME_ENTRY entry {};
        if (item.value().contains(QChar(0)))
            return {};
        if (item.type() == DNS) {
            dnsNames.push_back(item.value());
            entry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
            entry.pwszDNSName     = reinterpret_cast<LPWSTR>(dnsNames.back().data());
        } else if (item.type() == XMPP) {
            const auto      text = item.value();
            CERT_NAME_VALUE value {
                CERT_RDN_UTF8_STRING,
                {DWORD(text.size() * sizeof(wchar_t)), reinterpret_cast<PBYTE>(const_cast<ushort *>(text.utf16()))}};
            xmppNames.push_back(encode(X509_UNICODE_ANY_STRING, &value));
            if (xmppNames.back().isEmpty())
                return {};
            otherNames.push_back({const_cast<char *>("1.3.6.1.5.5.7.8.5"),
                                  {DWORD(xmppNames.back().size()), reinterpret_cast<PBYTE>(xmppNames.back().data())}});
            entry.dwAltNameChoice = CERT_ALT_NAME_OTHER_NAME;
            entry.pOtherName      = &otherNames.back();
        } else
            return {};
        alternatives.push_back(entry);
    }
    if (!alternatives.empty()) {
        CERT_ALT_NAME_INFO alt {DWORD(alternatives.size()), alternatives.data()};
        if (!add(szOID_SUBJECT_ALT_NAME2, X509_ALTERNATE_NAME, &alt, false))
            return {};
    }
    CERT_INFO cert {};
    cert.dwVersion          = CERT_V3;
    cert.SerialNumber       = {DWORD(serial.size()), reinterpret_cast<PBYTE>(serial.data())};
    cert.SignatureAlgorithm = {const_cast<char *>(szOID_RSA_SHA256RSA), {0, nullptr}};
    cert.Subject = cert.Issuer = {DWORD(subject.size()), reinterpret_cast<PBYTE>(subject.data())};
    cert.NotBefore             = fileTime(opts.notValidBefore());
    cert.NotAfter              = fileTime(opts.notValidAfter());
    cert.SubjectPublicKeyInfo  = *reinterpret_cast<PCERT_PUBLIC_KEY_INFO>(publicInfo.data());
    cert.cExtension            = DWORD(extensions.size());
    cert.rgExtension           = extensions.data();
    DWORD size                 = 0;
    if (!CryptSignAndEncodeCertificate(handle,
                                       CERT_NCRYPT_KEY_SPEC,
                                       X509_ASN_ENCODING,
                                       X509_CERT_TO_BE_SIGNED,
                                       &cert,
                                       &cert.SignatureAlgorithm,
                                       nullptr,
                                       nullptr,
                                       &size) ||
        size > 1024 * 1024)
        return {};
    QByteArray result(int(size), 0);
    if (!CryptSignAndEncodeCertificate(handle,
                                       CERT_NCRYPT_KEY_SPEC,
                                       X509_ASN_ENCODING,
                                       X509_CERT_TO_BE_SIGNED,
                                       &cert,
                                       &cert.SignatureAlgorithm,
                                       nullptr,
                                       reinterpret_cast<PBYTE>(result.data()),
                                       &size))
        return {};
    result.resize(int(size));
    return result;
}
} // namespace wincryptoQCAPlugin
