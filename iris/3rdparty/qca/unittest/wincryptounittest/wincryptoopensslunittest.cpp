// SPDX-License-Identifier: LGPL-2.1-or-later
// OpenSSL is an independent test peer, never a dependency of qca-wincrypto.
#include <QQueue>
#include <QTest>
#include <QtCrypto>
#include <cstring>
#include <memory>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif

namespace {
const QString Provider = QStringLiteral("qca-wincrypto");
struct Packets
{
    QQueue<QByteArray> input, output;
    int                mtu = 1200;
};
int readPacket(BIO *bio, char *out, int size)
{
    BIO_clear_retry_flags(bio);
    auto &packets = *static_cast<Packets *>(BIO_get_data(bio));
    if (packets.input.isEmpty()) {
        BIO_set_retry_read(bio);
        return -1;
    }
    const auto packet = packets.input.dequeue();
    const int  count  = qMin(size, int(packet.size()));
    if (count > 0)
        std::memcpy(out, packet.constData(), std::size_t(count));
    return count;
}
int writePacket(BIO *bio, const char *data, int size)
{
    BIO_clear_retry_flags(bio);
    static_cast<Packets *>(BIO_get_data(bio))->output.enqueue(QByteArray(data, size));
    return size;
}
long controlPacket(BIO *bio, int operation, long value, void *)
{
    auto &packets = *static_cast<Packets *>(BIO_get_data(bio));
    switch (operation) {
    case BIO_CTRL_FLUSH:
    case BIO_CTRL_DGRAM_SET_CONNECTED:
    case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
        return 1;
    case BIO_CTRL_DGRAM_QUERY_MTU:
    case BIO_CTRL_DGRAM_GET_MTU:
        return packets.mtu;
    case BIO_CTRL_DGRAM_SET_MTU:
        packets.mtu = int(value);
        return value;
    case BIO_CTRL_PENDING:
        return packets.input.isEmpty() ? 0 : packets.input.head().size();
    default:
        return 0;
    }
}
struct PacketMethod
{
    BIO_METHOD *value = BIO_meth_new(BIO_TYPE_DGRAM, "QCA independent datagram peer");
    PacketMethod()
    {
        BIO_meth_set_write(value, writePacket);
        BIO_meth_set_read(value, readPacket);
        BIO_meth_set_ctrl(value, controlPacket);
        BIO_meth_set_create(value, [](BIO *bio) {
            BIO_set_init(bio, 1);
            BIO_set_data(bio, new Packets);
            return 1;
        });
        BIO_meth_set_destroy(value, [](BIO *bio) {
            delete static_cast<Packets *>(BIO_get_data(bio));
            return 1;
        });
    }
    ~PacketMethod()
    {
        BIO_meth_free(value);
    }
};
int password(char *out, int size, int, void *arg)
{
    const auto &pass = *static_cast<QByteArray *>(arg);
    if (pass.size() > size)
        return 0;
    std::memcpy(out, pass.constData(), std::size_t(pass.size()));
    return int(pass.size());
}
QByteArray bioBytes(BIO *bio)
{
    char      *bytes = nullptr;
    const auto size  = BIO_get_mem_data(bio, &bytes);
    return size > 0 && size < 65536 ? QByteArray(bytes, int(size)) : QByteArray();
}
}
class WinCryptoOpenSSLTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        qInfo() << "Independent crypto peer:" << OpenSSL_version(OPENSSL_VERSION);
    }
    void interoperability_data();
    void interoperability();
    void streamClientCertificateRequest();
    void rsaOperations();
    void rsaSerialization();

