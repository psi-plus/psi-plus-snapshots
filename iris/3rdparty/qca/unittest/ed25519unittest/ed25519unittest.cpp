#include <QTest>
#include <QtCrypto>

#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif

// RFC 8032, section 7.1, test vector 1: empty-message Ed25519.
class Ed25519UnitTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void rfc8032Vector();
    void wrongSchemeAndTampering();
    void pemAndDerRoundtrip();
    void generatesEd25519();
private:
    QCA::Initializer _qca;
    static QCA::SignatureAlgorithm algorithm()
    {
        return {QCA::SignatureScheme::Ed25519, QCA::SignatureDigest::None};
    }
    static QCA::PrivateKey privateKey()
    {
        // PKCS#8 wrapping the RFC 8032 32-byte private seed.
        return QCA::PrivateKey::fromDER(QCA::SecureArray(QByteArray::fromHex(
            "302e020100300506032b657004220420"
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")));
    }
    static QCA::PublicKey publicKey()
    {
        // SubjectPublicKeyInfo wrapping the matching RFC public key.
        return QCA::PublicKey::fromDER(QByteArray::fromHex(
            "302a300506032b6570032100"
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    }
};

void Ed25519UnitTest::rfc8032Vector()
{
    QVERIFY2(QCA::isSupported("pkey"), "qca-ossl provider unavailable");
    QVERIFY(QCA::PKey::supportedIOTypes().contains(QCA::PKey::Ed25519));
    QCA::PrivateKey secret = privateKey();
    QCA::PublicKey publicPart = publicKey();
    QVERIFY(!secret.isNull());
    QVERIFY(!publicPart.isNull());
    QVERIFY(secret.isEd25519());
    QVERIFY(publicPart.isEd25519());
    QVERIFY(secret.canSign());
    QVERIFY(publicPart.canVerify());
    QVERIFY(!publicPart.isPrivate());
    QVERIFY(secret.isPrivate());

    const QByteArray signature = QByteArray::fromHex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901"
        "555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    QCOMPARE(signature.size(), 64);
    QCOMPARE(secret.signMessage(QByteArray(), algorithm()), signature);
    QVERIFY(publicPart.verifyMessage(QByteArray(), signature, algorithm()));
    QCA::PublicKey derived = secret.toPublicKey();
    QVERIFY(!derived.isNull());
    QVERIFY(derived.isEd25519());
    QVERIFY(!derived.isPrivate());
    QVERIFY(derived.verifyMessage(QByteArray(), signature, algorithm()));
}

void Ed25519UnitTest::wrongSchemeAndTampering()
{
    QCA::PrivateKey secret = privateKey();
    QCA::PublicKey publicPart = publicKey();
    QVERIFY(!secret.isNull());
    QVERIFY(!publicPart.isNull());
    const QByteArray payload("MatterLeast plugin signature content");
    QByteArray signature = secret.signMessage(payload, algorithm());
    QCOMPARE(signature.size(), 64);
    QVERIFY(publicPart.verifyMessage(payload, signature, algorithm()));
    QVERIFY(!publicPart.verifyMessage(QByteArray(payload).append("tampered"), signature, algorithm()));
    signature[0] = static_cast<char>(signature[0] ^ 1);
    QVERIFY(!publicPart.verifyMessage(payload, signature, algorithm()));
    const QCA::SignatureAlgorithm wrongScheme {
        QCA::SignatureScheme::RSA_PKCS1v15, QCA::SignatureDigest::SHA256
    };
    QVERIFY(secret.signMessage(payload, wrongScheme).isEmpty());
    QVERIFY(!publicPart.verifyMessage(payload, signature, wrongScheme));
    const QCA::SignatureAlgorithm wrongDigest {
        QCA::SignatureScheme::Ed25519, QCA::SignatureDigest::SHA512
    };
    QVERIFY(secret.signMessage(payload, wrongDigest).isEmpty());
    QVERIFY(!publicPart.verifyMessage(payload, signature, wrongDigest));
}

void Ed25519UnitTest::pemAndDerRoundtrip()
{
    QCA::PrivateKey secret = privateKey();
    QCA::PublicKey publicPart = publicKey();
    QVERIFY(!secret.isNull());
    QVERIFY(!publicPart.isNull());
    QCA::ConvertResult result = QCA::ConvertGood;
    QCA::PrivateKey pemSecret = QCA::PrivateKey::fromPEM(secret.toPEM(), {}, &result);
    QCOMPARE(result, QCA::ConvertGood);
    QVERIFY(!pemSecret.isNull());
    QVERIFY(pemSecret.isEd25519());
    QCA::PublicKey pemPublic = QCA::PublicKey::fromPEM(publicPart.toPEM(), &result);
    QCOMPARE(result, QCA::ConvertGood);
    QVERIFY(!pemPublic.isNull());
    QVERIFY(pemPublic.isEd25519());
    QCA::PublicKey derPublic = QCA::PublicKey::fromDER(publicPart.toDER(), &result);
    QCOMPARE(result, QCA::ConvertGood);
    QVERIFY(!derPublic.isNull());
    const QByteArray msg("format roundtrip");
    const QByteArray sig = pemSecret.signMessage(msg, algorithm());
    QVERIFY(pemPublic.verifyMessage(msg, sig, algorithm()));
    QVERIFY(derPublic.verifyMessage(msg, sig, algorithm()));
}

void Ed25519UnitTest::generatesEd25519()
{
    QCA::KeyGenerator generator;
    QCA::PrivateKey key = generator.createEd25519(QStringLiteral("qca-ossl"));
    QVERIFY(!key.isNull());
    QVERIFY(key.isEd25519());
    QCA::PublicKey pub = key.toPublicKey();
    QVERIFY(!pub.isNull());
    const QByteArray data("generated key test");
    QByteArray sig = key.signMessage(data, algorithm());
    QCOMPARE(sig.size(), 64);
    QVERIFY(pub.verifyMessage(data, sig, algorithm()));
}

QTEST_APPLESS_MAIN(Ed25519UnitTest)
#include "ed25519unittest.moc"
