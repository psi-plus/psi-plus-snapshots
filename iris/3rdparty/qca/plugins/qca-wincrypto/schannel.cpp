// SPDX-License-Identifier: LGPL-2.1-or-later
// Native X.509 certificates, TLS and DTLS through Windows security APIs.
#include "aesmodes.h"
#include "native_keys.h"
#include "native_tls.h"
#include <QElapsedTimer>
#include <QQueue>
#include <QTimer>
#include <QUrl>
#include <windows.h>
// Keep windows.h before the Windows security headers.
#define SECURITY_WIN32
#include <security.h>
#include <winternl.h>
#define SCHANNEL_USE_BLACKLISTS
#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <schannel.h>
#include <wincrypt.h>

using namespace QCA;

namespace wincryptoQCAPlugin {
namespace {
constexpr DWORD Encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
const BYTE     *bytes(const QByteArray &a)
{
    return reinterpret_cast<const BYTE *>(a.constData());
}
SecureArray copySecure(const char *data, int length)
{
    SecureArray result(length);
    if (length)
        std::memcpy(result.data(), data, std::size_t(length));
    return result;
}

struct Store
{
    HCERTSTORE value = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    ~Store()
    {
        if (value)
            CertCloseStore(value, 0);
    }
    bool add(const QByteArray &der) const
    {
        return value &&
            CertAddEncodedCertificateToStore(
                   value, Encoding, bytes(der), DWORD(der.size()), CERT_STORE_ADD_ALWAYS, nullptr);
    }
};

template<class T> struct Decoded
{
    T *value = nullptr;
    Decoded(LPCSTR type, const CRYPT_DATA_BLOB &blob, DWORD flags = 0)
    {
        DWORD size = 0;
        CryptDecodeObjectEx(
            Encoding, type, blob.pbData, blob.cbData, CRYPT_DECODE_ALLOC_FLAG | flags, nullptr, &value, &size);
    }
    ~Decoded()
    {
        if (value)
            LocalFree(value);
    }
};

QDateTime dateTime(const FILETIME &time)
{
    ULARGE_INTEGER ticks;
    ticks.LowPart  = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    return QDateTime::fromMSecsSinceEpoch(qint64(ticks.QuadPart / 10000) - 11644473600000LL, Qt::UTC);
}

CertificateInfoOrdered distinguishedName(const CERT_NAME_BLOB &blob)
{
    CertificateInfoOrdered  out;
    Decoded<CERT_NAME_INFO> name(X509_UNICODE_NAME, blob);
    if (!name.value)
        return out;
    for (DWORD r = 0; r < name.value->cRDN; ++r) {
        const auto &rdn = name.value->rgRDN[r];
        for (DWORD a = 0; a < rdn.cRDNAttr; ++a) {
            auto &attr = rdn.rgRDNAttr[a];
            auto  text = QString::fromWCharArray(reinterpret_cast<const wchar_t *>(attr.Value.pbData),
                                                 int(attr.Value.cbData / sizeof(wchar_t)));
            if (text.endsWith(QChar(0)))
                text.chop(1);
            out += CertificateInfoPair(CertificateInfoType(QString::fromLatin1(attr.pszObjId), CertificateInfoType::DN),
                                       text);
        }
    }
    return out;
}

Validity trustError(DWORD error)
{
    if (!error)
        return ValidityGood;
    if (error & CERT_TRUST_IS_REVOKED)
        return ErrorRevoked;
    if (error & CERT_TRUST_IS_NOT_SIGNATURE_VALID)
        return ErrorSignatureFailed;
    if (error & CERT_TRUST_IS_NOT_TIME_VALID)
        return ErrorExpired;
    if (error & CERT_TRUST_IS_NOT_VALID_FOR_USAGE)
        return ErrorInvalidPurpose;
    if (error & CERT_TRUST_INVALID_BASIC_CONSTRAINTS)
        return ErrorInvalidCA;
    if (error & CERT_TRUST_IS_EXPLICIT_DISTRUST)
        return ErrorRejected;
    if (error & (CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN))
        return ErrorUntrusted;
    return ErrorValidityUnknown;
}

SignatureDigest digestOID(const char *oid)
{
    const QByteArray id(oid);
    if (id == "1.3.14.3.2.26")
        return SignatureDigest::SHA1;
    if (id == "2.16.840.1.101.3.4.2.1")
        return SignatureDigest::SHA256;
    if (id == "2.16.840.1.101.3.4.2.2")
        return SignatureDigest::SHA384;
    if (id == "2.16.840.1.101.3.4.2.3")
        return SignatureDigest::SHA512;
    if (id == "2.16.840.1.101.3.4.2.4")
        return SignatureDigest::SHA224;
    return SignatureDigest::Unknown;
}

SignatureAlgorithm signatureAlgorithm(const CRYPT_ALGORITHM_IDENTIFIER &algorithm)
{
    const QByteArray oid(algorithm.pszObjId);
    if (oid == "1.2.840.113549.1.1.10") {
        Decoded<CRYPT_RSA_SSA_PSS_PARAMETERS> pss(PKCS_RSA_SSA_PSS_PARAMETERS, algorithm.Parameters);
        if (!pss.value)
            return {};
        return {SignatureScheme::RSA_PSS,
                digestOID(pss.value->HashAlgorithm.pszObjId),
                digestOID(pss.value->MaskGenAlgorithm.HashAlgorithm.pszObjId),
                int(pss.value->dwSaltLength),
                int(pss.value->dwTrailerField)};
    }
    if (oid == "1.3.101.112")
        return {SignatureScheme::Ed25519, SignatureDigest::None};
    if (oid == "1.3.101.113")
        return {SignatureScheme::Ed448, SignatureDigest::None};
    if (oid == "1.2.840.10040.4.3")
        return {SignatureScheme::DSA, SignatureDigest::SHA1};
    const bool rsa = oid.startsWith("1.2.840.113549.1.1.");
    const bool ec  = oid.startsWith("1.2.840.10045.4.");
    if (!rsa && !ec)
        return {};
    SignatureDigest digest = SignatureDigest::Unknown;
    if (oid == "1.2.840.113549.1.1.4")
        digest = SignatureDigest::MD5;
    if (oid == "1.2.840.113549.1.1.5" || oid == "1.2.840.10045.4.1")
        digest = SignatureDigest::SHA1;
    if (oid == "1.2.840.113549.1.1.14" || oid == "1.2.840.10045.4.3.1")
        digest = SignatureDigest::SHA224;
    if (oid == "1.2.840.113549.1.1.11" || oid == "1.2.840.10045.4.3.2")
        digest = SignatureDigest::SHA256;
    if (oid == "1.2.840.113549.1.1.12" || oid == "1.2.840.10045.4.3.3")
        digest = SignatureDigest::SHA384;
    if (oid == "1.2.840.113549.1.1.13" || oid == "1.2.840.10045.4.3.4")
        digest = SignatureDigest::SHA512;
    if (digest == SignatureDigest::Unknown)
        return {};
    return {rsa ? SignatureScheme::RSA_PKCS1v15 : SignatureScheme::ECDSA, digest};
}

// The exclusive root store is essential: an empty QCA trust list must not
// silently inherit machine trust. No AIA/root downloads occur on the event loop.
Validity validateCertificate(PCCERT_CONTEXT           cert,
                             HCERTSTORE               intermediates,
                             const QList<QByteArray> &trusted,
                             const QList<QByteArray> &crls,
                             UsageMode                usage,
                             ValidateFlags            flags,
                             const QString           &host = QString())
{
    if (!cert)
        return ErrorValidityUnknown;
    Store roots;
    if (!roots.value)
        return ErrorValidityUnknown;
    for (const auto &der : trusted)
        if (!roots.add(der))
            return ErrorValidityUnknown;
    for (const auto &der : crls)
        if (!CertAddEncodedCRLToStore(
                intermediates, Encoding, bytes(der), DWORD(der.size()), CERT_STORE_ADD_ALWAYS, nullptr))
            return ErrorValidityUnknown;
    CERT_CHAIN_ENGINE_CONFIG config {};
    config.cbSize           = sizeof(config);
    config.hExclusiveRoot   = roots.value;
    config.dwFlags          = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE;
    HCERTCHAINENGINE engine = nullptr;
    if (!CertCreateCertificateChainEngine(&config, &engine))
        return ErrorValidityUnknown;
    CERT_CHAIN_PARA params {};
    params.cbSize = sizeof(params);
    LPSTR oid     = nullptr;
    switch (usage) {
    case UsageTLSServer:
        oid = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        break;
    case UsageTLSClient:
        oid = const_cast<LPSTR>(szOID_PKIX_KP_CLIENT_AUTH);
        break;
    case UsageCodeSigning:
        oid = const_cast<LPSTR>(szOID_PKIX_KP_CODE_SIGNING);
        break;
    case UsageEmailProtection:
        oid = const_cast<LPSTR>(szOID_PKIX_KP_EMAIL_PROTECTION);
        break;
    case UsageTimeStamping:
        oid = const_cast<LPSTR>(szOID_PKIX_KP_TIMESTAMP_SIGNING);
        break;
    case UsageAny:
        break;
    default:
        CertFreeCertificateChainEngine(engine);
        return ErrorInvalidPurpose;
    }
    params.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    if (oid) {
        params.RequestedUsage.Usage.cUsageIdentifier     = 1;
        params.RequestedUsage.Usage.rgpszUsageIdentifier = &oid;
    }
    DWORD chainFlags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE;
    // Match QCA's CRL-based validation. An explicit revocation check must fail
    // when revocation information is unavailable, rather than assuming good.
    if (!crls.isEmpty() || (flags & ValidateRevoked))
        chainFlags |= CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY;
    PCCERT_CHAIN_CONTEXT chain  = nullptr;
    Validity             result = ErrorValidityUnknown;
    if (CertGetCertificateChain(engine, cert, nullptr, intermediates, &params, chainFlags, nullptr, &chain)) {
        result = trustError(chain->TrustStatus.dwErrorStatus);
        if (result == ValidityGood && !host.isEmpty()) {
            const auto                       aceHost = QString::fromLatin1(QUrl::toAce(host));
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl {};
            ssl.cbSize         = sizeof(ssl);
            ssl.dwAuthType     = AUTHTYPE_SERVER;
            ssl.pwszServerName = const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(aceHost.utf16()));
            CERT_CHAIN_POLICY_PARA policy {};
            policy.cbSize            = sizeof(policy);
            policy.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS status {};
            status.cbSize = sizeof(status);
            if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status))
                result = ErrorValidityUnknown;
            else if (status.dwError)
                result = status.dwError == CERT_E_CN_NO_MATCH ? ErrorInvalidPurpose : ErrorRejected;
        }
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateChainEngine(engine);
    return result;
}