private:
    QCA::Initializer _qca;
};
void WinCryptoOpenSSLTest::rsaOperations()
{
    auto native = QCA::KeyGenerator().createRSA(2048, 65537, Provider);
    QVERIFY(!native.isNull());
    QCOMPARE(native.provider()->name(), Provider);
    auto        publicKey = native.toPublicKey();
    const auto  exported  = native.toDER();
    const auto *data      = reinterpret_cast<const unsigned char *>(exported.constData());
    auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(d2i_AutoPrivateKey(nullptr, &data, exported.size()),
                                                                   EVP_PKEY_free);
    QVERIFY(key);
    QCOMPARE(data, reinterpret_cast<const unsigned char *>(exported.constData() + exported.size()));
    for (auto algorithm : {QCA::EME_PKCS1v15, QCA::EME_PKCS1_OAEP, QCA::EME_NO_PADDING}) {
        const int padding = algorithm == QCA::EME_PKCS1v15 ? RSA_PKCS1_PADDING
            : algorithm == QCA::EME_PKCS1_OAEP             ? RSA_PKCS1_OAEP_PADDING
                                                           : RSA_NO_PADDING;
        for (int length : {0, 42, publicKey.maximumEncryptSize(algorithm)}) {
            QByteArray plain(length, 'p');
            if (algorithm == QCA::EME_NO_PADDING) {
                plain      = QByteArray(256, 0);
                plain[255] = 3;
            }
            auto ctx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(EVP_PKEY_CTX_new(key.get(), nullptr),
                                                                                   EVP_PKEY_CTX_free);
            QVERIFY(ctx);
            QVERIFY(EVP_PKEY_encrypt_init(ctx.get()) > 0);
            QVERIFY(EVP_PKEY_CTX_set_rsa_padding(ctx.get(), padding) > 0);
            QByteArray  encrypted(256, 0);
            std::size_t size = encrypted.size();
            QVERIFY(EVP_PKEY_encrypt(ctx.get(),
                                     reinterpret_cast<unsigned char *>(encrypted.data()),
                                     &size,
                                     reinterpret_cast<const unsigned char *>(plain.constData()),
                                     std::size_t(plain.size())) > 0);
            QCA::SecureArray decrypted;
            QVERIFY(native.decrypt(QCA::SecureArray(encrypted), &decrypted, algorithm));
            QCOMPARE(decrypted.toByteArray(), plain);
            const auto nativeEncrypted = publicKey.encrypt(QCA::SecureArray(plain), algorithm);
            QCOMPARE(nativeEncrypted.size(), 256);
            QVERIFY(EVP_PKEY_decrypt_init(ctx.get()) > 0);
            QVERIFY(EVP_PKEY_CTX_set_rsa_padding(ctx.get(), padding) > 0);
            QByteArray result(256, 0);
            size = result.size();
            QVERIFY(EVP_PKEY_decrypt(ctx.get(),
                                     reinterpret_cast<unsigned char *>(result.data()),
                                     &size,
                                     reinterpret_cast<const unsigned char *>(nativeEncrypted.constData()),
                                     nativeEncrypted.size()) > 0);
            result.resize(int(size));
            QCOMPARE(result, plain);
        }
    }
    for (bool pss : {false, true})
        for (const auto digest : {QCA::SignatureDigest::SHA1,
                                  QCA::SignatureDigest::SHA256,
                                  QCA::SignatureDigest::SHA384,
                                  QCA::SignatureDigest::SHA512})
            for (int salt : {0, 32}) {
                const auto                   *md = digest == QCA::SignatureDigest::SHA1 ? EVP_sha1()
                    : digest == QCA::SignatureDigest::SHA256                            ? EVP_sha256()
                    : digest == QCA::SignatureDigest::SHA384                            ? EVP_sha384()
                                                                                        : EVP_sha512();
                const QCA::SignatureAlgorithm algorithm {pss ? QCA::SignatureScheme::RSA_PSS
                                                             : QCA::SignatureScheme::RSA_PKCS1v15,
                                                         digest,
                                                         pss ? digest : QCA::SignatureDigest::Unknown,
                                                         pss ? salt : -1,
                                                         pss ? 1 : -1};
                const QByteArray              message(1024 * 1024 + 17, 'm');
                const auto                    signature = native.signMessage(QCA::SecureArray(message), algorithm);
                QCOMPARE(signature.size(), 256);
                auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
                QVERIFY(ctx);
                EVP_PKEY_CTX *pkey = nullptr;
                QVERIFY(EVP_DigestVerifyInit(ctx.get(), &pkey, md, nullptr, key.get()) > 0);
                if (pss) {
                    QVERIFY(EVP_PKEY_CTX_set_rsa_padding(pkey, RSA_PKCS1_PSS_PADDING) > 0);
                    QVERIFY(EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey, salt) > 0);
                }
                QVERIFY(EVP_DigestVerify(ctx.get(),
                                         reinterpret_cast<const unsigned char *>(signature.constData()),
                                         signature.size(),
                                         reinterpret_cast<const unsigned char *>(message.constData()),
                                         message.size()) == 1);
                QVERIFY(EVP_DigestSignInit(ctx.get(), &pkey, md, nullptr, key.get()) > 0);
                if (pss) {
                    QVERIFY(EVP_PKEY_CTX_set_rsa_padding(pkey, RSA_PKCS1_PSS_PADDING) > 0);
                    QVERIFY(EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey, salt) > 0);
                }
                QByteArray  peerSignature(256, 0);
                std::size_t size = peerSignature.size();
                QVERIFY(EVP_DigestSign(ctx.get(),
                                       reinterpret_cast<unsigned char *>(peerSignature.data()),
                                       &size,
                                       reinterpret_cast<const unsigned char *>(message.constData()),
                                       message.size()) > 0);
                peerSignature.resize(int(size));
                QVERIFY(publicKey.verifyMessage(QCA::SecureArray(message), peerSignature, algorithm));
            }
    for (const QByteArray plain : {QByteArray(), QByteArray("raw recovery")}) {
        const auto signature = native.signMessage(QCA::SecureArray(plain), QCA::EMSA3_Raw);
        auto ctx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(EVP_PKEY_CTX_new(key.get(), nullptr),
                                                                               EVP_PKEY_CTX_free);
        QVERIFY(ctx);
        // Check the complete raw PKCS#1 block with the peer's public primitive.
        // This includes the zero-length case rejected by newer verify-recover APIs.
        QVERIFY(EVP_PKEY_encrypt_init(ctx.get()) > 0);
        QVERIFY(EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_NO_PADDING) > 0);
        QByteArray  recovered(256, 0);
        std::size_t size = recovered.size();
        QVERIFY2(EVP_PKEY_encrypt(ctx.get(),
                                  reinterpret_cast<unsigned char *>(recovered.data()),
                                  &size,
                                  reinterpret_cast<const unsigned char *>(signature.constData()),
                                  signature.size()) > 0,
                 ERR_error_string(ERR_peek_last_error(), nullptr));
        recovered.resize(int(size));
        QByteArray expected(256 - plain.size() - 1, char(0xff));
        expected[0] = 0;
        expected[1] = 1;
        expected += char(0);
        expected += plain;
        QCOMPARE(recovered, expected);
    }
}
void WinCryptoOpenSSLTest::rsaSerialization()
{
    auto ctx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                                                                           EVP_PKEY_CTX_free);
    QVERIFY(ctx);
    QVERIFY(EVP_PKEY_keygen_init(ctx.get()) > 0);
    QVERIFY(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) > 0);
    EVP_PKEY *generated = nullptr;
    QVERIFY(EVP_PKEY_keygen(ctx.get(), &generated) > 0);
    auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(generated, EVP_PKEY_free);
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
    QVERIFY(bio);
    QVERIFY(i2d_PrivateKey_bio(bio.get(), key.get()) > 0);
    QCA::ConvertResult result;
    auto               native = QCA::PrivateKey::fromDER(QCA::SecureArray(bioBytes(bio.get())), {}, &result, Provider);
    QCOMPARE(result, QCA::ConvertGood);
    QCOMPARE(native.provider()->name(), Provider);
    QVERIFY(BIO_reset(bio.get()) > 0);
    QVERIFY(i2d_PUBKEY_bio(bio.get(), key.get()) > 0);
    QCOMPARE(QCA::PublicKey::fromDER(bioBytes(bio.get()), &result, Provider).toDER(), native.toPublicKey().toDER());
    QCOMPARE(result, QCA::ConvertGood);
    // Include embedded NUL and a non-ASCII byte in a binary passphrase.
    QByteArray pass("binary\0password\xff", 16);
    for (const auto *cipher : {EVP_des_ede3_cbc(), EVP_aes_128_cbc(), EVP_aes_192_cbc(), EVP_aes_256_cbc()}) {
        QVERIFY(BIO_reset(bio.get()) > 0);
        // OpenSSL's current PBES2 default PRF is HMAC-SHA-256.
        QVERIFY(i2d_PKCS8PrivateKey_bio(bio.get(), key.get(), cipher, pass.data(), int(pass.size()), nullptr, nullptr) >
                0);
        auto imported =
            QCA::PrivateKey::fromDER(QCA::SecureArray(bioBytes(bio.get())), QCA::SecureArray(pass), &result, Provider);
        QCOMPARE(result, QCA::ConvertGood);
        QCOMPARE(imported.toPublicKey().toDER(), native.toPublicKey().toDER());
    }
    for (int prf : {NID_hmacWithSHA1, NID_hmacWithSHA256, NID_hmacWithSHA384, NID_hmacWithSHA512}) {
        auto info = std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)>(EVP_PKEY2PKCS8(key.get()),
                                                                                              PKCS8_PRIV_KEY_INFO_free);
        QVERIFY(info);
        auto algorithm = std::unique_ptr<X509_ALGOR, decltype(&X509_ALGOR_free)>(
            PKCS5_pbe2_set_iv(EVP_aes_256_cbc(), 4096, nullptr, 16, nullptr, prf), X509_ALGOR_free);
        QVERIFY(algorithm);
        auto encrypted = std::unique_ptr<X509_SIG, decltype(&X509_SIG_free)>(
            PKCS8_set0_pbe(pass.constData(), int(pass.size()), info.get(), algorithm.get()), X509_SIG_free);
        QVERIFY(encrypted);
        algorithm.release(); // Ownership transfers on success; info remains caller-owned.
        QVERIFY(BIO_reset(bio.get()) > 0);
        QVERIFY(i2d_PKCS8_bio(bio.get(), encrypted.get()) > 0);
        auto imported =
            QCA::PrivateKey::fromDER(QCA::SecureArray(bioBytes(bio.get())), QCA::SecureArray(pass), &result, Provider);
        QCOMPARE(result, QCA::ConvertGood);
        QCOMPARE(imported.toPublicKey().toDER(), native.toPublicKey().toDER());
    }
    for (const auto algorithm :
         {QCA::PBEDefault, QCA::PBES2_AES128_SHA1, QCA::PBES2_AES192_SHA1, QCA::PBES2_AES256_SHA1}) {
        const auto encrypted = native.toDER(QCA::SecureArray(pass), algorithm);
        QVERIFY(!encrypted.isEmpty());
        auto input = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new_mem_buf(encrypted.constData(), encrypted.size()),
                                                               BIO_free);
        auto imported = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
            d2i_PKCS8PrivateKey_bio(input.get(), nullptr, password, &pass), EVP_PKEY_free);
        QVERIFY(imported);
        const QCA::SecureArray message("peer imported key");
        const auto             signature = native.signMessage(message, QCA::EMSA3_SHA256);
        auto verify = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        QVERIFY(verify);
        QVERIFY(EVP_DigestVerifyInit(verify.get(), nullptr, EVP_sha256(), nullptr, imported.get()) > 0);
        QCOMPARE(EVP_DigestVerify(verify.get(),
                                  reinterpret_cast<const unsigned char *>(signature.constData()),
                                  signature.size(),
                                  reinterpret_cast<const unsigned char *>(message.constData()),
                                  message.size()),
                 1);
    }
}

