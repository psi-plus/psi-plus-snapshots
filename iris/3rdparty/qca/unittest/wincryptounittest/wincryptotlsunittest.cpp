// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtCrypto>
#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <winternl.h>
#define SCHANNEL_USE_BLACKLISTS
#include <cstring>
#include <memory>
#include <schannel.h>
#include <wincrypt.h>

namespace {
const QString ProviderName = QStringLiteral("qca-wincrypto");

// Independent native TLS server. Schannel needs a named private key: create a
// unique test-only Windows user key and delete it in the destructor. The
// certificate stays in memory and is never added to Windows trusted roots.
class NativeServer
{
public:
    explicit NativeServer(bool expired = false, bool tls13 = false, bool conflictingSAN = false,
                          bool requestClientCertificate = false)
        : _requestClientCertificate(requestClientCertificate)
    {
        _keyName = QStringLiteral("qca-wincrypto-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto *keyName = reinterpret_cast<const wchar_t *>(_keyName.utf16());
        if (!CryptAcquireContextW(
                &_provider, keyName, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET | CRYPT_SILENT) ||
            !CryptGenKey(_provider, AT_KEYEXCHANGE, (2048 << 16), &_key))
            return;
        DWORD nameSize = 0;
        CertStrToNameW(X509_ASN_ENCODING, L"CN=qca.test", CERT_X500_NAME_STR, nullptr, nullptr, &nameSize, nullptr);
        QByteArray name(int(nameSize), 0);
        if (!CertStrToNameW(X509_ASN_ENCODING,
                            L"CN=qca.test",
                            CERT_X500_NAME_STR,
                            nullptr,
                            reinterpret_cast<PBYTE>(name.data()),
                            &nameSize,
                            nullptr))
            return;
        CERT_NAME_BLOB             subject {nameSize, reinterpret_cast<PBYTE>(name.data())};
        CRYPT_ALGORITHM_IDENTIFIER signature {const_cast<LPSTR>(szOID_RSA_SHA256RSA), {0, nullptr}};
        const auto                 systemTime = [](const QDateTime &time) {
            SYSTEMTIME value {};
            value.wYear   = WORD(time.date().year());
            value.wMonth  = WORD(time.date().month());
            value.wDay    = WORD(time.date().day());
            value.wHour   = WORD(time.time().hour());
            value.wMinute = WORD(time.time().minute());
            value.wSecond = WORD(time.time().second());
            return value;
        };
        const auto          now   = QDateTime::currentDateTimeUtc();
        auto                start = systemTime(now.addDays(expired ? -2 : -1));
        auto                end   = systemTime(now.addDays(expired ? -1 : 1));
        CRYPT_KEY_PROV_INFO keyInfo {};
        keyInfo.pwszContainerName = const_cast<wchar_t *>(keyName);
        keyInfo.pwszProvName      = const_cast<wchar_t *>(MS_ENH_RSA_AES_PROV_W);
        keyInfo.dwProvType        = PROV_RSA_AES;
        keyInfo.dwKeySpec         = AT_KEYEXCHANGE;
        auto       eku            = QByteArray::fromHex("300a06082b06010505070301");
        QByteArray san            = QByteArray::fromHex("300a8208");
        san += conflictingSAN ? QByteArray("san.test") : QByteArray("qca.test");
        CERT_EXTENSION  extensions[] {{const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE),
                                       FALSE,
                                       {DWORD(eku.size()), reinterpret_cast<BYTE *>(eku.data())}},
                                      {const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2),
                                       FALSE,
                                       {DWORD(san.size()), reinterpret_cast<BYTE *>(san.data())}}};
        CERT_EXTENSIONS extensionList {2, extensions};
        _cert =
            CertCreateSelfSignCertificate(_provider, &subject, 0, &keyInfo, &signature, &start, &end, &extensionList);
        if (!_cert) {
            qWarning("Create certificate failed: %lu", GetLastError());
            return;
        }
        SCH_CREDENTIALS creds {};
        creds.dwVersion = SCH_CREDENTIALS_VERSION;
        creds.cCreds    = 1;
        creds.paCred    = &_cert;
        TLS_PARAMETERS params {};
        params.grbitDisabledProtocols =
            SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1 | SP_PROT_TLS1_1 | (tls13 ? SP_PROT_TLS1_2 : SP_PROT_TLS1_3);
        creds.cTlsParameters = 1;
        creds.pTlsParameters = &params;
        creds.dwFlags        = SCH_USE_STRONG_CRYPTO;
        TimeStamp  expiry;
        const auto status = AcquireCredentialsHandleW(nullptr,
                                                      const_cast<wchar_t *>(UNISP_NAME_W),
                                                      SECPKG_CRED_INBOUND,
                                                      nullptr,
                                                      &creds,
                                                      nullptr,
                                                      nullptr,
                                                      &_cred,
                                                      &expiry);
        if (status != SEC_E_OK)
            qWarning("Acquire server credentials failed: %lx", static_cast<unsigned long>(status));
        _haveCred = status == SEC_E_OK;
    }
    ~NativeServer()
    {
        if (_haveCtx)
            DeleteSecurityContext(&_ctx);
        if (_haveCred)
            FreeCredentialsHandle(&_cred);
        if (_cert)
            CertFreeCertificateContext(_cert);
        if (_key)
            CryptDestroyKey(_key);
        if (_provider) {
            CryptReleaseContext(_provider, 0);
            HCRYPTPROV unused = 0;
            if (!CryptAcquireContextW(&unused,
                                      reinterpret_cast<const wchar_t *>(_keyName.utf16()),
                                      MS_ENH_RSA_AES_PROV_W,
                                      PROV_RSA_AES,
                                      CRYPT_DELETEKEYSET | CRYPT_SILENT))
                qFatal("Could not remove temporary QCA test key (error %lx)", GetLastError());
        }
    }
    bool valid() const
    {
        return _haveCred;
    }
    QByteArray der() const
    {
        return _cert ? QByteArray(reinterpret_cast<const char *>(_cert->pbCertEncoded), int(_cert->cbCertEncoded))
                     : QByteArray();
    }
    QByteArray peerCertificateDer() const
    {
        if (!_haveCtx || !complete)
            return {};
        PCCERT_CONTEXT cert = nullptr;
        if (QueryContextAttributesW(const_cast<CtxtHandle *>(&_ctx), SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                    &cert) != SEC_E_OK || !cert)
            return {};
        const QByteArray der(reinterpret_cast<const char *>(cert->pbCertEncoded), int(cert->cbCertEncoded));
        CertFreeCertificateContext(cert);
        return der;
    }
    QByteArray step(const QByteArray &input)
    {
        _input += input;
        SecBuffer     in[2] {{ULONG(_input.size()), SECBUFFER_TOKEN, _input.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc {SECBUFFER_VERSION, 2, in};
        SecBuffer     out {0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc output {SECBUFFER_VERSION, 1, &out};
        ULONG         attributes = 0;
        TimeStamp     expiry;
        const auto status = AcceptSecurityContext(&_cred,
                                                  _haveCtx ? &_ctx : nullptr,
                                                  _input.isEmpty() ? nullptr : &desc,
                                                  ASC_REQ_CONFIDENTIALITY | ASC_REQ_STREAM | ASC_REQ_ALLOCATE_MEMORY |
                                                      ASC_REQ_EXTENDED_ERROR | (_requestClientCertificate ? ASC_REQ_MUTUAL_AUTH : 0),
                                                  SECURITY_NATIVE_DREP,
                                                  &_ctx,
                                                  &output,
                                                  &attributes,
                                                  &expiry);
        QByteArray result;
        if (out.pvBuffer) {
            result = QByteArray(static_cast<char *>(out.pvBuffer), int(out.cbBuffer));
            FreeContextBuffer(out.pvBuffer);
        }
        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED)
            _haveCtx = true;
        if (status == SEC_E_INCOMPLETE_MESSAGE)
            return result;
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
            failed = true;
            return result;
        }
        _input = in[1].BufferType == SECBUFFER_EXTRA ? _input.right(int(in[1].cbBuffer)) : QByteArray();
        if (status == SEC_E_OK) {
            complete = true;
            QueryContextAttributesW(&_ctx, SECPKG_ATTR_STREAM_SIZES, &_sizes);
        }
        return result;
    }
    QByteArray encrypt(const QByteArray &plain)
    {
        QByteArray result;
        for (int offset = 0; offset < plain.size();) {
            const ULONG size = qMin(ULONG(plain.size() - offset), _sizes.cbMaximumMessage);
            if (!size)
                return {};
            QByteArray record(int(_sizes.cbHeader + size + _sizes.cbTrailer), 0);
            std::memcpy(record.data() + _sizes.cbHeader, plain.constData() + offset, size);
            SecBuffer buffers[4] {{_sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                                  {size, SECBUFFER_DATA, record.data() + _sizes.cbHeader},
                                  {_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + _sizes.cbHeader + size},
                                  {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc {SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&_ctx, 0, &desc, 0) != SEC_E_OK)
                return {};
            for (const auto &b : buffers)
                if (b.cbBuffer)
                    result.append(static_cast<char *>(b.pvBuffer), int(b.cbBuffer));
            offset += int(size);
        }
        return result;
    }
    QByteArray decrypt(QByteArray input)
    {
        QByteArray result;
        while (!input.isEmpty()) {
            SecBuffer     buffers[4] {{ULONG(input.size()), SECBUFFER_DATA, input.data()},
                                      {0, SECBUFFER_EMPTY, nullptr},
                                      {0, SECBUFFER_EMPTY, nullptr},
                                      {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc {SECBUFFER_VERSION, 4, buffers};
            if (DecryptMessage(&_ctx, &desc, 0, nullptr) != SEC_E_OK)
                return {};
            int extra = 0;
            for (const auto &b : buffers) {
                if (b.BufferType == SECBUFFER_DATA)
                    result.append(static_cast<char *>(b.pvBuffer), int(b.cbBuffer));
                if (b.BufferType == SECBUFFER_EXTRA)
                    extra = int(b.cbBuffer);
            }
            input = extra ? input.right(extra) : QByteArray();
        }
        return result;
    }
    QByteArray exporter()
    {
        char                             label[] = "EXPORTER-Channel-Binding";
        SecPkgContext_KeyingMaterialInfo info {sizeof(label), label, 0, nullptr, 32};
        if (SetContextAttributesW(&_ctx, SECPKG_ATTR_KEYING_MATERIAL_INFO, &info, sizeof(info)) != SEC_E_OK)
            return {};
        SecPkgContext_KeyingMaterial material {};
        if (QueryContextAttributesW(&_ctx, SECPKG_ATTR_KEYING_MATERIAL, &material) != SEC_E_OK)
            return {};
        QByteArray result(reinterpret_cast<const char *>(material.pbKeyingMaterial), int(material.cbKeyingMaterial));
        if (material.pbKeyingMaterial) {
            SecureZeroMemory(material.pbKeyingMaterial, material.cbKeyingMaterial);
            FreeContextBuffer(material.pbKeyingMaterial);
        }
        return result;
    }
    bool complete = false, failed = false;

private:
    bool                      _requestClientCertificate = false;
    HCRYPTPROV                _provider = 0;
    HCRYPTKEY                 _key      = 0;
    QString                   _keyName;
    PCCERT_CONTEXT            _cert = nullptr;
    CredHandle                _cred {};
    CtxtHandle                _ctx {};
    SecPkgContext_StreamSizes _sizes {};
    bool                      _haveCred = false, _haveCtx = false;
    QByteArray                _input;
};

std::unique_ptr<QCA::TLSContext> context()
{
    auto *provider = QCA::findProvider(ProviderName);
    return std::unique_ptr<QCA::TLSContext>(
        provider ? static_cast<QCA::TLSContext *>(provider->createContext(QStringLiteral("tls"))) : nullptr);
}
}

class WinCryptoTLSTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void systemTrustWithoutCRLs();
    void configuredTrustWithoutCRLs();
    void unsupportedCRLImports();
    void certificates();
    void nativeIdentities();
    void clientTLS_data();
    void clientTLS();
    void optionalClientCertificateRequest();
    void explicitClientCertificate();
    void publicTLS();
    void publicTLS_data();
    void unsupportedConfiguration();

private:
    QCA::Initializer _qca;
};

void WinCryptoTLSTest::systemTrustWithoutCRLs()
{
    QVERIFY(QCA::isSupported("cert", ProviderName));
    if (QCA::isSupported("crl"))
        QSKIP("Requires a configuration without a CRL provider");

    const auto store = CertOpenSystemStoreW(0, L"ROOT");
    QVERIFY(store);
    const auto first = CertEnumCertificatesInStore(store, nullptr);
    QByteArray der;
    if (first) {
        der = QByteArray(reinterpret_cast<const char *>(first->pbCertEncoded), int(first->cbCertEncoded));
        CertFreeCertificateContext(first);
    }
    CertCloseStore(store, 0);
    if (der.isEmpty())
        QSKIP("The Windows root certificate store is empty");
    const auto expected = QCA::Certificate::fromDER(der, nullptr, ProviderName);
    QVERIFY(!expected.isNull());
    QVERIFY(QCA::haveSystemStore());
    const auto roots = QCA::systemStore();
    QVERIFY(roots.certificates().contains(expected));
}

void WinCryptoTLSTest::configuredTrustWithoutCRLs()
{
    if (QCA::isSupported("crl"))
        QSKIP("Requires a configuration without a CRL provider");
    NativeServer server;
    QVERIFY(server.valid());
    const auto certificate = QCA::Certificate::fromDER(server.der(), nullptr, ProviderName);
    QVERIFY(!certificate.isNull());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto fileName = directory.filePath(QStringLiteral("roots.pem"));
    QFile      file(fileName);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray pem =
        certificate.toPEM().toUtf8() + QByteArray("\n-----BEGIN X509 CRL-----\nAA==\n-----END X509 CRL-----\n");
    QCOMPARE(file.write(pem), qint64(pem.size()));
    file.close();

    struct RestoreConfig
    {
        QVariantMap config;
        ~RestoreConfig()
        {
            QCA::setProviderConfig(QStringLiteral("default"), config);
        }
    } restore {QCA::getProviderConfig(QStringLiteral("default"))};
    auto config                          = restore.config;
    config[QStringLiteral("use_system")] = false;
    config[QStringLiteral("roots_file")] = fileName;
    QCA::setProviderConfig(QStringLiteral("default"), config);
    auto stores = std::unique_ptr<QCA::KeyStoreListContext>(
        static_cast<QCA::KeyStoreListContext *>(QCA::defaultProvider()->createContext(QStringLiteral("keystorelist"))));
    QVERIFY(stores);
    stores->start();
    QVERIFY(stores->keyStores().contains(0));
    QVERIFY(stores->entryTypes(0).contains(QCA::KeyStoreEntry::TypeCertificate));
    QVERIFY(!stores->entryTypes(0).contains(QCA::KeyStoreEntry::TypeCRL));
    const auto entries = stores->entryList(0);
    const bool matches = entries.size() == 1 && entries.first()->certificate() == certificate;
    qDeleteAll(entries);
    QVERIFY(matches);
}

void WinCryptoTLSTest::unsupportedCRLImports()
{
    if (QCA::isSupported("crl"))
        QSKIP("Requires a configuration without a CRL provider");
    QCA::ConvertResult result = QCA::ConvertGood;
    QVERIFY(QCA::CRL::fromDER(QByteArray(), &result, ProviderName).isNull());
    QCOMPARE(result, QCA::ErrorDecode);
    result = QCA::ConvertGood;
    QVERIFY(QCA::CRL::fromPEM(QString(), &result, ProviderName).isNull());
    QCOMPARE(result, QCA::ErrorDecode);
}

void WinCryptoTLSTest::certificates()
{
    QVERIFY(QCA::isSupported("cert", ProviderName));
    auto probe = context();
    QVERIFY(probe);
    probe->setup(false, QStringLiteral("qca.test"), false);
    probe->start();
    probe->waitForResultsReady(1000);
    QCOMPARE(probe->result(), QCA::TLSContext::Success);
    NativeServer server;
    QVERIFY(!server.der().isEmpty());
    const auto cert = QCA::Certificate::fromDER(server.der(), nullptr, ProviderName);
    QVERIFY(!cert.isNull());
    QCOMPARE(cert.provider()->name(), ProviderName);
    QCOMPARE(cert.commonName(), QStringLiteral("qca.test"));
    QVERIFY(cert.isSelfSigned());
    QVERIFY(cert.matchesHostName(QStringLiteral("qca.test")));
    QVERIFY(!cert.matchesHostName(QStringLiteral("other.test")));
    QCOMPARE(cert.toDER(), server.der());
    QCOMPARE(QCA::Certificate::fromPEM(cert.toPEM(), nullptr, ProviderName), cert);
    QCA::CertificateCollection trust;
    QVERIFY(cert.validate(trust, {}) != QCA::ValidityGood);
    trust.addCertificate(cert);
    QCOMPARE(cert.validate(trust, {}), QCA::ValidityGood);
    auto malformed = server.der();
    malformed[0]   = 0;
    QVERIFY(QCA::Certificate::fromDER(malformed, nullptr, ProviderName).isNull());
    NativeServer expired(true);
    QVERIFY(!expired.der().isEmpty());
    const auto old = QCA::Certificate::fromDER(expired.der(), nullptr, ProviderName);
    QVERIFY(!old.isNull());
    QCA::CertificateCollection oldTrust;
    oldTrust.addCertificate(old);
    QCOMPARE(old.validate(oldTrust, {}), QCA::ErrorExpired);
}

void WinCryptoTLSTest::nativeIdentities()
{
    QCA::KeyGenerator generator;
    const auto        key = generator.createRSA(2048, 65537, ProviderName);
    QVERIFY(!key.isNull());
    QVERIFY(key.isPrivate());
    QVERIFY(key.canExport());
    QCOMPARE(key.bitSize(), 2048);
    QCOMPARE(key.toRSA().e(), QCA::BigInteger(65537));
    QVERIFY(key.toRSA().n() > QCA::BigInteger(0));
    const auto              copy = key;
    QCA::CertificateOptions options;
    QCA::CertificateInfo    info;
    info.insert(QCA::CommonName, QStringLiteral("qca.test"));
    info.insert(QCA::DNS, QStringLiteral("qca.test"));
    info.insert(QCA::XMPP, QStringLiteral("alice@qca.test"));
    options.setInfo(info);
    options.setSerialNumber(QCA::BigInteger(123456));
    const auto now = QDateTime::currentDateTimeUtc();
    options.setValidityPeriod(now.addSecs(-60), now.addDays(1));
    options.setConstraints({QCA::DigitalSignature, QCA::KeyEncipherment, QCA::ClientAuth, QCA::ServerAuth});
    const QCA::Certificate cert(options, copy, ProviderName);
    QVERIFY(!cert.isNull());
    QVERIFY(cert.isSelfSigned());
    QVERIFY(!cert.isCA());
    QCOMPARE(cert.serialNumber(), QCA::BigInteger(123456));
    QCOMPARE(cert.commonName(), QStringLiteral("qca.test"));
    QVERIFY(cert.matchesHostName(QStringLiteral("qca.test")));
    QCOMPARE(cert.subjectInfo().value(QCA::XMPP), QStringLiteral("alice@qca.test"));
    QCA::CertificateCollection trust;
    trust.addCertificate(cert);
    QCOMPARE(cert.validate(trust, {}, QCA::UsageTLSServer), QCA::ValidityGood);
    const auto publicView = key.toPublicKey();
    QVERIFY(!publicView.isPrivate());
    QCOMPARE(publicView.bitSize(), 2048);
    QCOMPARE(publicView.toRSA().n(), key.toRSA().n());
    QVERIFY(publicView.canExport());
    QCOMPARE(cert.subjectPublicKey().toDER(), publicView.toDER());
    QVERIFY(!key.isNull());
    info.remove(QCA::CommonName);
    info.insert(QCA::CommonName, QStringLiteral("Unicode key \u03a9\u96ea"));
    options.setInfo(info);
    const QCA::Certificate unicodeCert(options, key, ProviderName);
    QVERIFY(!unicodeCert.isNull());
    QCOMPARE(unicodeCert.commonName(), QStringLiteral("Unicode key \u03a9\u96ea"));
    options.setAsCA();
    QVERIFY(QCA::Certificate(options, key, ProviderName).isCA());
    QVERIFY(generator.createRSA(1024, 65537, ProviderName).isNull());
    QCA::KeyGenerator asynchronous;
    asynchronous.setBlockingEnabled(false);
    QSignalSpy ready(&asynchronous, &QCA::KeyGenerator::finished);
    QVERIFY(asynchronous.createRSA(2048, 65537, ProviderName).isNull());
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    QVERIFY(!asynchronous.key().isNull());
}

void WinCryptoTLSTest::clientTLS_data()
{
    QTest::addColumn<bool>("trusted");
    QTest::addColumn<bool>("expired");
    QTest::addColumn<bool>("tls13");
    QTest::newRow("trusted") << true << false << false;
    QTest::newRow("untrusted") << false << false << false;
    QTest::newRow("expired") << true << true << false;
    auto client = context();
    if (client && !client->supportedCipherSuites(QCA::TLS::TLS_v1_3).isEmpty())
        QTest::newRow("TLS1.3") << true << false << true;
}

void WinCryptoTLSTest::clientTLS()
{
    QFETCH(bool, trusted);
    QFETCH(bool, expired);
    QFETCH(bool, tls13);
    QVERIFY(QCA::isSupported("tls", ProviderName));
    NativeServer server(expired, tls13);
    QVERIFY(server.valid());
    auto client = context();
    QVERIFY(client);
    client->setup(false, QStringLiteral("qca.test"), false);
    QCA::CertificateCollection trust;
    if (trusted)
        trust.addCertificate(QCA::Certificate::fromDER(server.der(), nullptr, ProviderName));
    client->setTrustedCertificates(trust);
    client->start();
    QVERIFY(client->waitForResultsReady(1000));
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    client->update({}, {});
    client->waitForResultsReady(1000);
    for (int round = 0; round < 20 && (!server.complete || client->result() != QCA::TLSContext::Success); ++round) {
        const auto outgoing = client->to_net();
        const auto reply    = server.step(outgoing);
        QVERIFY(!server.failed);
        // Fragment even TLS headers to exercise SEC_E_INCOMPLETE_MESSAGE.
        for (int offset = 0; offset < reply.size(); offset += 7) {
            client->update(reply.mid(offset, 7), {});
            client->waitForResultsReady(1000);
            QVERIFY(client->result() != QCA::TLSContext::Error);
        }
    }
    QVERIFY(server.complete);
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QVERIFY(!client->peerCertificateChain().isEmpty());
    QCOMPARE(client->peerCertificateValidity(),
             expired       ? QCA::ErrorExpired
                 : trusted ? QCA::ValidityGood
                           : QCA::ErrorUntrusted);
    if (!trusted || expired)
        return;
    QVERIFY(client->sessionInfo().version == QCA::TLS::TLS_v1_2 || client->sessionInfo().version == QCA::TLS::TLS_v1_3);
    QVERIFY(client->sessionInfo().cipherBits >= 128);
    QVERIFY(client->supportedCipherSuites(client->sessionInfo().version).contains(client->sessionInfo().cipherSuite));
    QVERIFY(client->channelBindingTypes().contains(QStringLiteral("tls-server-end-point")));
    const auto binding = client->channelBinding(QStringLiteral("tls-server-end-point"));
    QCOMPARE(binding, QCA::Hash(QStringLiteral("sha256"), ProviderName).hash(server.der()).toByteArray());
    if (tls13) {
        const auto exporter = client->channelBinding(QStringLiteral("tls-exporter"));
        QCOMPARE(exporter.size(), 32);
        QCOMPARE(exporter, server.exporter());
        QVERIFY(!client->channelBindingTypes().contains(QStringLiteral("tls-unique")));
    } else {
        QVERIFY(!client->channelBinding(QStringLiteral("tls-unique")).isEmpty());
    }
    const QByteArray message(100000, 's');
    client->update({}, message);
    client->waitForResultsReady(1000);
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QCOMPARE(client->encoded(), message.size());
    QCOMPARE(server.decrypt(client->to_net()), message);
    const auto response = server.encrypt(message);
    QVERIFY(!response.isEmpty());
    QByteArray plain;
    for (int offset = 0; offset < response.size(); offset += 11) {
        client->update(response.mid(offset, 11), {});
        client->waitForResultsReady(1000);
        QCOMPARE(client->result(), QCA::TLSContext::Success);
        plain += client->to_app();
    }
    QCOMPARE(plain, message);
    // Multiple complete records in one update must not repeatedly copy the
    // remaining ciphertext. This also exercises bulk plaintext collection.
    const QByteArray bulk(8 * 1024 * 1024 + 29, 'b');
    const auto       bulkRecords = server.encrypt(bulk);
    QVERIFY(!bulkRecords.isEmpty());
    client->update(bulkRecords, {});
    client->waitForResultsReady(1000);
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QCOMPARE(client->to_app(), bulk);
    auto tampered                 = server.encrypt(QByteArray("secret"));
    tampered[tampered.size() - 1] = char(tampered.at(tampered.size() - 1) ^ 1);
    client->update(tampered, {});
    client->waitForResultsReady(1000);
    QCOMPARE(client->result(), QCA::TLSContext::Error);
    QVERIFY(client->to_app().isEmpty());
    client->update({}, message);
    client->waitForResultsReady(1000);
    QCOMPARE(client->result(), QCA::TLSContext::Error);
}

void WinCryptoTLSTest::optionalClientCertificateRequest()
{
    // Exercise an SSPI server CertificateRequest with a client that has no
    // configured certificate. NativeServer and its trust anchor are ephemeral.
    NativeServer server(false, false, false, true);
    QVERIFY(server.valid());
    auto client = context();
    QVERIFY(client);
    client->setup(false, QStringLiteral("qca.test"), false);
    QCA::CertificateCollection trust;
    trust.addCertificate(QCA::Certificate::fromDER(server.der(), nullptr, ProviderName));
    client->setTrustedCertificates(trust);
    client->start();
    QVERIFY(client->waitForResultsReady(1000));
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    client->update({}, {});
    client->waitForResultsReady(1000);
    for (int round = 0; round < 20 && (!server.complete || client->result() != QCA::TLSContext::Success); ++round) {
        const auto outgoing = client->to_net();
        const auto reply = server.step(outgoing);
        QVERIFY(!server.failed);
        for (int offset = 0; offset < reply.size(); offset += 7) {
            client->update(reply.mid(offset, 7), {});
            client->waitForResultsReady(1000);
            QVERIFY(client->result() != QCA::TLSContext::Error);
        }
    }
    QVERIFY(server.complete);
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QCOMPARE(client->peerCertificateValidity(), QCA::ValidityGood);
    const QByteArray message("secure data after optional client certificate request");
    client->update({}, message);
    client->waitForResultsReady(1000);
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QCOMPARE(server.decrypt(client->to_net()), message);
}

void WinCryptoTLSTest::explicitClientCertificate()
{
    QCA::KeyGenerator generator;
    const auto key = generator.createRSA(2048, 65537, ProviderName);
    QVERIFY(!key.isNull());
    QCA::CertificateOptions options;
    QCA::CertificateInfo subject;
    subject.insert(QCA::CommonName, QStringLiteral("client.qca.test"));
    options.setInfo(subject);
    options.setSerialNumber(QCA::BigInteger(123458));
    const auto now = QDateTime::currentDateTimeUtc();
    options.setValidityPeriod(now.addSecs(-60), now.addDays(1));
    options.setConstraints({QCA::DigitalSignature, QCA::ClientAuth});
    const QCA::Certificate cert(options, key, ProviderName);
    QVERIFY(!cert.isNull());
    QCA::CertificateChain chain;
    chain += cert;

    // Never accept malformed explicit credentials by falling back to no
    // certificate, even when the server would otherwise allow anonymous TLS.
    auto invalid = context();
    QVERIFY(invalid);
    invalid->setup(false, QStringLiteral("qca.test"), false);
    invalid->setCertificate(chain, generator.createRSA(2048, 65537, ProviderName));
    invalid->start();
    QVERIFY(invalid->waitForResultsReady(1000));
    QCOMPARE(invalid->result(), QCA::TLSContext::Error);
    auto incomplete = context();
    QVERIFY(incomplete);
    incomplete->setup(false, QStringLiteral("qca.test"), false);
    incomplete->setCertificate(chain, {});
    incomplete->start();
    QVERIFY(incomplete->waitForResultsReady(1000));
    QCOMPARE(incomplete->result(), QCA::TLSContext::Error);

    NativeServer server(false, false, false, true);
    QVERIFY(server.valid());
    auto client = context();
    QVERIFY(client);
    client->setup(false, QStringLiteral("qca.test"), false);
    QCA::CertificateCollection trust;
    trust.addCertificate(QCA::Certificate::fromDER(server.der(), nullptr, ProviderName));
    client->setTrustedCertificates(trust);
    client->setCertificate(chain, key);
    client->start();
    QVERIFY(client->waitForResultsReady(1000));
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    client->update({}, {});
    client->waitForResultsReady(1000);
    for (int round = 0; round < 20 && (!server.complete || client->result() != QCA::TLSContext::Success); ++round) {
        const auto outgoing = client->to_net();
        const auto reply = server.step(outgoing);
        QVERIFY(!server.failed);
        for (int offset = 0; offset < reply.size(); offset += 7) {
            client->update(reply.mid(offset, 7), {});
            client->waitForResultsReady(1000);
            QVERIFY(client->result() != QCA::TLSContext::Error);
        }
    }
    QVERIFY(server.complete);
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QCOMPARE(client->peerCertificateValidity(), QCA::ValidityGood);
    QCOMPARE(server.peerCertificateDer(), cert.toDER());
    const QByteArray message("mutually authenticated stream TLS data");
    client->update({}, message);
    QVERIFY(client->waitForResultsReady(1000));
    QCOMPARE(client->result(), QCA::TLSContext::Success);
    QCOMPARE(server.decrypt(client->to_net()), message);
}

void WinCryptoTLSTest::publicTLS_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<bool>("conflictingSAN");
    QTest::addColumn<bool>("trusted");
    QTest::addColumn<bool>("expired");
    QTest::newRow("matching") << QStringLiteral("qca.test") << true << false << true << false;
    QTest::newRow("wrong-host") << QStringLiteral("other.test") << false << false << true << false;
    QTest::newRow("SAN-overrides-CN") << QStringLiteral("qca.test") << false << true << true << false;
    QTest::newRow("caller-host-check") << QString() << true << false << true << false;
    QTest::newRow("caller-host-check-untrusted") << QString() << false << false << false << false;
    QTest::newRow("caller-host-check-expired") << QString() << false << false << true << true;
}

void WinCryptoTLSTest::publicTLS()
{
    QFETCH(QString, host);
    QFETCH(bool, valid);
    QFETCH(bool, conflictingSAN);
    QFETCH(bool, trusted);
    QFETCH(bool, expired);
    NativeServer server(expired, false, conflictingSAN);
    QVERIFY(server.valid());
    QCA::TLS                   client(QCA::TLS::Stream, nullptr, ProviderName);
    QCA::CertificateCollection trust;
    if (trusted)
        trust.addCertificate(QCA::Certificate::fromDER(server.der(), nullptr, ProviderName));
    client.setTrustedCertificates(trust);
    bool complete = false, failed = false;
    connect(&client, &QCA::TLS::handshaken, &client, [&] {
        complete = true;
        client.continueAfterStep();
    });
    connect(&client, &QCA::TLS::peerCertificateAvailable, &client, [&] { client.continueAfterStep(); });
    connect(&client, &QCA::TLS::error, &client, [&] { failed = true; });
    client.startClient(host);
    for (int round = 0; round < 100 && (!complete || !server.complete) && !failed; ++round) {
        QCoreApplication::processEvents();
        const auto outgoing = client.readOutgoing();
        if (!outgoing.isEmpty()) {
            const auto reply = server.step(outgoing);
            if (!reply.isEmpty())
                client.writeIncoming(reply);
        }
    }
    QVERIFY(!failed);
    QVERIFY(!server.failed);
    QVERIFY(complete);
    QVERIFY(server.complete);
    if (!valid) {
        QVERIFY(client.peerIdentityResult() != QCA::TLS::Valid);
        if (expired)
            QCOMPARE(client.peerCertificateValidity(), QCA::ErrorExpired);
        else if (!trusted)
            QCOMPARE(client.peerCertificateValidity(), QCA::ErrorUntrusted);
        return;
    }
    QCOMPARE(client.peerIdentityResult(), QCA::TLS::Valid);
    if (host.isEmpty()) {
        // The caller accepts the chain and checks the intended
        // peer separately before it sends any application data.
        const auto cert = client.peerCertificateChain().primary();
        QVERIFY(cert.matchesHostName(QStringLiteral("qca.test")));
        QVERIFY(!cert.matchesHostName(QStringLiteral("other.test")));
    }
    client.write(QByteArray("application data"));
    for (int i = 0; i < 10; ++i)
        QCoreApplication::processEvents();
    QCOMPARE(server.decrypt(client.readOutgoing()), QByteArray("application data"));
    QByteArray closeRecord;
    connect(&client, &QCA::TLS::readyReadOutgoing, &client, [&] { closeRecord += client.readOutgoing(); });
    client.close();
    for (int i = 0; i < 10; ++i)
        QCoreApplication::processEvents();
    QVERIFY(!closeRecord.isEmpty());
}

void WinCryptoTLSTest::unsupportedConfiguration()
{
    for (int config = 0; config < 3; ++config) {
        auto client = context();
        QVERIFY(client);
        client->setup(
            config == 0, config == 1 ? QStringLiteral("qca\0.test") : QStringLiteral("qca.test"), config == 2);
        client->start();
        client->waitForResultsReady(1000);
        QCOMPARE(client->result(), QCA::TLSContext::Error);
    }
}

QTEST_GUILESS_MAIN(WinCryptoTLSTest)
#include "wincryptotlsunittest.moc"