class WinCertificate : public CertContext
{
public:
    explicit WinCertificate(Provider *p)
        : CertContext(p)
    {
    }
    WinCertificate(const WinCertificate &other)
        : CertContext(other)
        , _der(other._der)
        , _props(other._props)
    {
        if (other._cert)
            _cert = CertDuplicateCertificateContext(other._cert);
    }
    ~WinCertificate() override
    {
        if (_cert)
            CertFreeCertificateContext(_cert);
    }
    Context *clone() const override
    {
        return new WinCertificate(*this);
    }
    bool createSelfSigned(const CertificateOptions &opts, const PKeyContext &key) override
    {
        return fromDER(createSelfSignedCertificate(opts, key)) == ConvertGood;
    }
    const CertContextProps *props() const override
    {
        return &_props;
    }
    QByteArray toDER() const override
    {
        return _der;
    }
    QString toPEM() const override
    {
        if (_der.isEmpty())
            return {};
        const auto b64 = _der.toBase64();
        QByteArray out("-----BEGIN CERTIFICATE-----\n");
        for (int i = 0; i < b64.size(); i += 64)
            out += b64.mid(i, 64) + '\n';
        out += "-----END CERTIFICATE-----\n";
        return QString::fromLatin1(out);
    }
    ConvertResult fromPEM(const QString &pem) override
    {
        const auto input = pem.toLatin1();
        DWORD      size  = 0;
        if (!CryptStringToBinaryA(
                input.constData(), DWORD(input.size()), CRYPT_STRING_BASE64HEADER, nullptr, &size, nullptr, nullptr))
            return ErrorDecode;
        QByteArray der(int(size), 0);
        if (!CryptStringToBinaryA(input.constData(),
                                  DWORD(input.size()),
                                  CRYPT_STRING_BASE64HEADER,
                                  reinterpret_cast<BYTE *>(der.data()),
                                  &size,
                                  nullptr,
                                  nullptr))
            return ErrorDecode;
        der.resize(int(size));
        return fromDER(der);
    }
    ConvertResult fromDER(const QByteArray &der) override
    {
        if (_cert)
            CertFreeCertificateContext(_cert);
        _cert = nullptr;
        _der.clear();
        _props = CertContextProps {};
        if (der.isEmpty())
            return ErrorDecode;
        _cert = CertCreateCertificateContext(Encoding, bytes(der), DWORD(der.size()));
        if (!_cert || _cert->cbCertEncoded != DWORD(der.size()))
            return ErrorDecode;
        _der = der;
        readProperties();
        return ConvertGood;
    }
    bool compare(const CertContext *other) const override
    {
        return other && _der == other->toDER();
    }
    PKeyContext *subjectPublicKey() const override
    {
        if (!_cert)
            return nullptr;
        DWORD       size = 0;
        const auto *info = &_cert->pCertInfo->SubjectPublicKeyInfo;
        if (!CryptEncodeObjectEx(Encoding, X509_PUBLIC_KEY_INFO, info, 0, nullptr, nullptr, &size) || !size ||
            size > 65536)
            return nullptr;
        QByteArray der(int(size), 0);
        if (!CryptEncodeObjectEx(Encoding, X509_PUBLIC_KEY_INFO, info, 0, nullptr, der.data(), &size))
            return nullptr;
        der.resize(int(size));
        auto key = std::unique_ptr<PKeyContext>(static_cast<PKeyContext *>(createPKeyContext(provider())));
        return key->publicFromDER(der) == ConvertGood ? key.release() : nullptr;
    }
    bool isIssuerOf(const CertContext *other) const override
    {
        const auto der  = other ? other->toDER() : QByteArray();
        auto       cert = CertCreateCertificateContext(Encoding, bytes(der), DWORD(der.size()));
        if (!cert || !_cert) {
            if (cert)
                CertFreeCertificateContext(cert);
            return false;
        }
        const bool result =
            CertCompareCertificateName(Encoding, &_cert->pCertInfo->Subject, &cert->pCertInfo->Issuer) &&
            CryptVerifyCertificateSignatureEx(0,
                                              Encoding,
                                              CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
                                              const_cast<PCERT_CONTEXT>(cert),
                                              CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT,
                                              const_cast<PCERT_CONTEXT>(_cert),
                                              0,
                                              nullptr);
        CertFreeCertificateContext(cert);
        return result;
    }
    Validity validate(const QList<CertContext *> &trusted,
                      const QList<CertContext *> &untrusted,
                      const QList<CRLContext *>  &crls,
                      UsageMode                   usage,
                      ValidateFlags               flags) const override
    {
        Store store;
        if (!store.value)
            return ErrorValidityUnknown;
        for (auto *cert : untrusted)
            if (!store.add(cert->toDER()))
                return ErrorValidityUnknown;
        QList<QByteArray> roots, revocations;
        for (auto *cert : trusted)
            roots += cert->toDER();
        for (auto *crl : crls)
            revocations += crl->toDER();
        return validateCertificate(_cert, store.value, roots, revocations, usage, flags);
    }
    Validity validate_chain(const QList<CertContext *> &chain,
                            const QList<CertContext *> &trusted,
                            const QList<CRLContext *>  &crls,
                            UsageMode                   usage,
                            ValidateFlags               flags) const override
    {
        if (chain.isEmpty())
            return ErrorValidityUnknown;
        return chain.first()->validate(trusted, chain.mid(1), crls, usage, flags);
    }

private:
    void readProperties()
    {
        const auto &info    = *_cert->pCertInfo;
        _props.version      = int(info.dwVersion) + 1;
        _props.start        = dateTime(info.NotBefore);
        _props.end          = dateTime(info.NotAfter);
        _props.subject      = distinguishedName(info.Subject);
        _props.issuer       = distinguishedName(info.Issuer);
        _props.pathLimit    = -1;
        _props.isSelfSigned = isIssuerOf(this);
        SecureArray serial(int(info.SerialNumber.cbData) + 1, 0);
        for (DWORD i = 0; i < info.SerialNumber.cbData; ++i)
            serial[int(i) + 1] = char(info.SerialNumber.pbData[info.SerialNumber.cbData - 1 - i]);
        _props.serial = BigInteger(serial);
        if (auto *ext = CertFindExtension(szOID_BASIC_CONSTRAINTS2, info.cExtension, info.rgExtension)) {
            Decoded<CERT_BASIC_CONSTRAINTS2_INFO> basic(X509_BASIC_CONSTRAINTS2, ext->Value);
            if (basic.value) {
                _props.isCA = basic.value->fCA;
                if (basic.value->fPathLenConstraint)
                    _props.pathLimit = int(basic.value->dwPathLenConstraint);
            }
        }
        BYTE usage[2] {};
        if (CertGetIntendedKeyUsage(Encoding, _cert->pCertInfo, usage, sizeof(usage))) {
            const ConstraintTypeKnown types[] = {DigitalSignature,
                                                 NonRepudiation,
                                                 KeyEncipherment,
                                                 DataEncipherment,
                                                 KeyAgreement,
                                                 KeyCertificateSign,
                                                 CRLSign,
                                                 EncipherOnly,
                                                 DecipherOnly};
            for (unsigned i = 0; i < 9; ++i)
                if (usage[i / 8] & (0x80 >> (i % 8)))
                    _props.constraints += types[i];
        }
        if (auto *ext = CertFindExtension(szOID_ENHANCED_KEY_USAGE, info.cExtension, info.rgExtension)) {
            Decoded<CERT_ENHKEY_USAGE> eku(X509_ENHANCED_KEY_USAGE, ext->Value);
            if (eku.value)
                for (DWORD i = 0; i < eku.value->cUsageIdentifier; ++i)
                    _props.constraints += ConstraintType(QString::fromLatin1(eku.value->rgpszUsageIdentifier[i]),
                                                         ConstraintType::ExtendedKeyUsage);
        }
        if (auto *ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2, info.cExtension, info.rgExtension)) {
            Decoded<CERT_ALT_NAME_INFO> names(X509_ALTERNATE_NAME, ext->Value);
            if (names.value)
                for (DWORD i = 0; i < names.value->cAltEntry; ++i) {
                    const auto &name = names.value->rgAltEntry[i];
                    switch (name.dwAltNameChoice) {
                    case CERT_ALT_NAME_DNS_NAME:
                        _props.subject += CertificateInfoPair(DNS, QString::fromWCharArray(name.pwszDNSName));
                        break;
                    case CERT_ALT_NAME_RFC822_NAME:
                        _props.subject += CertificateInfoPair(Email, QString::fromWCharArray(name.pwszRfc822Name));
                        break;
                    case CERT_ALT_NAME_URL:
                        _props.subject += CertificateInfoPair(URI, QString::fromWCharArray(name.pwszURL));
                        break;
                    case CERT_ALT_NAME_IP_ADDRESS:
                    {
                        const auto &ip = name.IPAddress;
                        QStringList parts;
                        if (ip.cbData == 4) {
                            for (DWORD b = 0; b < 4; ++b)
                                parts += QString::number(ip.pbData[b]);
                            _props.subject += CertificateInfoPair(IPAddress, parts.join(QLatin1Char('.')));
                        } else if (ip.cbData == 16) {
                            for (DWORD b = 0; b < 16; b += 2)
                                parts += QString::number((unsigned(ip.pbData[b]) << 8) | ip.pbData[b + 1], 16);
                            _props.subject += CertificateInfoPair(IPAddress, parts.join(QLatin1Char(':')));
                        }
                        break;
                    }
                    case CERT_ALT_NAME_OTHER_NAME:
                        if (std::strcmp(name.pOtherName->pszObjId, "1.3.6.1.5.5.7.8.5") == 0) {
                            Decoded<CERT_NAME_VALUE> xmpp(X509_UNICODE_ANY_STRING, name.pOtherName->Value);
                            if (xmpp.value)
                                _props.subject += CertificateInfoPair(
                                    XMPP,
                                    QString::fromWCharArray(reinterpret_cast<const wchar_t *>(xmpp.value->Value.pbData),
                                                            int(xmpp.value->Value.cbData / sizeof(wchar_t))));
                        }
                        break;
                    default:
                        break;
                    }
                }
        }
        _props.sigalgo = signatureAlgorithm(info.SignatureAlgorithm);
        CRYPT_DATA_BLOB                   encoded {_cert->cbCertEncoded, _cert->pbCertEncoded};
        Decoded<CERT_SIGNED_CONTENT_INFO> content(X509_CERT, encoded, CRYPT_DECODE_NO_SIGNATURE_BYTE_REVERSAL_FLAG);
        if (content.value)
            _props.sig = QByteArray(reinterpret_cast<const char *>(content.value->Signature.pbData),
                                    int(content.value->Signature.cbData));
        if (auto *ext = CertFindExtension(szOID_SUBJECT_KEY_IDENTIFIER, info.cExtension, info.rgExtension)) {
            Decoded<CRYPT_DATA_BLOB> id(X509_OCTET_STRING, ext->Value);
            if (id.value)
                _props.subjectId = QByteArray(reinterpret_cast<const char *>(id.value->pbData), int(id.value->cbData));
        }
        if (auto *ext = CertFindExtension(szOID_AUTHORITY_KEY_IDENTIFIER2, info.cExtension, info.rgExtension)) {
            Decoded<CERT_AUTHORITY_KEY_ID2_INFO> id(X509_AUTHORITY_KEY_ID2, ext->Value);
            if (id.value)
                _props.issuerId =
                    QByteArray(reinterpret_cast<const char *>(id.value->KeyId.pbData), int(id.value->KeyId.cbData));
        }
    }
    PCCERT_CONTEXT   _cert = nullptr;
    QByteArray       _der;
    CertContextProps _props {};
};

} // namespace