void WinCryptoOpenSSLTest::streamClientCertificateRequest()
{
    // These stream TLS tests are optional OpenSSL interoperability tests.
    // WinCrypto's production and native-only test builds require no OpenSSL.
    for (const bool required : {false, true}) {
        auto keyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        QVERIFY(keyContext);
        QVERIFY(EVP_PKEY_keygen_init(keyContext.get()) > 0);
        QVERIFY(EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 2048) > 0);
        EVP_PKEY *generated = nullptr;
        QVERIFY(EVP_PKEY_keygen(keyContext.get(), &generated) > 0);
        auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(generated, EVP_PKEY_free);
        auto cert = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
        QVERIFY(cert);
        QVERIFY(X509_set_version(cert.get(), 2) == 1);
        QVERIFY(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), required ? 2 : 1) == 1);
        QVERIFY(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60));
        QVERIFY(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600));
        QVERIFY(X509_set_pubkey(cert.get(), key.get()) == 1);
        auto *name = X509_get_subject_name(cert.get());
        QVERIFY(name);
        QVERIFY(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                            reinterpret_cast<const unsigned char *>("qca.test"), -1, -1, 0) == 1);
        QVERIFY(X509_set_issuer_name(cert.get(), name) == 1);
        X509V3_CTX extensions {};
        X509V3_set_ctx(&extensions, cert.get(), cert.get(), nullptr, nullptr, 0);
        auto san = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
            X509V3_EXT_conf_nid(nullptr, &extensions, NID_subject_alt_name,
                                const_cast<char *>("DNS:qca.test")), X509_EXTENSION_free);
        QVERIFY(san && X509_add_ext(cert.get(), san.get(), -1) == 1);
        QVERIFY(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0);

        auto ctx = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
        QVERIFY(ctx);
        QVERIFY(SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION));
        QVERIFY(SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION));
        QVERIFY(SSL_CTX_use_certificate(ctx.get(), cert.get()) == 1);
        QVERIFY(SSL_CTX_use_PrivateKey(ctx.get(), key.get()) == 1);
        QVERIFY(SSL_CTX_check_private_key(ctx.get()));
        // SSL_VERIFY_PEER sends CertificateRequest. FAIL_IF_NO_PEER_CERT is
        // intentionally absent for the optional-client-certificate case.
        SSL_CTX_set_verify(ctx.get(),
                           SSL_VERIFY_PEER | (required ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0), nullptr);
        auto ssl = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(ctx.get()), SSL_free);
        QVERIFY(ssl);
        BIO *input = BIO_new(BIO_s_mem()), *output = BIO_new(BIO_s_mem());
        QVERIFY(input && output);
        BIO_set_mem_eof_return(input, -1);
        SSL_set_bio(ssl.get(), input, output); // SSL owns both BIOs.
        SSL_set_accept_state(ssl.get());

        const int derLength = i2d_X509(cert.get(), nullptr);
        QVERIFY(derLength > 0);
        QByteArray der(derLength, 0);
        auto *derCursor = reinterpret_cast<unsigned char *>(der.data());
        QCOMPARE(i2d_X509(cert.get(), &derCursor), derLength);
        auto *provider = QCA::findProvider(Provider);
        QVERIFY(provider);
        auto native = std::unique_ptr<QCA::TLSContext>(
            static_cast<QCA::TLSContext *>(provider->createContext(QStringLiteral("tls"))));
        QVERIFY(native);
        native->setup(false, QStringLiteral("qca.test"), false);
        QCA::CertificateCollection trust;
        trust.addCertificate(QCA::Certificate::fromDER(der, nullptr, Provider));
        native->setTrustedCertificates(trust);
        native->start();
        QVERIFY(native->waitForResultsReady(1000));
        QCOMPARE(native->result(), QCA::TLSContext::Success);
        native->update({}, {});
        native->waitForResultsReady(1000);
        bool rejected = false;
        for (int round = 0; round < 50; ++round) {
            const auto outgoing = native->to_net();
            if (!outgoing.isEmpty())
                QCOMPARE(BIO_write(SSL_get_rbio(ssl.get()), outgoing.constData(), outgoing.size()), outgoing.size());
            if (!rejected && !SSL_is_init_finished(ssl.get())) {
                const int status = SSL_do_handshake(ssl.get());
                if (status <= 0) {
                    const int error = SSL_get_error(ssl.get(), status);
                    rejected = error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE;
                }
            }
            QByteArray reply;
            while (BIO_ctrl_pending(SSL_get_wbio(ssl.get())) > 0) {
                char bytes[4096];
                const int count = BIO_read(SSL_get_wbio(ssl.get()), bytes, sizeof(bytes));
                QVERIFY(count > 0);
                reply.append(bytes, count);
            }
            for (int offset = 0; offset < reply.size(); offset += 7) {
                native->update(reply.mid(offset, 7), {});
                native->waitForResultsReady(1000);
                if (native->result() == QCA::TLSContext::Error)
                    break;
            }
            if (rejected || native->result() == QCA::TLSContext::Error ||
                (SSL_is_init_finished(ssl.get()) && native->result() == QCA::TLSContext::Success))
                break;
        }
        if (required) {
            // A server requiring a client certificate must not establish
            // application data, regardless of which peer rejects first.
            QVERIFY(rejected || native->result() == QCA::TLSContext::Error);
            QVERIFY(!SSL_is_init_finished(ssl.get()));
            continue;
        }
        QVERIFY(!rejected);
        QVERIFY(SSL_is_init_finished(ssl.get()));
        QCOMPARE(native->result(), QCA::TLSContext::Success);
        QCOMPARE(native->peerCertificateValidity(), QCA::ValidityGood);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        auto peerCertificate = std::unique_ptr<X509, decltype(&X509_free)>(
            SSL_get1_peer_certificate(ssl.get()), X509_free);
