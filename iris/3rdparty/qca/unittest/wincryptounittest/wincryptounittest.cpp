// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aesmodes.h"
#include <QElapsedTimer>
#include <QMessageAuthenticationCode>
#include <QTest>
#include <QtCrypto>
#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif
#include <bcrypt.h>
#include <cstring>
#include <future>
#include <limits>
#include <windows.h>

namespace {
const QString     ProviderName = QStringLiteral("qca-wincrypto");
QCA::SymmetricKey key()
{
    return QCA::SymmetricKey(QCA::SecureArray(32, 'k'));
}
QCA::InitializationVector iv(int size = 12)
{
    return QCA::InitializationVector(QCA::SecureArray(size, 'n'));
}
QCA::Cipher cipher(QCA::Direction dir, const QCA::AuthTag &tag, int nonceSize = 12)
{
    return QCA::Cipher(QStringLiteral("aes256"),
                       QCA::Cipher::GCM,
                       QCA::Cipher::NoPadding,
                       dir,
                       key(),
                       iv(nonceSize),
                       tag,
                       ProviderName);
}

class FailingCipher : public wincryptoQCAPlugin::BlockCipher
{
public:
    std::unique_ptr<BlockCipher> clone() const override
    {
        return std::make_unique<FailingCipher>();
    }
    bool encrypt(const std::uint8_t *, std::uint8_t *out, std::size_t blocks) override
    {
        std::memset(out, 0xab, blocks * BlockSize);
        return false;
    }
    bool decrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) override
    {
        return encrypt(in, out, blocks);
    }
};

struct KeyLifetime
{
    int  alive = 0;
    bool fail  = false;
};
class TrackedCipher : public wincryptoQCAPlugin::BlockCipher
{
public:
    explicit TrackedCipher(std::shared_ptr<KeyLifetime> lifetime)
        : _lifetime(std::move(lifetime))
    {
        ++_lifetime->alive;
    }
    ~TrackedCipher() override
    {
        --_lifetime->alive;
    }
    std::unique_ptr<BlockCipher> clone() const override
    {
        return std::make_unique<TrackedCipher>(_lifetime);
    }
    bool encrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) override
    {
        if (_lifetime->fail)
            return false;
        std::memcpy(out, in, blocks * BlockSize);
        return true;
    }
    bool decrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) override
    {
        return encrypt(in, out, blocks);
    }

private:
    std::shared_ptr<KeyLifetime> _lifetime;
};
}

class WinCryptoTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void gcmStreaming_data();
    void gcmStreaming();
    void gcmEmptyAndInvalidParameters();
    void failureIsTerminal();
    void hkdfLimitsAndVectors();
    void pbkdfLimits();
    void largeGcm();
    void cbcAndCtrStreaming();
    void hashesAndMacsInParallel();
    void releasesFinishedKeys();
    void macClearAndRekey();
    void secureEraseRanges();

private:
    QCA::Initializer _qca;
};

void WinCryptoTest::initTestCase()
{
    // These assertions prevent a passing run caused by provider fallback or skips.
    const QStringList required {QStringLiteral("random"),
                                QStringLiteral("hmac(sha256)"),
                                QStringLiteral("sha512"),
                                QStringLiteral("aes256-gcm"),
                                QStringLiteral("aes128-gcm"),
                                QStringLiteral("aes128-cbc-pkcs7"),
                                QStringLiteral("aes192-cbc-pkcs7"),
                                QStringLiteral("aes256-cbc-pkcs7"),
                                QStringLiteral("aes128-ctr"),
                                QStringLiteral("aes192-ctr"),
                                QStringLiteral("aes256-ctr"),
                                QStringLiteral("hkdf(sha256)"),
                                QStringLiteral("pbkdf2(sha256)"),
                                QStringLiteral("cert"),
                                QStringLiteral("tls")};
    QVERIFY(QCA::isSupported(required, ProviderName));
    QCA::Random random(ProviderName);
    QCOMPARE(random.nextBytes(32).size(), 32);
}