Provider::Context *createCertificateContext(Provider *provider)
{
    return new WinCertificate(provider);
}

namespace {
class WinTLS : public TLSContext
{
public:
    explicit WinTLS(Provider *p, bool datagram = false)
        : TLSContext(p, datagram ? QStringLiteral("dtls") : QStringLiteral("tls"))
        , _datagram(datagram)
    {
        _ready.setSingleShot(true);
        connect(&_ready, &QTimer::timeout, this, [this] { emit resultsReady(); });
        _retransmit.setSingleShot(true);
        connect(&_retransmit, &QTimer::timeout, this, [this] {
            _resend = true;
            emit dtlsTimeout();
        });
    }
    ~WinTLS() override
    {
        release();
    }
    // A live Schannel context cannot be duplicated. QCA clones TLS only to
    // construct a fresh session; never share credentials or record counters.
    Context *clone() const override
    {
        return new WinTLS(provider(), _datagram);
    }
    void reset() override
    {
        _ready.stop();
        release();
        _host.clear();
        _input.clear();
        _network.clear();
        wipe(_application);
        _peer.clear();
        _trusted    = CertificateCollection();
        _server     = false;
        _configured = true;
        _handshaken = false;
        _eof        = false;
        _closing    = false;
        _minSSF     = 0;
        _maxSSF     = 256;
        _suites.clear();
        _encoded  = 0;
        _validity = ErrorValidityUnknown;
        _result   = Error;
        _info     = SessionInfo {};
        _profiles.clear();
        _localCertificate.clear();
        _localKey = PrivateKey();
    }
    QStringList supportedCipherSuites(const TLS::Version &version) const override
    {
        if ((_datagram && version != TLS::DTLS_v1_2) ||
            (!_datagram && version != TLS::TLS_v1_2 && version != TLS::TLS_v1_3))
            return {};
        PCRYPT_CONTEXT_FUNCTIONS functions = nullptr;
        ULONG                    size      = 0;
        QStringList              list;
        if (BCRYPT_SUCCESS(
                BCryptEnumContextFunctions(CRYPT_LOCAL, L"SSL", NCRYPT_SCHANNEL_INTERFACE, &size, &functions))) {
            for (ULONG i = 0; i < functions->cFunctions; ++i) {
                const auto name = QString::fromWCharArray(functions->rgpszFunctions[i]);
                const bool tls13 =
                    name.startsWith(QLatin1String("TLS_AES_")) || name.startsWith(QLatin1String("TLS_CHACHA20_"));
                if (tls13 == (version == TLS::TLS_v1_3))
                    list += name;
            }
            BCryptFreeBuffer(functions);
        }
        return list;
    }
    bool canCompress() const override
    {
        return false;
    }
    bool canSetHostName() const override
    {
        return true;
    }
    int maxSSF() const override
    {
        return 256;
    }
    void setup(bool server, const QString &host, bool compress) override
    {
        _server = server;
        _host   = host;
        // An empty host is QCA's documented mode for caller-side identity
        // checks, including protocol-specific certificate matching rules.
        _configured = (!server || _datagram) && !compress && !host.contains(QChar(0));
    }
    void setConstraints(int min, int max) override
    {
        _minSSF = min;
        _maxSSF = max < 0 ? 256 : max;
    }
    void setConstraints(const QStringList &list) override
    {
        _suites = list;
    }
    void setTrustedCertificates(const CertificateCollection &trusted) override
    {
        _trusted = trusted;
    }
    void setIssuerList(const QList<CertificateInfoOrdered> &) override
    {
    }
    void setCertificate(const CertificateChain &cert, const PrivateKey &key) override
    {
        // Only explicitly supplied identities may be offered to Schannel.
        // Native key conversion validates that the leaf certificate matches.
        _localCertificate = cert;
        _localKey         = key;
    }
    void setSessionId(const TLSSessionContext &) override
    {
    }
    void shutdown() override
    {
        _closing = true;
        // QCA suppresses empty updates after an idle connection. Request the
        // update that actually generates close_notify.
        QTimer::singleShot(0, this, [this] { emit dtlsTimeout(); });
    }
    void start() override
    {
        release();
        _result = Error;
        if (_datagram) {
            startDatagram();
            _ready.start(0);
            return;
        }
        if (_configured && !_server && _minSSF <= _maxSSF && _maxSSF >= 128) {
            // Reject incomplete or mismatched identities. An invalid explicit
            // certificate must never silently degrade to anonymous TLS.
            if (_localCertificate.isEmpty() != _localKey.isNull())
                return;
            auto identity = _localCertificate.isEmpty()
                ? nullptr : createCredentialCertificate(_localCertificate, _localKey);
            if (!_localCertificate.isEmpty() && !identity)
                return;
            PCCERT_CONTEXT certificate = identity ? identity->certificate : nullptr;
            TLS_PARAMETERS params {};
            params.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1 | SP_PROT_TLS1_1;
            SCH_CREDENTIALS creds {};
            creds.dwVersion      = SCH_CREDENTIALS_VERSION;
            creds.dwFlags        = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;
            creds.cCreds         = certificate ? 1 : 0;
            creds.paCred         = certificate ? &certificate : nullptr;
            creds.cTlsParameters = 1;
            creds.pTlsParameters = &params;
            TimeStamp  expiry;
            const auto status = AcquireCredentialsHandleW(nullptr,
                                                          const_cast<wchar_t *>(UNISP_NAME_W),
                                                          SECPKG_CRED_OUTBOUND,
                                                          nullptr,
                                                          &creds,
                                                          nullptr,
                                                          nullptr,
                                                          &_cred,
                                                          &expiry);
            _haveCred = status == SEC_E_OK;
            if (status == SEC_E_UNKNOWN_CREDENTIALS || status == SEC_E_UNSUPPORTED_FUNCTION) {
                // Early Windows 10 lacks SCH_CREDENTIALS. Its legacy API can
                // still provide TLS 1.2 without enabling older protocols.
                SCHANNEL_CRED legacy {};
                legacy.dwVersion               = SCHANNEL_CRED_VERSION;
                legacy.grbitEnabledProtocols   = SP_PROT_TLS1_2_CLIENT;
                legacy.dwFlags                 = creds.dwFlags;
                legacy.cCreds                  = creds.cCreds;
                legacy.paCred                  = creds.paCred;
                legacy.dwMinimumCipherStrength = ULONG(std::max(128, _minSSF));
                legacy.dwMaximumCipherStrength = ULONG(_maxSSF);
                _haveCred = AcquireCredentialsHandleW(nullptr,
                                                      const_cast<wchar_t *>(UNISP_NAME_W),
                                                      SECPKG_CRED_OUTBOUND,
                                                      nullptr,
                                                      &legacy,
                                                      nullptr,
                                                      nullptr,
                                                      &_cred,
                                                      &expiry) == SEC_E_OK;
            }
            // Schannel has opened its own key reference. Remove only the
            // temporary persisted bridge key, never the original QCA key.
            if (identity && !identity->removeTemporaryKey()) {
                release();
                return;
            }
            if (_haveCred)
                _result = Success;
        }
        _ready.start(0);
    }
    void update(const QByteArray &network, const QByteArray &application) override
    {
        if (_datagram) {
            updateDatagram(network, application);
            _ready.start(0);
            return;
        }
        _encoded = 0;
        _result  = Error;
        if (!_haveCred || (_eof && !_closing) || network.size() > std::numeric_limits<int>::max() - _input.size()) {
            _ready.start(0);
            return;
        }
        _input += network;
        if ((!_handshaken || _postHandshake) && _input.size() > 1024 * 1024) {
            release();
            _ready.start(0);
            return;
        }
        if (_closing && _handshaken) {
            DWORD         shutdown = SCHANNEL_SHUTDOWN;
            SecBuffer     token {sizeof(shutdown), SECBUFFER_TOKEN, &shutdown};
            SecBufferDesc desc {SECBUFFER_VERSION, 1, &token};
            if (ApplyControlToken(&_ctx, &desc) == SEC_E_OK) {
                handshake(true);
                if (_result != Error) {
                    _eof    = true;
                    _result = Success;
                }
            }
        } else if (!_handshaken) {
            handshake(false);
        } else {
            _result = Success;
            if (_postHandshake)
                handshake(false);
            if (_result != Error && !_postHandshake && !decode())
                _result = Error;
            if (_result != Error) {
                if (_postHandshake) {
                    if (std::size_t(application.size()) >
                        std::size_t(std::numeric_limits<int>::max()) - _deferred.size())
                        _result = Error;
                    else if (!application.isEmpty()) {
                        const auto *data = reinterpret_cast<const std::uint8_t *>(application.constData());
                        _deferred.insert(_deferred.end(), data, data + application.size());
                    }
                } else {
                    if (!_deferred.empty() &&
                        !encode(reinterpret_cast<const char *>(_deferred.data()), int(_deferred.size())))
                        _result = Error;
                    secureWipe(_deferred);
                    if (_result != Error && !application.isEmpty() &&
                        !encode(application.constData(), int(application.size())))
                        _result = Error;
                }
                if (_result != Error)
                    _result = Success;
            }
        }
        if (_result == Error) {
            wipe(_application);
            wipe(_input);
            release();
        }
        _ready.start(0);
    }
    bool waitForResultsReady(int) override
    {
        _ready.stop();
        return true;
    }
    Result result() const override
    {
        return _result;
    }
    QByteArray to_net() override
    {
        if (_datagram)
            return _packets.isEmpty() ? QByteArray() : _packets.dequeue();
        QByteArray result;
        result.swap(_network);
        return result;
    }
    int encoded() const override
    {
        return _encoded;
    }
    QByteArray to_app() override
    {
        if (_datagram) {
            if (_plaintext.empty())
                return {};
            const auto &packet = _plaintext.front();
            QByteArray  result(reinterpret_cast<const char *>(packet.data()), int(packet.size()));
            _plaintext.pop_front();
            return result;
        }
        const QByteArray result(_application.empty() ? "" : reinterpret_cast<const char *>(_application.data()),
                                int(_application.size()));
        secureWipe(_application);
        return result;
    }
    bool eof() const override
    {
        return _eof;
    }
    bool clientHelloReceived() const override
    {
        return false;
    }
    bool serverHelloReceived() const override
    {
        return _handshaken;
    }
    QString hostName() const override
    {
        return _host;
    }
    bool certificateRequested() const override
    {
        return false;
    }
    QList<CertificateInfoOrdered> issuerList() const override
    {
        return {};
    }
    Validity peerCertificateValidity() const override
    {
        return _validity;
    }
    CertificateChain peerCertificateChain() const override
    {
        return _peer;
    }
    SessionInfo sessionInfo() const override
    {
        return _info;
    }
    QByteArray unprocessed() override
    {
        QByteArray result;
        result.swap(_input);
        return result;
    }
    QStringList channelBindingTypes() const override
    {
        QStringList types;
        for (const auto &type :
             {QStringLiteral("tls-exporter"), QStringLiteral("tls-server-end-point"), QStringLiteral("tls-unique")})
            if (!channelBinding(type).isEmpty())
                types += type;
        return types;
    }
    QByteArray channelBinding(const QString &type) const override
    {
        if (!_handshaken)
            return {};
        if (type == QLatin1String("tls-exporter"))
            return _exporter.toByteArray();
        ULONG attribute;
        if (type == QLatin1String("tls-server-end-point"))
            attribute = SECPKG_ATTR_ENDPOINT_BINDINGS;
        else if (type == QLatin1String("tls-unique") && _info.version == TLS::TLS_v1_2)
            attribute = SECPKG_ATTR_UNIQUE_BINDINGS;
        else
            return {};
        SecPkgContext_Bindings bindings {};
        if (QueryContextAttributesW(const_cast<CtxtHandle *>(&_ctx), attribute, &bindings) != SEC_E_OK)
            return {};
        QByteArray  result;
        const auto *b = bindings.Bindings;
        if (b && b->dwApplicationDataOffset <= bindings.BindingsLength &&
            b->cbApplicationDataLength <= bindings.BindingsLength - b->dwApplicationDataOffset) {
            result                  = QByteArray(reinterpret_cast<const char *>(b) + b->dwApplicationDataOffset,
                                                 int(b->cbApplicationDataLength));
            const QByteArray prefix = type.toLatin1() + ':';
            if (result.startsWith(prefix))
                result.remove(0, prefix.size());
            else
                result.clear();
        }
        if (b)
            FreeContextBuffer(bindings.Bindings);
        return result;
    }
    void setMTU(int size) override
    {
        if (!_datagram || size == -1)
            return;
        if (size < 200 || size > 65535) {
            _configured = false;
            return;
        }
        _mtu = size;
        if (_haveCtx) {
            ULONG mtu = ULONG(size);
            if (SetContextAttributesW(&_ctx, SECPKG_ATTR_DTLS_MTU, &mtu, sizeof(mtu)) != SEC_E_OK)
                _configured = false;
        }
    }
    QStringList supportedSRTPProfiles() const override
    {
        if (!_datagram)
            return {};
        return {QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80"),
                QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_32"),
                QStringLiteral("SRTP_AEAD_AES_128_GCM"),
                QStringLiteral("SRTP_AEAD_AES_256_GCM")};
    }
    void setSRTPProfiles(const QStringList &profiles) override
    {
        _profiles = profiles;
        for (const auto &profile : profiles)
            if (!supportedSRTPProfiles().contains(profile))
                _configured = false;
    }
    QString selectedSRTPProfile() const override
    {
        return _handshaken ? _material.profile() : QString();
    }
    TLS::SRTPKeyingMaterial srtpKeyingMaterial() const override
    {
        return _handshaken ? _material : TLS::SRTPKeyingMaterial();
    }

private:
    static void wipe(QByteArray &data)
    {
        if (!data.isEmpty())
            secureWipe(data.data(), std::size_t(data.size()));
        data.clear();
    }
    static void wipe(Bytes &data)
    {
        secureWipe(data);
    }
    void release()
    {
        _retransmit.stop();
        _flight.clear();
        _packets.clear();
        _plaintext.clear();
        _material = TLS::SRTPKeyingMaterial();
        _resend   = false;
        _lastPeerPacket.clear();
        _peerAddress.clear();
        if (_haveCtx)
            DeleteSecurityContext(&_ctx);
        if (_haveCred)
            FreeCredentialsHandle(&_cred);
        _haveCtx = _haveCred = false;
        wipe(_application);
        wipe(_input);
        _exporter.clear();
        secureWipe(_deferred);
        _postHandshake = false;
        _noClientCertificateRetry = false;
    }
    void startDatagram()
    {
        _handshaken = _eof = _closing = false;
        _retries                      = 0;
        _retryDelay                   = 1000;
        if (!_configured || _minSSF > _maxSSF || _maxSSF < 128 || (_server && _localCertificate.isEmpty()) ||
            (!_profiles.isEmpty() && _localCertificate.isEmpty()))
            return;
        auto identity =
            _localCertificate.isEmpty() ? nullptr : createCredentialCertificate(_localCertificate, _localKey);
        if (!_localCertificate.isEmpty() && !identity)
            return;
        SCHANNEL_CRED creds {};
        creds.dwVersion               = SCHANNEL_CRED_VERSION;
        creds.grbitEnabledProtocols   = _server ? SP_PROT_DTLS1_2_SERVER : SP_PROT_DTLS1_2_CLIENT;
        creds.dwMinimumCipherStrength = ULONG(std::max(128, _minSSF));
        creds.dwMaximumCipherStrength = ULONG(_maxSSF);
        creds.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
            SCH_CRED_NO_SYSTEM_MAPPER;
        if (identity) {
            creds.cCreds = 1;
            creds.paCred = &identity->certificate;
        }
        TimeStamp expiry;
        _haveCred = AcquireCredentialsHandleW(nullptr,
                                              const_cast<wchar_t *>(UNISP_NAME_W),
                                              _server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND,
                                              nullptr,
                                              &creds,
                                              nullptr,
                                              nullptr,
                                              &_cred,
                                              &expiry) == SEC_E_OK;
        // Schannel has opened its own key reference. Do not keep a persisted
        // identity for the lifetime of the connection or wait for teardown.
        if (identity && !identity->removeTemporaryKey()) {
            release();
            return;
        }
        if (!_haveCred)
            return;
        // QCA routes each association to one peer, but exposes no sockaddr.
        // Give Schannel a stable, random per-association cookie namespace.
        _peerAddress    = QByteArray(28, 0);
        _peerAddress[0] = 23; // AF_INET6
        if (BCryptGenRandom(
                nullptr, reinterpret_cast<PUCHAR>(_peerAddress.data() + 8), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            release();
            return;
        }
        _handshakeTime.start();
        _result = Success;
    }
    void updateDatagram(const QByteArray &network, const QByteArray &application)
    {
        _encoded = 0;
        _result  = Error;
        if (!_haveCred || !_configured || (_eof && !_closing)) {
            release();
            return;
        }
        if (_closing && _handshaken) {
            if (_eof) {
                _result = Success;
                return;
            }
            const char alert[2] {1, 0};
            if (encodeDatagram(alert, 2, SECQOP_WRAP_OOB_DATA)) {
                _eof    = true;
                _result = Success;
                _retransmit.stop();
            }
            return;
        }
        if (_resend) {
            _resend = false;
            if (++_retries > (_handshaken ? 3 : 6)) {
                if (!_handshaken) {
                    release();
                    return;
                }
                _flight.clear();
            } else {
                for (const auto &packet : _flight)
                    _packets.enqueue(packet);
                _retryDelay = std::min(_retryDelay * 2, 60000);
                _retransmit.start(_retryDelay);
            }
        }
        if (!_handshaken) {
            if (_handshakeTime.elapsed() > 120000 || !application.isEmpty()) {
                release();
                return;
            }
            if (_server && !_haveCtx && network.isEmpty()) {
                _result = Continue;
                return;
            }
            handshakeDatagram(network);
        } else {
            _result = Success;
            if (!network.isEmpty() && !decodeDatagram(network))
                _result = Error;
            if (_result != Error && !application.isEmpty()) {
                if (!encodeDatagram(application.constData(), int(application.size()), 0))
                    _result = Error;
                else {
                    _encoded = int(application.size());
                    _retransmit.stop();
                    _flight.clear();
                }
            }
        }
        if (_result == Error)
            release();
    }
    void handshakeDatagram(QByteArray network)
    {
        _result = Continue;
        if (network.size() > 65535)
            return;
        QList<QByteArray> flight;
        const auto        peerPacket = network;
        const auto        supported  = supportedSRTPProfiles();
        const WORD        ids[] {1, 2, 7, 8};
        std::vector<WORD> profiles(1, WORD(_profiles.size() * sizeof(WORD)));
        for (const auto &profile : _profiles) {
            // SSPI's WORD profile IDs are already in network byte order.
            // ProfilesSize, unlike each list entry, is a host-order length.
            const WORD id = ids[supported.indexOf(profile)];
            profiles.push_back(WORD((id << 8) | (id >> 8)));
        }
        for (int fragment = 0; fragment < 128; ++fragment) {
            SEC_DTLS_MTU mtu {USHORT(_mtu)};
            SecBuffer    buffers[5] {};
            ULONG        count = 0;
            if (!network.isEmpty())
                buffers[count++] = {ULONG(network.size()), SECBUFFER_TOKEN, network.data()};
            if (!_profiles.isEmpty())
                buffers[count++] = {
                    ULONG(profiles.size() * sizeof(WORD)), SECBUFFER_SRTP_PROTECTION_PROFILES, profiles.data()};
            buffers[count++] = {sizeof(mtu), SECBUFFER_DTLS_MTU, &mtu};
            if (_server)
                buffers[count++] = {ULONG(_peerAddress.size()), SECBUFFER_EXTRA, _peerAddress.data()};
            buffers[count++] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc input {SECBUFFER_VERSION, count, buffers};
            SecBuffer     outputBuffer {0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc output {SECBUFFER_VERSION, 1, &outputBuffer};
            ULONG         attributes = 0;
            TimeStamp     expiry;
            const auto    host   = QString::fromLatin1(QUrl::toAce(_host));
            const auto    status = _server
                ? AcceptSecurityContext(&_cred,
                                        _haveCtx ? &_ctx : nullptr,
                                        &input,
                                        ASC_REQ_DATAGRAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_REPLAY_DETECT |
                                            ASC_REQ_SEQUENCE_DETECT | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_MUTUAL_AUTH,
                                        SECURITY_NATIVE_DREP,
                                        &_ctx,
                                        &output,
                                        &attributes,
                                        &expiry)
                : InitializeSecurityContextW(
                      &_cred,
                      _haveCtx ? &_ctx : nullptr,
                      host.isEmpty() ? nullptr : const_cast<LPWSTR>(reinterpret_cast<const wchar_t *>(host.utf16())),
                      ISC_REQ_DATAGRAM | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                          ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS,
                      0,
                      SECURITY_NATIVE_DREP,
                      &input,
                      0,
                      &_ctx,
                      &output,
                      &attributes,
                      &expiry);
            if (status >= 0)
                _haveCtx = true;
            if (outputBuffer.pvBuffer) {
                if (outputBuffer.cbBuffer && outputBuffer.cbBuffer <= ULONG(_mtu))
                    flight.append(
                        QByteArray(static_cast<const char *>(outputBuffer.pvBuffer), int(outputBuffer.cbBuffer)));
                else if (outputBuffer.cbBuffer)
                    _result = Error;
                FreeContextBuffer(outputBuffer.pvBuffer);
            }
            if (_result == Error)
                return;
            network.clear();
            if (status == SEC_I_MESSAGE_FRAGMENT)
                continue;
            if (status == SEC_E_INCOMPLETE_MESSAGE || status == SEC_E_OUT_OF_SEQUENCE)
                return;
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
                _result = Error;
                return;
            }
            if (!flight.isEmpty()) {
                _flight = flight;
                for (const auto &packet : flight)
                    _packets.enqueue(packet);
                _retries    = 0;
                _retryDelay = 1000;
                _retransmit.start(_retryDelay);
            }
            if (status == SEC_E_OK) {
                if (!finishHandshake() || !exportSRTP()) {
                    _result = Error;
                    return;
                }
                _handshaken     = true;
                _lastPeerPacket = peerPacket;
                _duplicateReply.invalidate();
                _result = Success;
            }
            return;
        }
        _result = Error;
    }
    bool exportSRTP()
    {
        SecPkgContext_SrtpParameters params {};
        if (QueryContextAttributesW(&_ctx, SECPKG_ATTR_SRTP_PARAMETERS, &params) != SEC_E_OK)
            return _profiles.isEmpty();
        const WORD selected = WORD((params.ProtectionProfile << 8) | (params.ProtectionProfile >> 8));
        const bool hasMKI   = params.MasterKeyIdentifierSize != 0;
        if (params.MasterKeyIdentifier)
            FreeContextBuffer(params.MasterKeyIdentifier);
        if (hasMKI)
            return false;
        if (!selected)
            return _profiles.isEmpty();
        int index = selected == 1 ? 0 : selected == 2 ? 1 : selected == 7 ? 2 : selected == 8 ? 3 : -1;
        if (index < 0)
            return false;
        const auto profile = supportedSRTPProfiles()[index];
        if (!_profiles.contains(profile))
            return false;
        const int                        keyLength  = selected == 8 ? 32 : 16;
        const int                        saltLength = selected >= 7 ? 12 : 14;
        const int                        length     = 2 * (keyLength + saltLength);
        char                             label[]    = "EXTRACTOR-dtls_srtp";
        SecPkgContext_KeyingMaterialInfo request {sizeof(label), label, 0, nullptr, DWORD(length)};
        if (SetContextAttributesW(&_ctx, SECPKG_ATTR_KEYING_MATERIAL_INFO, &request, sizeof(request)) != SEC_E_OK)
            return false;
        SecPkgContext_KeyingMaterial exported {};
        if (QueryContextAttributesW(&_ctx, SECPKG_ATTR_KEYING_MATERIAL, &exported) != SEC_E_OK)
            return false;
        SecureArray secret;
        if (exported.pbKeyingMaterial && exported.cbKeyingMaterial == DWORD(length))
            secret = copySecure(reinterpret_cast<const char *>(exported.pbKeyingMaterial), length);
        if (exported.pbKeyingMaterial) {
            secureWipe(exported.pbKeyingMaterial, exported.cbKeyingMaterial);
            FreeContextBuffer(exported.pbKeyingMaterial);
        }
        if (secret.isEmpty())
            return false;
        const int local = _server ? 1 : 0, remote = 1 - local;
        _material =
            TLS::SRTPKeyingMaterial(profile,
                                    copySecure(secret.constData() + local * keyLength, keyLength),
                                    copySecure(secret.constData() + 2 * keyLength + local * saltLength, saltLength),
                                    copySecure(secret.constData() + remote * keyLength, keyLength),
                                    copySecure(secret.constData() + 2 * keyLength + remote * saltLength, saltLength));
        return true;
    }
    bool encodeDatagram(const char *data, int size, ULONG qop)
    {
        if (!_sizes.cbMaximumMessage || size < 0 || ULONG(size) > _sizes.cbMaximumMessage ||
            _sizes.cbHeader + ULONG(size) + _sizes.cbTrailer > ULONG(_mtu))
            return false;
        SecureArray record {int(_sizes.cbHeader + ULONG(size) + _sizes.cbTrailer)};
        std::memcpy(record.data() + _sizes.cbHeader, data, std::size_t(size));
        SecBuffer     buffers[4] {{_sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                                  {ULONG(size), SECBUFFER_DATA, record.data() + _sizes.cbHeader},
                                  {_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + _sizes.cbHeader + size},
                                  {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc {SECBUFFER_VERSION, 4, buffers};
        if (EncryptMessage(&_ctx, qop, &desc, 0) != SEC_E_OK)
            return false;
        QByteArray packet;
        packet.reserve(int(record.size()));
        for (const auto &buffer : buffers)
            if (buffer.cbBuffer)
                packet.append(static_cast<const char *>(buffer.pvBuffer), int(buffer.cbBuffer));
        if (packet.size() > _mtu)
            return false;
        _packets.enqueue(packet);
        return true;
    }
    bool decodeDatagram(const QByteArray &packet)
    {
        if (packet.size() > 65535)
            return true;
        if (packet == _lastPeerPacket && !_flight.isEmpty()) {
            if (!_duplicateReply.isValid() || _duplicateReply.elapsed() >= 1000) {
                for (const auto &part : _flight)
                    _packets.enqueue(part);
                _duplicateReply.start();
            }
            return true;
        }
        // A peer awaiting our final flight can retransmit its old handshake.
        // Those records are not a new handshake on the established session.
        if (!packet.isEmpty() && (uchar(packet[0]) == 20 || uchar(packet[0]) == 22))
            return true;
        SecureArray input  = copySecure(packet.constData(), int(packet.size()));
        int         offset = 0;
        while (offset < input.size()) {
            SecBuffer     buffers[4] {{ULONG(input.size() - offset), SECBUFFER_DATA, input.data() + offset},
                                      {0, SECBUFFER_EMPTY, nullptr},
                                      {0, SECBUFFER_EMPTY, nullptr},
                                      {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc {SECBUFFER_VERSION, 4, buffers};
            const auto    status = DecryptMessage(&_ctx, &desc, 0, nullptr);
            if (status == SEC_E_OUT_OF_SEQUENCE || status == SEC_E_MESSAGE_ALTERED || status == SEC_E_DECRYPT_FAILURE ||
                status == SEC_E_INVALID_TOKEN || status == SEC_E_INCOMPLETE_MESSAGE)
                return true;
            if (status == SEC_I_CONTEXT_EXPIRED) {
                _eof = true;
                return true;
            }
            if (status != SEC_E_OK)
                return false;
            int extra = 0;
            for (const auto &buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer) {
                    const auto *data = static_cast<const std::uint8_t *>(buffer.pvBuffer);
                    _plaintext.emplace_back(data, data + buffer.cbBuffer);
                    _retransmit.stop();
                    _flight.clear();
                } else if (buffer.BufferType == SECBUFFER_EXTRA)
                    extra = int(buffer.cbBuffer);
            }
            if (!extra)
                break;
            if (extra >= input.size() - offset)
                return false;
            offset = int(input.size()) - extra;
        }
        return true;
    }
    void handshake(bool shutdown)
    {
        _result = Continue;
        do {
            SecBuffer     in[2] {{ULONG(_input.size()), SECBUFFER_TOKEN, _input.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc input {SECBUFFER_VERSION, 2, in};
            SecBuffer     out {0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc output {SECBUFFER_VERSION, 1, &out};
            ULONG         attributes = 0;
            TimeStamp     expiry;
            const auto    host     = QUrl::toAce(_host);
            const auto    wideHost = QString::fromLatin1(host);
            const DWORD   request  = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION |
                (_noClientCertificateRetry ? ISC_REQ_USE_SUPPLIED_CREDS : 0);
            const auto status = InitializeSecurityContextW(
                &_cred,
                _haveCtx ? &_ctx : nullptr,
                wideHost.isEmpty() ? nullptr
                                   : const_cast<wchar_t *>(reinterpret_cast<const wchar_t *>(wideHost.utf16())),
                request,
                0,
                SECURITY_NATIVE_DREP,
                shutdown || _input.isEmpty() ? nullptr : &input,
                0,
                &_ctx,
                &output,
                &attributes,
                &expiry);
            if (out.pvBuffer) {
                _network.append(static_cast<const char *>(out.pvBuffer), int(out.cbBuffer));
                FreeContextBuffer(out.pvBuffer);
            }
            if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS)
                _haveCtx = true;
            if (status == SEC_E_INCOMPLETE_MESSAGE)
                return;
            if (status == SEC_I_INCOMPLETE_CREDENTIALS && !_noClientCertificateRetry && !_input.isEmpty()) {
                // A server may request a client certificate without requiring one.
                // Retry the same unconsumed server flight at most once, with
                // explicit credentials only. SCH_CRED_NO_DEFAULT_CREDS prevents
                // selecting a certificate from the Windows user store.
                _noClientCertificateRetry = true;
                qInfo("qca-wincrypto: Schannel requested client credentials (0x%08lx); retrying without "
                      "a client certificate",
                      static_cast<unsigned long>(status));
                continue;
            }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
                qWarning("qca-wincrypto: Schannel stream TLS handshake failed (0x%08lx)",
                         static_cast<unsigned long>(status));
                _result = Error;
                return;
            }
            const auto extra = in[1].BufferType == SECBUFFER_EXTRA ? int(in[1].cbBuffer) : 0;
            if (extra)
                _input = _input.right(extra);
            else
                _input.clear();
            if (shutdown) {
                _result = Success;
                return;
            }
            if (status == SEC_E_OK) {
                if (_postHandshake) {
                    _postHandshake = false;
                    _result        = Success;
                    return;
                }
                if (!(attributes & ISC_RET_CONFIDENTIALITY) || !finishHandshake()) {
                    _result = Error;
                    return;
                }
                _handshaken = true;
                _result     = Success;
                if (!_input.isEmpty())
                    QTimer::singleShot(0, this, [this] { emit dtlsTimeout(); });
                return;
            }
        } while (!_input.isEmpty());
    }
    bool finishHandshake()
    {
        SecPkgContext_ConnectionInfo connection {};
        SecPkgContext_CipherInfo     cipher {};
        if (QueryContextAttributesW(&_ctx, SECPKG_ATTR_STREAM_SIZES, &_sizes) != SEC_E_OK ||
            QueryContextAttributesW(&_ctx, SECPKG_ATTR_CONNECTION_INFO, &connection) != SEC_E_OK ||
            QueryContextAttributesW(&_ctx, SECPKG_ATTR_CIPHER_INFO, &cipher) != SEC_E_OK)
            return false;
        if (connection.dwCipherStrength < ULONG(std::max(128, _minSSF)) || connection.dwCipherStrength > ULONG(_maxSSF))
            return false;
        if (_datagram ? !(connection.dwProtocol & SP_PROT_DTLS1_2)
                      : !(connection.dwProtocol & (SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT)))
            return false;
        _info.version     = _datagram                         ? TLS::DTLS_v1_2
            : (connection.dwProtocol & SP_PROT_TLS1_3_CLIENT) ? TLS::TLS_v1_3
                                                              : TLS::TLS_v1_2;
        _info.cipherSuite = QString::fromWCharArray(cipher.szCipherSuite);
        _info.cipherBits = _info.cipherMaxBits = int(connection.dwCipherStrength);
        if (!_suites.isEmpty() && !_suites.contains(_info.cipherSuite))
            return false;
        if (_info.version == TLS::TLS_v1_3) {
            // RFC 9266: 32 bytes with an empty context and this exact label.
            char                             label[] = "EXPORTER-Channel-Binding";
            SecPkgContext_KeyingMaterialInfo request {};
            request.cbLabel          = sizeof(label);
            request.pszLabel         = label;
            request.cbKeyingMaterial = 32;
            if (SetContextAttributesW(&_ctx, SECPKG_ATTR_KEYING_MATERIAL_INFO, &request, sizeof(request)) == SEC_E_OK) {
                SecPkgContext_KeyingMaterial material {};
                if (QueryContextAttributesW(&_ctx, SECPKG_ATTR_KEYING_MATERIAL, &material) == SEC_E_OK) {
                    if (material.cbKeyingMaterial == 32 && material.pbKeyingMaterial) {
                        _exporter.resize(32);
                        std::memcpy(_exporter.data(), material.pbKeyingMaterial, 32);
                    }
                    if (material.pbKeyingMaterial) {
                        secureWipe(material.pbKeyingMaterial, material.cbKeyingMaterial);
                        FreeContextBuffer(material.pbKeyingMaterial);
                    }
                }
            }
        }
        PCCERT_CONTEXT cert = nullptr;
        if (QueryContextAttributesW(&_ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK)
            return false;
        _peer.clear();
        auto add = [this](PCCERT_CONTEXT c) {
            Certificate qcaCert = Certificate::fromDER(
                QByteArray(reinterpret_cast<const char *>(c->pbCertEncoded), int(c->cbCertEncoded)),
                nullptr,
                provider()->name());
            if (!qcaCert.isNull() && !_peer.contains(qcaCert))
                _peer += qcaCert;
        };
        add(cert);
        PCCERT_CONTEXT intermediate = nullptr;
        while ((intermediate = CertEnumCertificatesInStore(cert->hCertStore, intermediate)))
            add(intermediate);
        QList<QByteArray> trusted, crls;
        for (const auto &root : _trusted.certificates())
            trusted += root.toDER();
        for (const auto &crl : _trusted.crls())
            crls += crl.toDER();
        _validity = validateCertificate(
            cert, cert->hCertStore, trusted, crls, _server ? UsageTLSClient : UsageTLSServer, ValidateAll, _host);
        CertFreeCertificateContext(cert);
        // As in other QCA providers, report validation to TLS::peerIdentityResult.
        // The caller must accept the identity before sending application data.
        return !_peer.isEmpty();
    }
    bool decode()
    {
        int consumed = 0;
        // Allocate once before collecting plaintext, rather than leaving old
        // plaintext allocations behind on QByteArray growth.
        _application.reserve(_application.size() + _input.size());
        while (consumed < _input.size()) {
            const auto    remaining = int(_input.size()) - consumed;
            SecBuffer     buffers[4] {{ULONG(remaining), SECBUFFER_DATA, _input.data() + consumed},
                                      {0, SECBUFFER_EMPTY, nullptr},
                                      {0, SECBUFFER_EMPTY, nullptr},
                                      {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc {SECBUFFER_VERSION, 4, buffers};
            const auto    status = DecryptMessage(&_ctx, &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE)
                break;
            if (status != SEC_E_OK && status != SEC_I_CONTEXT_EXPIRED && status != SEC_I_RENEGOTIATE)
                return false;
            int extra = 0;
            for (const auto &b : buffers) {
                if (b.BufferType == SECBUFFER_DATA && b.cbBuffer) {
                    const auto *data = static_cast<const std::uint8_t *>(b.pvBuffer);
                    _application.insert(_application.end(), data, data + b.cbBuffer);
                    secureWipe(b.pvBuffer, b.cbBuffer);
                }
                if (b.BufferType == SECBUFFER_EXTRA)
                    extra = int(b.cbBuffer);
            }
            if (extra > remaining || (extra == remaining && status != SEC_I_RENEGOTIATE))
                return false;
            // DecryptMessage operates in place. Walk a large update once and
            // move only its final incomplete record, avoiding quadratic copies.
            secureWipe(_input.data() + consumed, std::size_t(remaining - extra));
            consumed += remaining - extra;
            if (status == SEC_I_CONTEXT_EXPIRED) {
                _eof = true;
                break;
            }
            if (status == SEC_I_RENEGOTIATE) {
                // TLS 1.3 uses this status for tickets/KeyUpdate, not just a
                // new handshake. Keep its record state in the same context.
                // TLS <=1.2 renegotiation is deliberately unsupported.
                if (_info.version != TLS::TLS_v1_3)
                    return false;
                _input.remove(0, consumed);
                consumed       = 0;
                _postHandshake = true;
                handshake(false);
                if (_result == Error)
                    return false;
                if (_postHandshake)
                    return true;
            }
        }
        _input.remove(0, consumed);
        return true;
    }
    bool encode(const char *data, int length)
    {
        if (!_sizes.cbMaximumMessage || _eof || length > std::numeric_limits<int>::max() - _encoded)
            return false;
        for (int offset = 0; offset < length;) {
            const auto  size = std::min(ULONG(length - offset), _sizes.cbMaximumMessage);
            SecureArray record(int(_sizes.cbHeader + size + _sizes.cbTrailer));
            std::memcpy(record.data() + _sizes.cbHeader, data + offset, size);
            SecBuffer buffers[4] {{_sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                                  {size, SECBUFFER_DATA, record.data() + _sizes.cbHeader},
                                  {_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + _sizes.cbHeader + size},
                                  {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc {SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&_ctx, 0, &desc, 0) != SEC_E_OK)
                return false;
            for (const auto &b : buffers)
                if (b.cbBuffer)
                    _network.append(static_cast<const char *>(b.pvBuffer), int(b.cbBuffer));
            _encoded += int(size);
            offset += int(size);
        }
        return true;
    }
    const bool                _datagram;
    QTimer                    _retransmit;
    QElapsedTimer             _handshakeTime, _duplicateReply;
    bool                      _resend = false;
    int                       _mtu = 1400, _retries = 0, _retryDelay = 1000;
    QQueue<QByteArray>        _packets;
    QList<QByteArray>         _flight;
    std::deque<Bytes>         _plaintext;
    QByteArray                _peerAddress, _lastPeerPacket;
    QStringList               _profiles;
    TLS::SRTPKeyingMaterial   _material;
    CertificateChain          _localCertificate;
    PrivateKey                _localKey;
    QTimer                    _ready;
    CredHandle                _cred {};
    CtxtHandle                _ctx {};
    SecPkgContext_StreamSizes _sizes {};
    bool                      _haveCred = false, _haveCtx = false, _configured = false, _server = false;
    bool                      _handshaken = false, _eof = false, _closing = false;
    bool                      _postHandshake = false, _noClientCertificateRetry = false;
    int                       _minSSF = 0, _maxSSF = 256, _encoded = 0;
    QString                   _host;
    QStringList               _suites;
    QByteArray                _input, _network;
    Bytes                     _application;
    Bytes                     _deferred;
    SecureArray               _exporter;
    CertificateCollection     _trusted;
    CertificateChain          _peer;
    Validity                  _validity = ErrorValidityUnknown;
    SessionInfo               _info {};
    Result                    _result = Error;
};
} // namespace

Provider::Context *createTLSContext(Provider *provider, bool datagram)
{
    return new WinTLS(provider, datagram);
}

} // namespace wincryptoQCAPlugin