#else
        auto peerCertificate = std::unique_ptr<X509, decltype(&X509_free)>(
            SSL_get_peer_certificate(ssl.get()), X509_free);
#endif
        QVERIFY(!peerCertificate); // No Windows user certificate may be sent.
        const QByteArray application("authenticated and encrypted application data");
        native->update({}, application);
        QVERIFY(native->waitForResultsReady(1000));
        QCOMPARE(native->result(), QCA::TLSContext::Success);
        const auto record = native->to_net();
        QCOMPARE(BIO_write(SSL_get_rbio(ssl.get()), record.constData(), record.size()), record.size());
        char decrypted[128] = {};
        const int read = SSL_read(ssl.get(), decrypted, sizeof(decrypted));
        QCOMPARE(read, application.size());
        QCOMPARE(QByteArray(decrypted, read), application);
    }
}

void WinCryptoOpenSSLTest::interoperability_data()
{
    QTest::addColumn<bool>("nativeServer");
    QTest::addColumn<QString>("profile");
    for (bool server : {false, true})
        for (const auto &profile : {QString(),
                                    QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80"),
                                    QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_32"),
                                    QStringLiteral("SRTP_AEAD_AES_128_GCM"),
                                    QStringLiteral("SRTP_AEAD_AES_256_GCM")}) {
            const QByteArray name =
                (server ? QByteArray("native-server:") : QByteArray("native-client:")) + profile.toLatin1();
            QTest::newRow(name.constData()) << server << profile;
        }
}
void WinCryptoOpenSSLTest::interoperability()
{
    QFETCH(bool, nativeServer);
    QFETCH(QString, profile);
    auto ctx = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(SSL_CTX_new(DTLS_method()), SSL_CTX_free);
    QVERIFY(ctx);
    QVERIFY(SSL_CTX_set_min_proto_version(ctx.get(), DTLS1_2_VERSION));
    QVERIFY(SSL_CTX_set_max_proto_version(ctx.get(), DTLS1_2_VERSION));
    QVERIFY(SSL_CTX_set_cipher_list(ctx.get(), "ECDHE-RSA-AES128-GCM-SHA256"));
    QVERIFY(SSL_CTX_use_certificate_file(ctx.get(), "dtlssrtp-certs/default.crt", SSL_FILETYPE_PEM));
    QVERIFY(SSL_CTX_use_PrivateKey_file(ctx.get(), "dtlssrtp-certs/default.key", SSL_FILETYPE_PEM));
    QVERIFY(SSL_CTX_check_private_key(ctx.get()));
    // Make a self-signed fixture with both EKUs in memory. The older shared
    // certificate fixture is server-only and cannot authenticate a DTLS client.
    auto peerCertificate = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
    QVERIFY(peerCertificate);
    QVERIFY(X509_set_version(peerCertificate.get(), 2));
    QVERIFY(ASN1_INTEGER_set(X509_get_serialNumber(peerCertificate.get()), 42));
    QVERIFY(X509_gmtime_adj(X509_getm_notBefore(peerCertificate.get()), -60));
    QVERIFY(X509_gmtime_adj(X509_getm_notAfter(peerCertificate.get()), 86400));
    QVERIFY(X509_set_pubkey(peerCertificate.get(), SSL_CTX_get0_privatekey(ctx.get())));
    auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    QVERIFY(subject);
    QVERIFY(X509_NAME_add_entry_by_txt(
        subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("openssl.test"), -1, -1, 0));
    QVERIFY(X509_set_subject_name(peerCertificate.get(), subject.get()));
    auto eku = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_ext_key_usage, "serverAuth,clientAuth"), X509_EXTENSION_free);
    QVERIFY(eku);
    QVERIFY(X509_add_ext(peerCertificate.get(), eku.get(), -1));
    QVERIFY(X509_set_issuer_name(peerCertificate.get(), X509_get_subject_name(peerCertificate.get())));
    QVERIFY(X509_sign(peerCertificate.get(), SSL_CTX_get0_privatekey(ctx.get()), EVP_sha256()) > 0);
    QVERIFY(SSL_CTX_use_certificate(ctx.get(), peerCertificate.get()));
    // Each side pins the other side's exact test certificate.
    SSL_CTX_set_verify(
        ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, [](int, X509_STORE_CTX *) { return 1; });
    const auto opensslProfile = QString(profile).replace(QStringLiteral("_HMAC_SHA1_"), QStringLiteral("_SHA1_"));
    if (!profile.isEmpty() && SSL_CTX_set_tlsext_use_srtp(ctx.get(), opensslProfile.toLatin1().constData()) != 0)
        QSKIP("The independent OpenSSL peer does not provide this SRTP profile");
    PacketMethod method;
    auto         ssl = std::unique_ptr<SSL, decltype(&SSL_free)>(SSL_new(ctx.get()), SSL_free);
    QVERIFY(ssl);
    auto bio = BIO_new(method.value);
    QVERIFY(bio);
    auto &packets = *static_cast<Packets *>(BIO_get_data(bio));
    SSL_set_bio(ssl.get(), bio, bio);
    SSL_set_mtu(ssl.get(), 1200);
    if (nativeServer)
        SSL_set_connect_state(ssl.get());
    else
        SSL_set_accept_state(ssl.get());
    QCA::TLS native(QCA::TLS::Datagram, nullptr, Provider);
    native.setPacketMTU(1200);
    const auto key = QCA::KeyGenerator().createRSA(2048, 65537, Provider);
    QVERIFY(!key.isNull());
    QCA::CertificateOptions options;
    QCA::CertificateInfo    info;
    info.insert(QCA::CommonName, QStringLiteral("qca.test"));
    options.setInfo(info);
    options.setSerialNumber(QCA::BigInteger(42));
    const auto now = QDateTime::currentDateTimeUtc();
    options.setValidityPeriod(now.addSecs(-60), now.addDays(1));
    options.setConstraints({QCA::DigitalSignature, QCA::KeyEncipherment, QCA::ClientAuth, QCA::ServerAuth});
    const QCA::Certificate certificate(options, key, Provider);
    QVERIFY(!certificate.isNull());
    QCA::CertificateChain chain;
    chain += certificate;
    native.setCertificate(chain, key);
    QCA::CertificateCollection trust;
    const int                  peerLength = i2d_X509(peerCertificate.get(), nullptr);
    QByteArray                 peerDER(peerLength, 0);
    auto                       peerOutput = reinterpret_cast<unsigned char *>(peerDER.data());
    QCOMPARE(i2d_X509(peerCertificate.get(), &peerOutput), peerLength);
    trust.addCertificate(QCA::Certificate::fromDER(peerDER, nullptr, Provider));
    native.setTrustedCertificates(trust);
    QVERIFY(native.setSRTPProfiles(profile.isEmpty() ? QStringList() : QStringList {profile}));
    bool complete = false, failed = false;
    connect(&native, &QCA::TLS::handshaken, &native, [&] {
        complete = true;
        native.continueAfterStep();
    });
    connect(&native, &QCA::TLS::peerCertificateAvailable, &native, &QCA::TLS::continueAfterStep);
    connect(&native, &QCA::TLS::error, &native, [&] { failed = true; });
    const auto transferToOpenSSL = [&] {
        QCoreApplication::processEvents();
        while (native.packetsOutgoingAvailable()) {
            const auto packet = native.readOutgoing();
            packets.input.enqueue(packet);
        }
    };
    const auto transferToNative = [&] {
        while (!packets.output.isEmpty())
            native.writeIncoming(packets.output.dequeue());
        QCoreApplication::processEvents();
    };
    if (nativeServer)
        native.startServer();
    else
        native.startClient();
    bool peerDone = false;
    for (int round = 0; round < 1000 && !(complete && peerDone) && !failed; ++round) {
        transferToOpenSSL();
        if (!peerDone) {
            const int result = SSL_do_handshake(ssl.get());
            if (result == 1)
                peerDone = true;
            else {
                const int error = SSL_get_error(ssl.get(), result);
                QVERIFY2(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
                         ERR_error_string(ERR_peek_last_error(), nullptr));
            }
        }
        transferToNative();
    }
    QVERIFY(!failed);
    QVERIFY(complete);
    QVERIFY(peerDone);
    QCOMPARE(native.peerIdentityResult(), QCA::TLS::Valid);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    auto remote = std::unique_ptr<X509, decltype(&X509_free)>(SSL_get1_peer_certificate(ssl.get()), X509_free);