void WinCryptoTest::gcmStreaming_data()
{
    QTest::addColumn<int>("nonceSize");
    QTest::addColumn<int>("tagSize");
    QTest::addColumn<int>("chunk");
    for (int nonce : {12, 13})
        for (int tag : {4, 8, 12, 13, 14, 15, 16})
            for (int chunk : {1, 15, 16, 17, 4096})
                QTest::newRow(qPrintable(QStringLiteral("iv%1-tag%2-chunk%3").arg(nonce).arg(tag).arg(chunk)))
                    << nonce << tag << chunk;
}

void WinCryptoTest::gcmStreaming()
{
    QFETCH(int, nonceSize);
    QFETCH(int, tagSize);
    QFETCH(int, chunk);
    QByteArray plain(8197, 0);
    for (int i = 0; i < plain.size(); ++i)
        plain[i] = char(i * 37);
    auto       single   = cipher(QCA::Encode, QCA::AuthTag(tagSize), nonceSize);
    const auto expected = single.process(QCA::SecureArray(plain)).toByteArray();
    QVERIFY(single.ok());
    auto enc = cipher(QCA::Encode, QCA::AuthTag(tagSize), nonceSize);
    QCOMPARE(enc.provider()->name(), ProviderName);
    QByteArray encoded;
    for (int offset = 0; offset < plain.size(); offset += chunk) {
        encoded += enc.update(QCA::SecureArray(plain.mid(offset, chunk))).toByteArray();
        QVERIFY(enc.ok());
        if (offset == 0) {
            auto       copy = enc;
            QByteArray tail = copy.update(QCA::SecureArray(plain.mid(chunk))).toByteArray();
            tail += copy.final().toByteArray();
            QVERIFY(copy.ok());
            QCOMPARE(encoded + tail, expected);
            QCOMPARE(copy.tag().toByteArray(), single.tag().toByteArray());
        }
    }
    encoded += enc.final().toByteArray();
    QVERIFY(enc.ok());
    QCOMPARE(encoded, expected);
    QCOMPARE(enc.tag().toByteArray(), single.tag().toByteArray());
    auto dec = cipher(QCA::Decode, enc.tag(), nonceSize);
    for (int offset = 0; offset < encoded.size(); offset += chunk) {
        QVERIFY(dec.update(QCA::SecureArray(encoded.mid(offset, chunk))).isEmpty());
        QVERIFY(dec.ok());
    }
    auto copy = dec;
    QCOMPARE(dec.final().toByteArray(), plain);
    QVERIFY(dec.ok());
    QCOMPARE(copy.final().toByteArray(), plain);
    QVERIFY(copy.ok());
    auto badTag = enc.tag().toByteArray();
    badTag[0]   = char(badTag[0] ^ 1);
    auto bad    = cipher(QCA::Decode, QCA::AuthTag(badTag), nonceSize);
    QVERIFY(bad.process(QCA::SecureArray(encoded)).isEmpty());
    QVERIFY(!bad.ok());
    encoded[encoded.size() / 2] = char(encoded[encoded.size() / 2] ^ 1);
    auto tampered               = cipher(QCA::Decode, enc.tag(), nonceSize);
    QVERIFY(tampered.process(QCA::SecureArray(encoded)).isEmpty());
    QVERIFY(!tampered.ok());
}

void WinCryptoTest::gcmEmptyAndInvalidParameters()
{
    for (int nonce : {12, 13}) {
        auto enc = cipher(QCA::Encode, QCA::AuthTag(16), nonce);
        QVERIFY(enc.process(QCA::SecureArray()).isEmpty());
        QVERIFY(enc.ok());
        auto dec = cipher(QCA::Decode, enc.tag(), nonce);
        QVERIFY(dec.process(QCA::SecureArray()).isEmpty());
        QVERIFY(dec.ok());
        auto tag = enc.tag().toByteArray();
        tag[0]   = char(tag[0] ^ 1);
        auto bad = cipher(QCA::Decode, QCA::AuthTag(tag), nonce);
        QVERIFY(bad.process(QCA::SecureArray()).isEmpty());
        QVERIFY(!bad.ok());
        for (int size : {0, 17}) {
            auto invalid = cipher(QCA::Encode, QCA::AuthTag(size), nonce);
            QVERIFY(invalid.process(QCA::SecureArray("secret")).isEmpty());
            QVERIFY(!invalid.ok());
        }
    }
}

