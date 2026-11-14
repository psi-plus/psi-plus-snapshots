// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rsa_codec.h"
#include <bcrypt.h>
#include <cstring>
#include <wincrypt.h>
#include <windows.h>

using namespace QCA;
namespace wincryptoQCAPlugin {
namespace {
constexpr int     MaxDER           = 65536;
constexpr quint32 ExportIterations = 1300000;
constexpr quint32 MaxKdfWork       = 10000000;
struct View
{
    const char *data = nullptr;
    int         size = 0;
};
SecureArray copy(View v)
{
    SecureArray result(v.size);
    if (v.size)
        std::memcpy(result.data(), v.data, std::size_t(v.size));
    return result;
}
// Strict, bounded DER: no indefinite lengths, negative integers, overlong
// encodings, trailing bytes, recursive descent or allocations from input sizes.
class Reader
{
public:
    Reader() = default;
    explicit Reader(View v)
        : _view(v)
    {
    }
    bool empty() const
    {
        return !_view.size;
    }
    int tag() const
    {
        return empty() ? -1 : uchar(_view.data[0]);
    }
    bool get(int tag, View *out)
    {
        if (_view.size < 2 || uchar(_view.data[0]) != tag)
            return false;
        int offset = 2, size = uchar(_view.data[1]);
        if (size & 0x80) {
            const int count = size & 0x7f;
            if (!count || count > 3 || _view.size < 2 + count || !_view.data[2])
                return false;
            size = 0;
            for (int i = 0; i < count; ++i)
                size = (size << 8) | uchar(_view.data[offset++]);
            if (size < 128)
                return false;
        }
        if (size > _view.size - offset)
            return false;
        *out = {_view.data + offset, size};
        _view.data += offset + size;
        _view.size -= offset + size;
        return true;
    }
    bool sequence(Reader *out)
    {
        View v;
        if (!get(0x30, &v))
            return false;
        *out = Reader(v);
        return true;
    }
    bool integer(SecureArray *out)
    {
        View v;
        if (!get(2, &v) || !v.size || v.size > 2049 || (uchar(v.data[0]) & 0x80))
            return false;
        if (v.size > 1 && !v.data[0]) {
            if (!(uchar(v.data[1]) & 0x80))
                return false;
            ++v.data;
            --v.size;
        }
        *out = copy(v);
        return true;
    }
    bool number(quint32 *out)
    {
        SecureArray value;
        if (!integer(&value) || value.size() > 4)
            return false;
        *out = 0;
        for (int i = 0; i < value.size(); ++i)
            *out = (*out << 8) | uchar(value[i]);
        return true;
    }
    bool oid(const char *hex)
    {
        View       v;
        const auto expected = QByteArray::fromHex(hex);
        return get(6, &v) && v.size == expected.size() &&
            !std::memcmp(v.data, expected.constData(), std::size_t(v.size));
    }
    bool nullOrEmpty()
    {
        View v;
        return empty() || (get(5, &v) && !v.size && empty());
    }

private:
    View _view;
};
View view(const MemoryRegion &a)
{
    return {a.constData(), a.size()};
}
SecureArray tlv(int tag, const SecureArray &value)
{
    if (value.size() > MaxDER)
        return {};
    int count = 0;
    for (int size = value.size(); size; size >>= 8)
        ++count;
    const int   header = value.size() < 128 ? 2 : 2 + count;
    SecureArray out(header + value.size());
    out[0] = char(tag);
    out[1] = char(value.size() < 128 ? value.size() : 0x80 | count);
    if (header > 2)
        for (int i = 0; i < count; ++i)
            out[2 + i] = char(value.size() >> (8 * (count - 1 - i)));
    if (value.size())
        std::memcpy(out.data() + header, value.constData(), std::size_t(value.size()));
    return out;
}
SecureArray sequence(const SecureArray &value)
{
    return tlv(0x30, value);
}
SecureArray oid(const char *hex)
{
    return tlv(6, SecureArray(QByteArray::fromHex(hex)));
}
SecureArray integer(const SecureArray &value)
{
    if (value.isEmpty())
        return {};
    SecureArray positive = value;
    if (uchar(value[0]) & 0x80)
        positive = SecureArray(1) + value;
    return tlv(2, positive);
}
SecureArray number(quint32 value)
{
    int count = 1;
    for (quint32 n = value >> 8; n; n >>= 8)
        ++count;
    SecureArray bytes(count);
    for (int i = 0; i < count; ++i)
        bytes[i] = char(value >> (8 * (count - 1 - i)));
    return integer(bytes);
}
bool rsaAlgorithm(Reader &reader)
{
    Reader algorithm;
    // Accept rsaEncryption only; never discard RSA-PSS key restrictions.
    return reader.sequence(&algorithm) && algorithm.oid("2a864886f70d010101") && algorithm.nullOrEmpty();
}
bool publicParts(Reader &key, RsaParts *parts)
{
    return key.integer(&parts->n) && key.integer(&parts->e) && key.empty();
}
bool privateParts(Reader &key, RsaParts *parts)
{
    quint32 version;
    return key.number(&version) && version == 0 && key.integer(&parts->n) && key.integer(&parts->e) &&
        key.integer(&parts->d) && key.integer(&parts->p) && key.integer(&parts->q) && key.integer(&parts->dp) &&
        key.integer(&parts->dq) && key.integer(&parts->qi) && key.empty();
}
bool plainPrivate(const SecureArray &der, RsaParts *parts)
{
    Reader input(view(der)), outer;
    if (der.isEmpty() || der.size() > MaxDER || !input.sequence(&outer) || !input.empty())
        return false;
    Reader  probe = outer;
    quint32 version;
    if (!probe.number(&version) || version != 0)
        return false;
    if (probe.tag() == 2)
        return privateParts(outer, parts); // PKCS#1
    View   octets;
    Reader key;
    if (!rsaAlgorithm(probe) || !probe.get(4, &octets) || !probe.empty())
        return false;
    Reader embedded(octets);
    return embedded.sequence(&key) && embedded.empty() && privateParts(key, parts);
}
struct Protection
{
    bool        tripleDES  = false;
    int         keySize    = 32;
    QString     hash       = QStringLiteral("sha1");
    quint32     iterations = ExportIterations;
    SecureArray salt, iv;
    int         blockSize() const
    {
        return tripleDES ? 8 : 16;
    }
};
bool protectionAlgorithm(PBEAlgorithm algorithm, Protection *p)
{
    switch (algorithm) {
    case PBEDefault:
    case PBES2_TripleDES_SHA1:
        p->tripleDES = true;
        p->keySize   = 24;
        return true;
    case PBES2_AES128_SHA1:
        p->keySize = 16;
        return true;
    case PBES2_AES192_SHA1:
        p->keySize = 24;
        return true;
    case PBES2_AES256_SHA1:
        p->keySize = 32;
        return true;
    default:
        return false;
    }
}
SecureArray cbc(const SecureArray &input, const SecureArray &key, const Protection &p, bool encrypt)
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
    if (input.isEmpty() || input.size() > MaxDER || key.size() != p.keySize || p.iv.size() != p.blockSize())
        return {};
    if (BCryptOpenAlgorithmProvider(
            &handles.algorithm, p.tripleDES ? BCRYPT_3DES_ALGORITHM : BCRYPT_AES_ALGORITHM, nullptr, 0) < 0 ||
        BCryptSetProperty(handles.algorithm,
                          BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC),
                          0) < 0 ||
        BCryptGenerateSymmetricKey(handles.algorithm,
                                   &handles.key,
                                   nullptr,
                                   0,
                                   reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())),
                                   ULONG(key.size()),
                                   0) < 0)
        return {};
    // CNG owns its key object and releases it at BCryptDestroyKey; plaintext
    // and derived bytes use QCA's protected allocator. Never publish partial output.
    SecureArray iv        = p.iv, output(input.size() + p.blockSize());
    ULONG       size      = 0;
    const auto  operation = encrypt ? BCryptEncrypt : BCryptDecrypt;
    if (operation(handles.key,
                  reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
                  ULONG(input.size()),
                  nullptr,
                  reinterpret_cast<PUCHAR>(iv.data()),
                  ULONG(iv.size()),
                  reinterpret_cast<PUCHAR>(output.data()),
                  ULONG(output.size()),
                  &size,
                  BCRYPT_BLOCK_PADDING) < 0 ||
        size > ULONG(output.size()))
        return {};
    output.resize(int(size));
    return output;
}
SecureArray derive(const SecureArray &passphrase, const Protection &p, const QString &provider)
{
    const quint32 hashSize = p.hash == QLatin1String("sha1") ? 20
        : p.hash == QLatin1String("sha256")                  ? 32
        : p.hash == QLatin1String("sha384")                  ? 48
                                                             : 64;
    if (!p.iterations || quint64(p.iterations) * ((p.keySize + hashSize - 1) / hashSize) > MaxKdfWork ||
        passphrase.size() > 1024 * 1024 || p.salt.isEmpty() || p.salt.size() > 1024)
        return {};
    if (!isSupported(QStringList {QStringLiteral("pbkdf2(%1)").arg(p.hash)}, provider))
        return {};
    return PBKDF2(p.hash, provider).makeKey(passphrase, InitializationVector(p.salt), p.keySize, p.iterations);
}
bool readProtection(Reader &outer, Protection *p, View *encrypted)
{
    Reader algorithm, params, kdf, kdfParams, cipher;
    View   salt, iv;
    if (!outer.sequence(&algorithm) || !algorithm.oid("2a864886f70d01050d") || !algorithm.sequence(&params) ||
        !algorithm.empty() || !params.sequence(&kdf) || !kdf.oid("2a864886f70d01050c") || !kdf.sequence(&kdfParams) ||
        !kdf.empty() || !kdfParams.get(4, &salt) || salt.size < 1 || salt.size > 1024 ||
        !kdfParams.number(&p->iterations) || !p->iterations)
        return false;
    quint32 declaredSize = 0;
    if (kdfParams.tag() == 2 && (!kdfParams.number(&declaredSize) || !declaredSize))
        return false;
    if (!kdfParams.empty()) {
        Reader prf;
        if (!kdfParams.sequence(&prf) || !kdfParams.empty())
            return false;
        Reader choice = prf;
        if (choice.oid("2a864886f70d0207"))
            p->hash = QStringLiteral("sha1");
        else if ((choice = prf).oid("2a864886f70d0209"))
            p->hash = QStringLiteral("sha256");
        else if ((choice = prf).oid("2a864886f70d020a"))
            p->hash = QStringLiteral("sha384");
        else if ((choice = prf).oid("2a864886f70d020b"))
            p->hash = QStringLiteral("sha512");
        else
            return false;
        if (!choice.nullOrEmpty())
            return false;
    }
    if (!params.sequence(&cipher) || !params.empty())
        return false;
    Reader choice = cipher;
    if (choice.oid("2a864886f70d0307")) {
        p->tripleDES = true;
        p->keySize   = 24;
    } else if ((choice = cipher).oid("608648016503040102"))
        p->keySize = 16;
    else if ((choice = cipher).oid("608648016503040116"))
        p->keySize = 24;
    else if ((choice = cipher).oid("60864801650304012a"))
        p->keySize = 32;
    else
        return false;
    if ((declaredSize && declaredSize != quint32(p->keySize)) || !choice.get(4, &iv) || !choice.empty() ||
        iv.size != p->blockSize() || !outer.get(4, encrypted) || !outer.empty() || !encrypted->size ||
        encrypted->size % p->blockSize())
        return false;
    p->salt = copy(salt);
    p->iv   = copy(iv);
    return true;
}
}
bool decodePublicRSA(const MemoryRegion &der, RsaParts *parts)
{
    *parts = {};
    Reader input(view(der)), outer;
    if (der.isEmpty() || der.size() > MaxDER || !input.sequence(&outer) || !input.empty())
        return false;
    if (outer.tag() == 2)
        return publicParts(outer, parts);
    View bits;
    if (!rsaAlgorithm(outer) || !outer.get(3, &bits) || !outer.empty() || bits.size < 2 || bits.data[0])
        return false;
    Reader embedded({bits.data + 1, bits.size - 1}), key;
    return embedded.sequence(&key) && embedded.empty() && publicParts(key, parts);
}
ConvertResult
decodePrivateRSA(const SecureArray &der, const SecureArray &passphrase, const QString &provider, RsaParts *parts)
{
    *parts = {};
    if (plainPrivate(der, parts))
        return ConvertGood;
    Reader input(view(der)), outer;
    if (der.isEmpty() || der.size() > MaxDER || !input.sequence(&outer) || !input.empty())
        return ErrorDecode;
    Protection p;
    View       encrypted;
    if (!readProtection(outer, &p, &encrypted))
        return ErrorDecode;
    const auto key = derive(passphrase, p, provider);
    if (key.isEmpty())
        return ErrorDecode;
    const auto plain = cbc(copy(encrypted), key, p, false);
    *parts           = {};
    return !plain.isEmpty() && plainPrivate(plain, parts) ? ConvertGood : ErrorPassphrase;
}
SecureArray encodePublicRSA(const RsaParts &parts)
{
    const auto body = sequence(integer(parts.n) + integer(parts.e));
    if (parts.n.isEmpty() || parts.e.isEmpty() || body.isEmpty())
        return {};
    return sequence(sequence(oid("2a864886f70d010101") + tlv(5, {})) + tlv(3, SecureArray(1) + body));
}
SecureArray protectPrivateRSA(const SecureArray &der,
                              const SecureArray &passphrase,
                              PBEAlgorithm       algorithm,
                              const QString     &provider)
{
    if (passphrase.isEmpty())
        return der;
    Protection p;
    if (der.isEmpty() || der.size() > MaxDER || !protectionAlgorithm(algorithm, &p))
        return {};
    Random random(provider);
    p.salt         = random.nextBytes(16);
    p.iv           = random.nextBytes(p.blockSize());
    const auto key = derive(passphrase, p, provider);
    if (key.isEmpty())
        return {};
    const auto encrypted = cbc(der, key, p, true);
    if (encrypted.isEmpty())
        return {};
    const auto kdf    = sequence(oid("2a864886f70d01050c") + sequence(tlv(4, p.salt) + number(p.iterations)));
    const auto cipher = sequence(oid(p.tripleDES           ? "2a864886f70d0307"
                                         : p.keySize == 16 ? "608648016503040102"
                                         : p.keySize == 24 ? "608648016503040116"
                                                           : "60864801650304012a") +
                                 tlv(4, p.iv));
    return sequence(sequence(oid("2a864886f70d01050d") + sequence(kdf + cipher)) + tlv(4, encrypted));
}
QString encodeKeyPEM(const SecureArray &der, const QString &label)
{
    if (der.isEmpty() || der.size() > MaxDER)
        return {};
    DWORD length = 0;
    if (!CryptBinaryToStringW(reinterpret_cast<const BYTE *>(der.constData()),
                              DWORD(der.size()),
                              CRYPT_STRING_BASE64,
                              nullptr,
                              &length) ||
        length > 2 * MaxDER)
        return {};
    const auto header = QStringLiteral("-----BEGIN %1-----\n").arg(label),
               footer = QStringLiteral("-----END %1-----\n").arg(label);
    QString    output(header.size() + length + footer.size(), QChar(0));
    std::memcpy(output.data(), header.constData(), std::size_t(header.size()) * sizeof(QChar));
    if (!CryptBinaryToStringW(reinterpret_cast<const BYTE *>(der.constData()),
                              DWORD(der.size()),
                              CRYPT_STRING_BASE64,
                              reinterpret_cast<wchar_t *>(output.data() + header.size()),
                              &length)) {
        SecureZeroMemory(output.data(), std::size_t(output.size()) * sizeof(QChar));
        return {};
    }
    // CryptBinaryToString's actual character count excludes its terminator.
    output.resize(header.size() + length + footer.size());
    std::memcpy(output.data() + header.size() + length, footer.constData(), std::size_t(footer.size()) * sizeof(QChar));
    return output;
}
bool decodeKeyPEM(const QString &pem, bool privateKey, SecureArray *der)
{
    der->clear();
    if (pem.isEmpty() || pem.size() > 2 * MaxDER)
        return false;
    const QStringList labels = privateKey
        ? QStringList {QStringLiteral("PRIVATE KEY"),
                       QStringLiteral("RSA PRIVATE KEY"),
                       QStringLiteral("ENCRYPTED PRIVATE KEY")}
        : QStringList {QStringLiteral("PUBLIC KEY"), QStringLiteral("RSA PUBLIC KEY")};
    int               start  = 0;
    while (start < pem.size() && pem[start].isSpace())
        ++start;
    for (const auto &label : labels) {
        const auto header = QStringLiteral("-----BEGIN %1-----").arg(label),
                   footer = QStringLiteral("-----END %1-----").arg(label);
        if (QStringView(pem).mid(start, header.size()) != header)
            continue;
        const auto end = pem.indexOf(footer, start + header.size());
        if (end < 0)
            return false;
        for (int i = int(end + footer.size()); i < pem.size(); ++i)
            if (!pem[i].isSpace())
                return false;
        const int offset = start + int(header.size()), size = int(end) - offset;
        // Decode only the body, accepting base64 and ASCII whitespace. Reject
        // legacy PEM encryption headers and arbitrary ignored prefix/suffixes.
        for (int i = offset; i < end; ++i) {
            const auto c = pem[i].unicode();
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
                  c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t'))
                return false;
        }
        DWORD      length = 0;
        const auto data   = reinterpret_cast<const wchar_t *>(pem.constData() + offset);
        if (!CryptStringToBinaryW(
                data, DWORD(size), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &length, nullptr, nullptr) ||
            !length || length > MaxDER)
            return false;
        SecureArray output {int(length)};
        if (!CryptStringToBinaryW(data,
                                  DWORD(size),
                                  CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                                  reinterpret_cast<BYTE *>(output.data()),
                                  &length,
                                  nullptr,
                                  nullptr))
            return false;
        output.resize(int(length));
        *der = output;
        return true;
    }
    return false;
}
}