#else
    auto remote = std::unique_ptr<X509, decltype(&X509_free)>(SSL_get_peer_certificate(ssl.get()), X509_free);
#endif
    QVERIFY(remote);
    const int  certLength = i2d_X509(remote.get(), nullptr);
    QByteArray remoteDER(certLength, 0);
    auto       derOutput = reinterpret_cast<unsigned char *>(remoteDER.data());
    QCOMPARE(i2d_X509(remote.get(), &derOutput), certLength);
    QCOMPARE(remoteDER, certificate.toDER());
    QCOMPARE(native.selectedSRTPProfile(), profile);
    if (profile.isEmpty()) {
        native.write(QByteArray("native datagram"));
        for (int round = 0; round < 10; ++round)
            transferToOpenSSL();
        char      plain[64] {};
        const int length = SSL_read(ssl.get(), plain, sizeof(plain));
        QCOMPARE(QByteArray(plain, length > 0 ? length : 0), QByteArray("native datagram"));
        QCOMPARE(SSL_write(ssl.get(), "OpenSSL reply", 13), 13);
        for (int round = 0; round < 10; ++round)
            transferToNative();
        QCOMPARE(native.packetsAvailable(), 1);
        QCOMPARE(native.read(), QByteArray("OpenSSL reply"));
    } else {
        const auto selected = SSL_get_selected_srtp_profile(ssl.get());
        QVERIFY(selected);
        QCOMPARE(QString::fromLatin1(selected->name), opensslProfile);
        const int        keyLength  = profile == QLatin1String("SRTP_AEAD_AES_256_GCM") ? 32 : 16;
        const int        saltLength = profile.contains(QLatin1String("GCM")) ? 12 : 14;
        QCA::SecureArray exported(2 * (keyLength + saltLength));
        const char       label[] = "EXTRACTOR-dtls_srtp";
        QVERIFY(SSL_export_keying_material(ssl.get(),
                                           reinterpret_cast<unsigned char *>(exported.data()),
                                           exported.size(),
                                           label,
                                           sizeof(label) - 1,
                                           nullptr,
                                           0,
                                           0));
        const auto material = native.srtpKeyingMaterial();
        const auto matches  = [&](const QCA::SecureArray &value, int offset) {
            return value.size() && std::memcmp(value.constData(), exported.constData() + offset, value.size()) == 0;
        };
        const int local = nativeServer ? 1 : 0, peer = 1 - local;
        QVERIFY(matches(material.localMasterKey(), local * keyLength));
        QVERIFY(matches(material.remoteMasterKey(), peer * keyLength));
        QVERIFY(matches(material.localMasterSalt(), 2 * keyLength + local * saltLength));
        QVERIFY(matches(material.remoteMasterSalt(), 2 * keyLength + peer * saltLength));
    }
}
QTEST_GUILESS_MAIN(WinCryptoOpenSSLTest)
#include "wincryptoopensslunittest.moc"