void WinCryptoTest::failureIsTerminal()
{
    using namespace wincryptoQCAPlugin;
    ModeEngine engine(CipherMode::CBC, false, true, std::make_unique<FailingCipher>(), Bytes(16, 0), {});
    QVERIFY(engine.isValid());
    Bytes       output {42};
    const Bytes input(16, 0);
    QVERIFY(!engine.update(input.data(), input.size(), output));
    QCOMPARE(output.size(), std::size_t(1));
    QCOMPARE(output[0], std::uint8_t(42));
    QVERIFY(!engine.isValid());
    QVERIFY(!engine.update(input.data(), input.size(), output));
    QVERIFY(!engine.final(output));
}

void WinCryptoTest::hkdfLimitsAndVectors()
{
    QCA::HKDF                       hkdf(QStringLiteral("sha256"), ProviderName);
    const QCA::SecureArray          ikm(QByteArray(22, char(0x0b)));
    const QCA::InitializationVector salt(QByteArray::fromHex("000102030405060708090a0b0c"));
    const QCA::InitializationVector info(QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9"));
    QCOMPARE(hkdf.makeKey(ikm, salt, info, 42).toByteArray().toHex(),
             QByteArray("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
    const auto maximum = hkdf.makeKey(ikm, salt, info, 255 * 32).toByteArray();
    QCOMPARE(maximum.size(), 255 * 32);
    // Independent Qt HMAC reference covers all 255 expansion blocks.
    const auto prk =
        QMessageAuthenticationCode::hash(ikm.toByteArray(), salt.toByteArray(), QCryptographicHash::Sha256);
    QByteArray expected, previous;
    for (int i = 1; i <= 255; ++i) {
        const QByteArray input = previous + info.toByteArray() + char(i);
        previous               = QMessageAuthenticationCode::hash(input, prk, QCryptographicHash::Sha256);
        expected += previous;
    }
    QCOMPARE(maximum, expected);
    QVERIFY(hkdf.makeKey(ikm, salt, info, 255 * 32 + 1).isEmpty());
}

void WinCryptoTest::pbkdfLimits()
{
    QCA::PBKDF2 kdf(QStringLiteral("sha256"), ProviderName);
    QVERIFY(kdf.makeKey(QCA::SecureArray("pass"), iv(), std::numeric_limits<unsigned>::max(), 1u).isEmpty());
    QVERIFY(kdf.makeKey(QCA::SecureArray("pass"), iv(), 32, 0u).isEmpty());
    unsigned iterations = 999;
    QCOMPARE(kdf.makeKey(QCA::SecureArray("pass"), iv(), 32, 0, &iterations).size(), 32);
    QVERIFY(iterations > 0);
}

void WinCryptoTest::largeGcm()
{
    const QByteArray plain(8 * 1024 * 1024 + 7, 'x');
    QElapsedTimer    timer;
    timer.start();
    auto       enc = cipher(QCA::Encode, QCA::AuthTag(16));
    QByteArray encoded;
    for (int offset = 0; offset < plain.size(); offset += 65537)
        encoded += enc.update(QCA::SecureArray(plain.mid(offset, 65537))).toByteArray();
    encoded += enc.final().toByteArray();
    QVERIFY(enc.ok());
    auto dec = cipher(QCA::Decode, enc.tag());
    for (int offset = 0; offset < encoded.size(); offset += 65537)
        QVERIFY(dec.update(QCA::SecureArray(encoded.mid(offset, 65537))).isEmpty());
    QCOMPARE(dec.final().toByteArray(), plain);
    QVERIFY(dec.ok());
    qInfo("8 MiB GCM encrypt+authenticate+decrypt: %lld ms", timer.elapsed());
}

void WinCryptoTest::cbcAndCtrStreaming()
{
    for (int keySize : {16, 24, 32}) {
        const auto              algorithm = QStringLiteral("aes%1").arg(keySize * 8);
        const QCA::SymmetricKey aesKey(QCA::SecureArray(keySize, 'k'));
        for (auto mode : {QCA::Cipher::CBC, QCA::Cipher::CTR}) {
            const auto padding = mode == QCA::Cipher::CBC ? QCA::Cipher::PKCS7 : QCA::Cipher::NoPadding;
            for (int size : {0, 1, 15, 16, 17, 1025, 65537}) {
                const QByteArray plain(size, 'p');
                QCA::Cipher      reference(algorithm, mode, padding, QCA::Encode, aesKey, iv(16), ProviderName);
                const auto       expected = reference.process(QCA::SecureArray(plain)).toByteArray();
                QVERIFY(reference.ok());
                for (int chunk : {1, 15, 17, 4096}) {
                    QCA::Cipher enc(algorithm, mode, padding, QCA::Encode, aesKey, iv(16), ProviderName);
                    QByteArray  encoded;
                    for (int offset = 0; offset < size; offset += chunk) {
                        encoded += enc.update(QCA::SecureArray(plain.mid(offset, chunk))).toByteArray();
                        QVERIFY(enc.ok());
                        if (offset == 0) {
                            auto copy = enc;
                            auto tail = copy.update(QCA::SecureArray(plain.mid(chunk))).toByteArray();
                            tail += copy.final().toByteArray();
                            QVERIFY(copy.ok());
                            QCOMPARE(encoded + tail, expected);
                        }
                    }
                    encoded += enc.final().toByteArray();
                    QVERIFY(enc.ok());
                    QCOMPARE(encoded, expected);
                    QCA::Cipher dec(algorithm, mode, padding, QCA::Decode, aesKey, iv(16), ProviderName);
                    QByteArray  decoded;
                    for (int offset = 0; offset < encoded.size(); offset += chunk)
                        decoded += dec.update(QCA::SecureArray(encoded.mid(offset, chunk))).toByteArray();
                    decoded += dec.final().toByteArray();
                    QVERIFY(dec.ok());
                    QCOMPARE(decoded, plain);
                }
            }
        }
    }
    QCA::Cipher enc(
        QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7, QCA::Encode, key(), iv(16), ProviderName);
    const auto encoded   = enc.process(QCA::SecureArray("12345678"));
    auto       corruptIV = iv(16).toByteArray();
    corruptIV[15]        = char(corruptIV[15] ^ 1);
    QCA::Cipher bad(QStringLiteral("aes256"),
                    QCA::Cipher::CBC,
                    QCA::Cipher::PKCS7,
                    QCA::Decode,
                    key(),
                    QCA::InitializationVector(corruptIV),
                    ProviderName);
    QVERIFY(bad.process(encoded).isEmpty());
    QVERIFY(!bad.ok());
}

void WinCryptoTest::hashesAndMacsInParallel()
{
    const QByteArray input(1024 * 1024 + 19, 'h');
    const auto       digest = QCryptographicHash::hash(input, QCryptographicHash::Sha512);
    const auto       mac    = QMessageAuthenticationCode::hash(input, QByteArray("secret"), QCryptographicHash::Sha256);
    std::vector<std::future<bool>> jobs;
    for (int job = 0; job < 8; ++job)
        jobs.push_back(std::async(std::launch::async, [&] {
            QCA::Hash                      hash(QStringLiteral("sha512"), ProviderName);
            QCA::MessageAuthenticationCode hmac(
                QStringLiteral("hmac(sha256)"), QCA::SymmetricKey(QByteArray("secret")), ProviderName);
            hash.update(QCA::MemoryRegion(input.left(17)));
            hmac.update(QCA::MemoryRegion(input.left(17)));
            auto hashCopy = hash;
            auto macCopy  = hmac;
            hash.update(QCA::MemoryRegion(input.mid(17)));
            hashCopy.update(QCA::MemoryRegion(input.mid(17)));
            hmac.update(QCA::MemoryRegion(input.mid(17)));
            macCopy.update(QCA::MemoryRegion(input.mid(17)));
            return hash.final().toByteArray() == digest && hashCopy.final().toByteArray() == digest &&
                hmac.final().toByteArray() == mac && macCopy.final().toByteArray() == mac;
        }));
    for (auto &job : jobs)
        QVERIFY(job.get());
    QCA::MessageAuthenticationCode empty(QStringLiteral("hmac(sha256)"), QCA::SymmetricKey(), ProviderName);
    empty.update(QCA::MemoryRegion(input));
    QCOMPARE(empty.final().toByteArray(),
             QMessageAuthenticationCode::hash(input, QByteArray(), QCryptographicHash::Sha256));
}

void WinCryptoTest::releasesFinishedKeys()
{
    using namespace wincryptoQCAPlugin;
    for (auto mode : {CipherMode::CBC, CipherMode::CTR, CipherMode::GCM}) {
        auto       lifetime = std::make_shared<KeyLifetime>();
        ModeEngine engine(mode,
                          mode == CipherMode::CBC,
                          true,
                          std::make_unique<TrackedCipher>(lifetime),
                          Bytes(mode == CipherMode::GCM ? 12 : 16, 0),
                          Bytes(mode == CipherMode::GCM ? 16 : 0, 0));
        QVERIFY(engine.isValid());
        Bytes       out;
        const Bytes input(23, 1);
        QVERIFY(engine.update(input.data(), input.size(), out));
        ModeEngine copy(engine);
        QCOMPARE(lifetime->alive, 2);
        QVERIFY(engine.final(out));
        QCOMPARE(lifetime->alive, 1); // The completed object itself is still alive.
        if (mode == CipherMode::GCM)
            QCOMPARE(engine.tag().size(), std::size_t(16));
        Bytes tail;
        QVERIFY(copy.final(tail));
        QCOMPARE(lifetime->alive, 0);
    }
    auto       lifetime = std::make_shared<KeyLifetime>();
    ModeEngine invalid(CipherMode::CBC, false, true, std::make_unique<TrackedCipher>(lifetime), {}, {});
    QVERIFY(!invalid.isValid());
    QCOMPARE(lifetime->alive, 0);
    ModeEngine failed(CipherMode::CBC, false, true, std::make_unique<TrackedCipher>(lifetime), Bytes(16, 0), {});
    lifetime->fail = true;
    Bytes out, input(16, 0);
    QVERIFY(!failed.update(input.data(), input.size(), out));
    QCOMPARE(lifetime->alive, 0);
    lifetime->fail = false;
    ModeEngine badPadding(CipherMode::CBC, true, false, std::make_unique<TrackedCipher>(lifetime), Bytes(16, 0), {});
    QVERIFY(badPadding.update(input.data(), input.size(), out));
    QCOMPARE(lifetime->alive, 1);
    QVERIFY(!badPadding.final(out));
    QCOMPARE(lifetime->alive, 0);
    ModeEngine failedTag(
        CipherMode::GCM, false, true, std::make_unique<TrackedCipher>(lifetime), Bytes(12, 0), Bytes(16, 0));
    QVERIFY(failedTag.isValid());
    lifetime->fail = true;
    QVERIFY(!failedTag.final(out));
    QCOMPARE(lifetime->alive, 0);
}

void WinCryptoTest::macClearAndRekey()
{
    const QByteArray message("message"), key1("first key"), key2("second key");
    const auto       expected1 = QMessageAuthenticationCode::hash(message, key1, QCryptographicHash::Sha256);
    const auto       expected2 = QMessageAuthenticationCode::hash(message, key2, QCryptographicHash::Sha256);
    QCA::MessageAuthenticationCode mac(QStringLiteral("hmac(sha256)"), QCA::SymmetricKey(key1), ProviderName);
    mac.update(QCA::MemoryRegion(message));
    QCOMPARE(mac.final().toByteArray(), expected1);
    QCOMPARE(mac.final().toByteArray(), expected1);
    auto copy = mac; // Copy a completed MAC whose native keyed state was released.
    copy.clear();
    copy.update(QCA::MemoryRegion(message));
    QCOMPARE(copy.final().toByteArray(), expected1);
    mac.clear();
    mac.update(QCA::MemoryRegion(message));
    QCOMPARE(mac.final().toByteArray(), expected1);
    mac.setup(QCA::SymmetricKey(key2));
    mac.update(QCA::MemoryRegion(message));
    QCOMPARE(mac.final().toByteArray(), expected2);
}

void WinCryptoTest::secureEraseRanges()
{
    using namespace wincryptoQCAPlugin;
    secureWipe(nullptr, 0);
    for (std::size_t offset = 0; offset < 32; ++offset)
        for (std::size_t count : {0, 1, 15, 16, 17, 31, 32, 255, 256, 257, 4096}) {
            Bytes data(offset + count + 32, 0xa7), expected(data);
            std::fill(expected.begin() + offset, expected.begin() + offset + count, 0);
            secureWipe(data.data() + offset, count);
            QVERIFY(data == expected);
        }
}

QTEST_GUILESS_MAIN(WinCryptoTest)
#include "wincryptounittest.moc"
