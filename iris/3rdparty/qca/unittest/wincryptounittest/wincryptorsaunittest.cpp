// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QTest>
#include <QtCrypto>
#include <cstring>
#include <memory>
#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif

namespace {
const QString Provider = QStringLiteral("qca-wincrypto");
QByteArray    der(int tag, const QByteArray &body)
{
    QByteArray out(1, char(tag));
    if (body.size() < 128)
        out += char(body.size());
    else {
        int count = 0;
        for (auto n = body.size(); n; n >>= 8)
            ++count;
        out += char(0x80 | count);
        for (int i = count - 1; i >= 0; --i)
            out += char(body.size() >> (8 * i));
    }
    return out + body;
}
QByteArray integer(const QCA::BigInteger &n)
{
    return der(2, n.toArray().toByteArray());
}
QByteArray oid(const char *hex)
{
    return der(6, QByteArray::fromHex(hex));
}
}
class WinCryptoRSATest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(QCA::isSupported("rsa", Provider));
        QVERIFY(QCA::PKey::supportedIOTypes(Provider).contains(QCA::PKey::RSA));
        _private = QCA::KeyGenerator().createRSA(2048, 65537, Provider);
        QVERIFY(!_private.isNull());
        QCOMPARE(_private.provider()->name(), Provider);
        _public = _private.toPublicKey();
        QVERIFY(!_public.isNull());
        QVERIFY(_private.canExport());
        QVERIFY(_public.canExport());
    }
    void components()
    {
        const auto rsa = _private.toRSA();
        QVERIFY(rsa.p() > QCA::BigInteger(2));
        QVERIFY(rsa.q() > QCA::BigInteger(2));
        QVERIFY(rsa.d() > QCA::BigInteger(1));
        QCA::RSAPrivateKey imported(rsa.n(), rsa.e(), rsa.p(), rsa.q(), rsa.d(), Provider);
        QVERIFY(!imported.isNull());
        QCOMPARE(imported.toPublicKey().toDER(), _public.toDER());
        const QCA::SecureArray message("component import");
        QVERIFY(_public.verifyMessage(message, imported.signMessage(message, QCA::EMSA3_SHA256), QCA::EMSA3_SHA256));
        // CNG may normalize d modulo lcm(p-1,q-1) rather than phi(n).
        auto phi = rsa.p();
        phi -= QCA::BigInteger(1);
        auto q1 = rsa.q();
        q1 -= QCA::BigInteger(1);
        phi *= q1;
        QCA::BigInteger dPhi;
        QVERIFY(QCA::BigIntegerMath::modInverse(rsa.e(), phi, &dPhi));
        QCA::RSAPrivateKey phiKey(rsa.n(), rsa.e(), rsa.p(), rsa.q(), dPhi, Provider);
        QVERIFY(!phiKey.isNull());
        QCOMPARE(phiKey.signMessage(message, QCA::EMSA3_SHA256), imported.signMessage(message, QCA::EMSA3_SHA256));
        QCA::RSAPublicKey pub(rsa.n(), rsa.e(), Provider);
        QVERIFY(!pub.isNull());
        QCOMPARE(pub.toDER(), _public.toDER());
        auto bad = rsa.d();
        bad += QCA::BigInteger(1);
        QVERIFY(QCA::RSAPrivateKey(rsa.n(), rsa.e(), rsa.p(), rsa.q(), bad, Provider).isNull());
        QVERIFY(QCA::RSAPrivateKey(rsa.n(), rsa.e(), rsa.p(), rsa.p(), rsa.d(), Provider).isNull());
        QVERIFY(QCA::RSAPublicKey(rsa.n(), QCA::BigInteger(2), Provider).isNull());
        QVERIFY(QCA::RSAPublicKey(QCA::BigInteger(-1), rsa.e(), Provider).isNull());
        QCA::SecureArray huge(2049, 1);
        QVERIFY(QCA::RSAPublicKey(QCA::BigInteger(huge), rsa.e(), Provider).isNull());
        // A public clone owns a separate public CNG handle and no private values.
        auto ctx       = std::unique_ptr<QCA::Provider::Context>(_public.context()->clone());
        auto pkey      = static_cast<QCA::PKeyContext *>(ctx.get());
        auto publicRSA = static_cast<QCA::RSAContext *>(pkey->key());
        QVERIFY(pkey->importKey(publicRSA));
        QVERIFY(!publicRSA->isPrivate());
        QCOMPARE(publicRSA->p(), QCA::BigInteger(0));
        QCOMPARE(publicRSA->q(), QCA::BigInteger(0));
        QCOMPARE(publicRSA->d(), QCA::BigInteger(0));
        publicRSA->startSign(QCA::EMSA3_SHA256, QCA::DefaultFormat);
        publicRSA->update(message);
        QVERIFY(publicRSA->endSign().isEmpty());
    }
    void encryption()
    {
        const int size = _public.bitSize() / 8;
        for (const auto algorithm : {QCA::EME_PKCS1v15, QCA::EME_PKCS1_OAEP, QCA::EME_NO_PADDING}) {
            const int max = _public.maximumEncryptSize(algorithm);
            QCOMPARE(max, algorithm == QCA::EME_NO_PADDING ? size : size - (algorithm == QCA::EME_PKCS1v15 ? 11 : 42));
            for (int length : {0, 1, max}) {
                QCA::SecureArray plain(length, 'x');
                if (algorithm == QCA::EME_NO_PADDING) {
                    plain           = QCA::SecureArray(size);
                    plain[size - 1] = 2;
                }
                const auto encrypted = _public.encrypt(plain, algorithm);
                QVERIFY2(encrypted.size() == size,
                         qPrintable(QStringLiteral("algorithm %1, input length %2").arg(int(algorithm)).arg(length)));
                QCA::SecureArray decrypted("old secret");
                QVERIFY(_private.decrypt(encrypted, &decrypted, algorithm));
                QCOMPARE(decrypted, plain);
                auto inplace = encrypted;
                QVERIFY(_private.decrypt(inplace, &inplace, algorithm));
                QCOMPARE(inplace, plain);
                QVERIFY(!_private.decrypt(QCA::SecureArray(size - 1), &decrypted, algorithm));
                QVERIFY(decrypted.isEmpty());
            }
            QVERIFY(_public.encrypt(QCA::SecureArray(max + 1), algorithm).isEmpty());
        }
        for (auto algorithm : {QCA::EME_PKCS1v15, QCA::EME_NO_PADDING}) {
            QCA::SecureArray plain(algorithm == QCA::EME_NO_PADDING ? size : 37);
            plain[plain.size() - 1] = 3;
            const auto encrypted    = _private.encrypt(plain, algorithm);
            QCOMPARE(encrypted.size(), size);
            QCA::SecureArray recovered;
            QVERIFY(_public.decrypt(encrypted, &recovered, algorithm));
            QCOMPARE(recovered, plain);
        }
        QCA::SecureArray output("old secret");
        QVERIFY(!_private.decrypt(QCA::SecureArray(size), &output, QCA::EME_PKCS1v15));
        QVERIFY(output.isEmpty());
        QVERIFY(!_private.decrypt(QCA::SecureArray(size), &output, QCA::EME_PKCS1_OAEP));
        QVERIFY(!_public.decrypt(QCA::SecureArray(size), &output, QCA::EME_PKCS1v15));
        QVERIFY(!_public.decrypt(
            _public.encrypt(QCA::SecureArray("a"), QCA::EME_PKCS1_OAEP), &output, QCA::EME_PKCS1_OAEP));
        QVERIFY(_private.encrypt(QCA::SecureArray("a"), QCA::EME_PKCS1_OAEP).isEmpty());
        QCOMPARE(_public.maximumEncryptSize(QCA::EME_PKCS1v15_SSL), 0);
        QVERIFY(_public.encrypt({}, QCA::EME_PKCS1v15_SSL).isEmpty());
        QVERIFY(_public.encrypt(QCA::SecureArray(size, char(0xff)), QCA::EME_NO_PADDING).isEmpty());
    }
    void signatures()
    {
        const QCA::SecureArray message("native RSA signatures");
        for (auto digest : {QCA::SignatureDigest::None,
                            QCA::SignatureDigest::MD5,
                            QCA::SignatureDigest::SHA1,
                            QCA::SignatureDigest::SHA256,
                            QCA::SignatureDigest::SHA384,
                            QCA::SignatureDigest::SHA512}) {
            const QCA::SignatureAlgorithm algorithm {QCA::SignatureScheme::RSA_PKCS1v15, digest};
            for (const auto &plain : {message, QCA::SecureArray()}) {
                const auto signature = _private.signMessage(plain, algorithm);
                QVERIFY2(signature.size() == 256,
                         qPrintable(QStringLiteral("digest %1, input length %2").arg(int(digest)).arg(plain.size())));
                QVERIFY(_public.verifyMessage(plain, signature, algorithm));
                QVERIFY(!_public.verifyMessage(QCA::SecureArray("other"), signature, algorithm));
                auto changed = signature;
                changed[42]  = char(changed[42] ^ 1);
                QVERIFY(!_public.verifyMessage(plain, changed, algorithm));
            }
        }
        for (auto digest : {QCA::SignatureDigest::SHA1,
                            QCA::SignatureDigest::SHA256,
                            QCA::SignatureDigest::SHA384,
                            QCA::SignatureDigest::SHA512})
            for (int salt : {-1, 0, 42}) {
                const QCA::SignatureAlgorithm algorithm {QCA::SignatureScheme::RSA_PSS, digest, digest, salt, 1};
                const auto                    signature = _private.signMessage(message, algorithm);
                QCOMPARE(signature.size(), 256);
                QVERIFY(_public.verifyMessage(message, signature, algorithm));
                QVERIFY(!_public.verifyMessage(message, signature, QCA::EMSA3_SHA256));
            }
        for (const QCA::SignatureAlgorithm algorithm : QList<QCA::SignatureAlgorithm> {
                 {QCA::SignatureScheme::ECDSA, QCA::SignatureDigest::SHA256},
                 {QCA::SignatureScheme::RSA_PKCS1v15, QCA::SignatureDigest::SHA224},
                 {QCA::SignatureScheme::RSA_PKCS1v15, QCA::SignatureDigest::Unknown},
                 {QCA::SignatureScheme::RSA_PKCS1v15, QCA::SignatureDigest::SHA256, QCA::SignatureDigest::SHA256},
                 {QCA::SignatureScheme::RSA_PSS, QCA::SignatureDigest::None},
                 {QCA::SignatureScheme::RSA_PSS, QCA::SignatureDigest::SHA256, QCA::SignatureDigest::SHA1},
                 {QCA::SignatureScheme::RSA_PSS, QCA::SignatureDigest::SHA256, QCA::SignatureDigest::SHA256, 1000},
                 {QCA::SignatureScheme::RSA_PSS, QCA::SignatureDigest::SHA256, QCA::SignatureDigest::Unknown, -2},
                 {QCA::SignatureScheme::RSA_PSS, QCA::SignatureDigest::SHA256, QCA::SignatureDigest::Unknown, 32, 2}}) {
            QVERIFY(_private.signMessage(message, algorithm).isEmpty());
            QVERIFY(!_public.verifyMessage(message, QByteArray(256, 0), algorithm));
        }
        QVERIFY(_private.signMessage(QCA::SecureArray(246), QCA::EMSA3_Raw).isEmpty());
    }
    void streamingAndState()
    {
        // Reuse a fixed chunk: message size must not become retained signing state.
        const QCA::SecureArray chunk(64 * 1024, 'x');
        auto                   signer = _private;
        signer.startSign(QCA::EMSA3_SHA256);
        for (int i = 0; i < 256; ++i)
            signer.update(chunk);
        auto       branch    = signer;
        const auto signature = signer.signature();
        QCOMPARE(signature.size(), 256);
        QCOMPARE(branch.signature(), signature);
        QVERIFY(signer.signature().isEmpty());
        auto verifier = _public;
        verifier.startVerify(QCA::EMSA3_SHA256);
        for (int i = 0; i < 256; ++i)
            verifier.update(chunk);
        auto verifyBranch = verifier;
        QVERIFY(verifier.validSignature(signature));
        QVERIFY(verifyBranch.validSignature(signature));
        QVERIFY(!verifier.validSignature(signature));
        signer.startSign(QCA::EMSA3_Raw);
        signer.update(QCA::SecureArray(246));
        signer.update(QCA::SecureArray("ignored"));
        QVERIFY(signer.signature().isEmpty());
        QVERIFY(!verifier.verifyMessage({}, QByteArray(255, 0), QCA::EMSA3_SHA256));
        QVERIFY(!verifier.verifyMessage({}, QByteArray(257, 0), QCA::EMSA3_SHA256));
    }
    void serialization()
    {
        QCA::ConvertResult result;
        const auto         privateDER = _private.toDER();
        const auto         publicDER  = _public.toDER();
        QVERIFY(!privateDER.isEmpty());
        QVERIFY(!publicDER.isEmpty());
        auto imported = QCA::PrivateKey::fromDER(privateDER, {}, &result, Provider);
        QCOMPARE(result, QCA::ConvertGood);
        QCOMPARE(imported.provider()->name(), Provider);
        QCOMPARE(imported.toPublicKey().toDER(), publicDER);
        QCOMPARE(imported.signMessage({}, QCA::EMSA3_SHA256), _private.signMessage({}, QCA::EMSA3_SHA256));
        QCOMPARE(QCA::PublicKey::fromDER(publicDER, &result, Provider).toDER(), publicDER);
        QCOMPARE(result, QCA::ConvertGood);
        for (const auto &pem : {_private.toPEM(), _public.toPEM()}) {
            QVERIFY(!pem.isEmpty());
            QVERIFY(!pem.contains(QChar(0)));
            QVERIFY(pem.endsWith(QLatin1Char('\n')));
        }
        QCOMPARE(QCA::PrivateKey::fromPEM(_private.toPEM(), {}, &result, Provider).toPublicKey().toDER(), publicDER);
        QCOMPARE(result, QCA::ConvertGood);
        QCOMPARE(QCA::PublicKey::fromPEM(_public.toPEM(), &result, Provider).toDER(), publicDER);
        QCOMPARE(result, QCA::ConvertGood);
        const auto rsa   = _private.toRSA();
        const auto pkcs1 = der(0x30, integer(rsa.n()) + integer(rsa.e()));
        QCOMPARE(QCA::PublicKey::fromDER(pkcs1, &result, Provider).toDER(), publicDER);
        auto p1 = rsa.p();
        p1 -= QCA::BigInteger(1);
        auto q1 = rsa.q();
        q1 -= QCA::BigInteger(1);
        auto dp = rsa.d();
        dp %= p1;
        auto dq = rsa.d();
        dq %= q1;
        QCA::BigInteger qi;
        QVERIFY(QCA::BigIntegerMath::modInverse(rsa.q(), rsa.p(), &qi));
        const auto makePrivate = [&](const QCA::BigInteger &coefficient) {
            return der(0x30,
                       QByteArray::fromHex("020100") + integer(rsa.n()) + integer(rsa.e()) + integer(rsa.d()) +
                           integer(rsa.p()) + integer(rsa.q()) + integer(dp) + integer(dq) + integer(coefficient));
        };
        QCOMPARE(
            QCA::PrivateKey::fromDER(QCA::SecureArray(makePrivate(qi)), {}, &result, Provider).toPublicKey().toDER(),
            publicDER);
        QCOMPARE(result, QCA::ConvertGood);
        qi += QCA::BigInteger(1);
        QVERIFY(QCA::PrivateKey::fromDER(QCA::SecureArray(makePrivate(qi)), {}, &result, Provider).isNull());
        QCOMPARE(result, QCA::ErrorDecode);
    }
    void encryptedKeys_data()
    {
        QTest::addColumn<int>("pbe");
        QTest::newRow("default-3des") << int(QCA::PBEDefault);
        QTest::newRow("aes128") << int(QCA::PBES2_AES128_SHA1);
        QTest::newRow("aes192") << int(QCA::PBES2_AES192_SHA1);
        QTest::newRow("aes256") << int(QCA::PBES2_AES256_SHA1);
    }
    void encryptedKeys()
    {
        QFETCH(int, pbe);
        QCA::SecureArray pass(7);
        pass[0]              = 'a';
        pass[2]              = char(0xff);
        pass[6]              = 'z';
        const auto encrypted = _private.toDER(pass, QCA::PBEAlgorithm(pbe));
        QVERIFY(!encrypted.isEmpty());
        QVERIFY(encrypted != _private.toDER(pass, QCA::PBEAlgorithm(pbe)));
        QCA::ConvertResult result;
        const auto         imported = QCA::PrivateKey::fromDER(encrypted, pass, &result, Provider);
        QCOMPARE(result, QCA::ConvertGood);
        QCOMPARE(imported.provider()->name(), Provider);
        QCOMPARE(imported.toPublicKey().toDER(), _public.toDER());
        QVERIFY(QCA::PrivateKey::fromDER(encrypted, QCA::SecureArray("a"), &result, Provider).isNull());
        QCOMPARE(result, QCA::ErrorPassphrase);
        const auto pem = _private.toPEM(pass, QCA::PBEAlgorithm(pbe));
        QVERIFY(pem.startsWith(QStringLiteral("-----BEGIN ENCRYPTED PRIVATE KEY-----")));
        QCOMPARE(QCA::PrivateKey::fromPEM(pem, pass, &result, Provider).toPublicKey().toDER(), _public.toDER());
        QCOMPARE(result, QCA::ConvertGood);
    }
    void malformedKeys()
    {
        QCA::ConvertResult result;
        const auto         valid = _private.toDER().toByteArray();
        for (const auto &bytes : QList<QByteArray> {QByteArray(),
                                                    valid.left(valid.size() - 1),
                                                    valid + 'x',
                                                    QByteArray::fromHex("30800201000000"),
                                                    QByteArray(65537, 0)}) {
            QVERIFY(QCA::PrivateKey::fromDER(QCA::SecureArray(bytes), {}, &result, Provider).isNull());
            QCOMPARE(result, QCA::ErrorDecode);
            QVERIFY(QCA::PublicKey::fromDER(bytes, &result, Provider).isNull());
        }
        for (int offset = 0; offset < valid.size(); offset += 17) {
            auto changed    = valid;
            changed[offset] = char(changed[offset] ^ 0x80);
            QVERIFY(QCA::PrivateKey::fromDER(QCA::SecureArray(changed), {}, &result, Provider).isNull());
            QCOMPARE(result, QCA::ErrorDecode);
        }
        const auto publicDER  = _public.toDER();
        auto       restricted = publicDER;
        const int  rsaOID     = restricted.indexOf(QByteArray::fromHex("06092a864886f70d010101"));
        QVERIFY(rsaOID >= 0);
        restricted[rsaOID + 10] = 10; // RSA-PSS restricted key, not rsaEncryption.
        QVERIFY(QCA::PublicKey::fromDER(restricted, &result, Provider).isNull());
        // Reject excessive KDF work before deriving any key.
        const auto params    = der(0x30, der(4, QByteArray(16, 's')) + QByteArray::fromHex("02047fffffff"));
        const auto kdf       = der(0x30, oid("2a864886f70d01050c") + params);
        const auto cipher    = der(0x30, oid("60864801650304012a") + der(4, QByteArray(16, 'i')));
        const auto algorithm = der(0x30, oid("2a864886f70d01050d") + der(0x30, kdf + cipher));
        const auto hostile   = der(0x30, algorithm + der(4, QByteArray(16, 0)));
        QVERIFY(QCA::PrivateKey::fromDER(QCA::SecureArray(hostile), {}, &result, Provider).isNull());
        QCOMPARE(result, QCA::ErrorDecode);
        for (const auto &pem :
             QStringList {QStringLiteral("prefix") + _private.toPEM(),
                          _private.toPEM() + QStringLiteral("suffix"),
                          QStringLiteral(
                              "-----BEGIN PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nAAAA\n-----END PRIVATE KEY-----\n"),
                          QString(131073, QLatin1Char('A'))})
            QVERIFY(QCA::PrivateKey::fromPEM(pem, {}, &result, Provider).isNull());
        QVERIFY(!QCA::PrivateKey::supportedPBEAlgorithms(Provider).contains(QCA::PBES2_DES_SHA1));
    }

private:
    QCA::Initializer _qca;
    QCA::PrivateKey  _private;
    QCA::PublicKey   _public;
};
QTEST_GUILESS_MAIN(WinCryptoRSATest)
#include "wincryptorsaunittest.moc"
