/*
 * Copyright (C) 2004-2007  Justin Karneges <justin@affinix.com>
 * Copyright (C) 2004-2006  Brad Hards <bradh@frogmouth.net>
 * Copyright (C) 2013-2016  Ivan Romanov <drizt@land.ru>
 * Copyright (C) 2017       Fabian Vogt <fabian@ritter-vogt.de>
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

#include "basetlscontext.h"

#include "certcontext.h"
#include "pkeycontext.h"
#include "utils.h"

#include <QDebug>
#include <QUrl>

#include <openssl/err.h>
#include <openssl/srtp.h>

#include <cstring>

namespace opensslQCAPlugin {

namespace {

struct SRTPProfileInfo
{
    const char   *qcaName;
    const char   *opensslName;
    unsigned long id;
    int           masterKeyLength;
    int           masterSaltLength;
};

// Public QCA names follow the IANA DTLS-SRTP registry.  OpenSSL uses
// historical shortened names for the two RFC 5764 AES-CM profiles and a
// prefixed name for the RFC 8723 double profiles.
static const SRTPProfileInfo srtpProfileInfo[] = {
    {"SRTP_AES128_CM_HMAC_SHA1_80", "SRTP_AES128_CM_SHA1_80", 0x0001, 16, 14},
    {"SRTP_AES128_CM_HMAC_SHA1_32", "SRTP_AES128_CM_SHA1_32", 0x0002, 16, 14},
    {"SRTP_AEAD_AES_128_GCM", "SRTP_AEAD_AES_128_GCM", 0x0007, 16, 12},
    {"SRTP_AEAD_AES_256_GCM", "SRTP_AEAD_AES_256_GCM", 0x0008, 32, 12},
    {"DOUBLE_AEAD_AES_128_GCM_AEAD_AES_128_GCM", "SRTP_DOUBLE_AEAD_AES_128_GCM_AEAD_AES_128_GCM", 0x0009, 32, 24},
    {"DOUBLE_AEAD_AES_256_GCM_AEAD_AES_256_GCM", "SRTP_DOUBLE_AEAD_AES_256_GCM_AEAD_AES_256_GCM", 0x000a, 64, 24},
    {"SRTP_ARIA_128_CTR_HMAC_SHA1_80", "SRTP_ARIA_128_CTR_HMAC_SHA1_80", 0x000b, 16, 14},
    {"SRTP_ARIA_128_CTR_HMAC_SHA1_32", "SRTP_ARIA_128_CTR_HMAC_SHA1_32", 0x000c, 16, 14},
    {"SRTP_ARIA_256_CTR_HMAC_SHA1_80", "SRTP_ARIA_256_CTR_HMAC_SHA1_80", 0x000d, 32, 14},
    {"SRTP_ARIA_256_CTR_HMAC_SHA1_32", "SRTP_ARIA_256_CTR_HMAC_SHA1_32", 0x000e, 32, 14},
    {"SRTP_AEAD_ARIA_128_GCM", "SRTP_AEAD_ARIA_128_GCM", 0x000f, 16, 12},
    {"SRTP_AEAD_ARIA_256_GCM", "SRTP_AEAD_ARIA_256_GCM", 0x0010, 32, 12},
};

const SRTPProfileInfo *srtpProfileByQCAName(const QString &name)
{
    for (const SRTPProfileInfo &profile : srtpProfileInfo) {
        if (name == QLatin1String(profile.qcaName))
            return &profile;
    }
    return nullptr;
}

const SRTPProfileInfo *srtpProfileById(unsigned long id)
{
    for (const SRTPProfileInfo &profile : srtpProfileInfo) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

SecureArray secureSlice(const SecureArray &source, int offset, int length)
{
    if (offset < 0 || length < 0 || offset + length > source.size())
        return SecureArray();

    SecureArray result(length);
    if (length > 0)
        std::memcpy(result.data(), source.constData() + offset, static_cast<size_t>(length));
    return result;
}

} // namespace

BaseOsslTLSContext::BaseOsslTLSContext(Provider *p, const QString &type)
    : TLSContext(p, type)
{
    ssl     = nullptr;
    context = nullptr;
    rbio    = nullptr;
    wbio    = nullptr;
    reset();
}

BaseOsslTLSContext::~BaseOsslTLSContext()
{
    // usually implementation should call reset() in its destructor
}

Provider::Context *BaseOsslTLSContext::clone() const
{
    return nullptr;
}

bool BaseOsslTLSContext::canCompress() const
{
    // TODO
    return false;
}

bool BaseOsslTLSContext::canSetHostName() const
{
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    return true;
#else
    return false;
#endif
}

int BaseOsslTLSContext::maxSSF() const
{
    // TODO
    return 256;
}

void BaseOsslTLSContext::setConstraints(int minSSF, int maxSSF)
{
    constraintSSF = true;
    constraintMin = minSSF;
    constraintMax = maxSSF;
    constraintSuites.clear();
}

void BaseOsslTLSContext::setConstraints(const QStringList &cipherSuiteList)
{
    constraintSSF    = false;
    constraintSuites = cipherSuiteList;
}

void BaseOsslTLSContext::setup(bool serverMode, const QString &hostName, bool compress)
{
    serv           = serverMode;
    targetHostName = serverMode ? QString() : hostName;
    receivedHostName.clear();
    clientHelloSeen         = false;
    clientHelloRetryPending = false;
    Q_UNUSED(compress); // TODO
}

void BaseOsslTLSContext::setTrustedCertificates(const CertificateCollection &_trusted)
{
    trusted = _trusted;
}

void BaseOsslTLSContext::setIssuerList(const QList<CertificateInfoOrdered> &issuerList)
{
    Q_UNUSED(issuerList); // TODO
}

void BaseOsslTLSContext::setCertificate(const CertificateChain &_cert, const PrivateKey &_key)
{
    if (!_cert.isEmpty())
        cert = _cert.primary(); // TODO: take the whole chain

    key = _key;

    if (ssl && !cert.isNull() && !key.isNull() && !applyCertificate()) {
        ERR_print_errors_cb(&BaseOsslTLSContext::ssl_error_callback, this);
    }
}

void BaseOsslTLSContext::setSessionId(const TLSSessionContext &id)
{
    // TODO
    Q_UNUSED(id);
}

bool BaseOsslTLSContext::clientHelloReceived() const
{
    return clientHelloSeen;
}

bool BaseOsslTLSContext::serverHelloReceived() const
{
    // TODO
    return false;
}

QString BaseOsslTLSContext::hostName() const
{
    return receivedHostName;
}

QStringList BaseOsslTLSContext::channelBindingTypes() const
{
    QStringList types;
    if (!ssl || mode != Active || SSL_is_init_finished(ssl) != 1)
        return types;

    const int version = SSL_version(ssl);
#ifdef TLS1_3_VERSION
    if (version == TLS1_3_VERSION) {
        types += QStringLiteral("tls-exporter");
        return types;
    }
#endif
#ifdef DTLS1_3_VERSION
    if (version == DTLS1_3_VERSION) {
        types += QStringLiteral("tls-exporter");
        return types;
    }
#endif

    // RFC 7677 permits tls-unique when the extended master secret was
    // negotiated, or when session resumption was not used.  tls-unique is
    // not defined for TLS 1.3.
    if (type() == QLatin1String("tls") && (SSL_get_extms_support(ssl) == 1 || SSL_session_reused(ssl) != 1)) {
        types += QStringLiteral("tls-unique");
    }

    return types;
}

QByteArray BaseOsslTLSContext::channelBinding(const QString &type) const
{
    if (!ssl || mode != Active || SSL_is_init_finished(ssl) != 1)
        return QByteArray();

    if (type == QLatin1String("tls-exporter")) {
        const int version   = SSL_version(ssl);
        bool      supported = false;
#ifdef TLS1_3_VERSION
        supported = (version == TLS1_3_VERSION);
#endif
#ifdef DTLS1_3_VERSION
        supported = supported || (version == DTLS1_3_VERSION);
#endif
        if (!supported)
            return QByteArray();

        static const char exporterLabel[] = "EXPORTER-Channel-Binding";
        QByteArray        result(32, '\0');
        if (SSL_export_keying_material(ssl,
                                       reinterpret_cast<unsigned char *>(result.data()),
                                       static_cast<size_t>(result.size()),
                                       exporterLabel,
                                       sizeof(exporterLabel) - 1,
                                       nullptr,
                                       0,
                                       0) != 1) {
            return QByteArray();
        }
        return result;
    }

    if (type == QLatin1String("tls-unique")) {
        if (!channelBindingTypes().contains(QStringLiteral("tls-unique")))
            return QByteArray();

        // tls-unique is the first Finished message sent in the most recent
        // handshake.  The client sends it first in a full handshake; the
        // server sends it first in an abbreviated (resumed) handshake.
        const bool resumed            = SSL_session_reused(ssl) == 1;
        const bool localFinishedFirst = serv ? resumed : !resumed;

        unsigned char finished[64];
        const size_t  size = localFinishedFirst ? SSL_get_finished(ssl, finished, sizeof(finished))
                                                : SSL_get_peer_finished(ssl, finished, sizeof(finished));
        if (size == 0 || size > sizeof(finished))
            return QByteArray();

        return QByteArray(reinterpret_cast<const char *>(finished), static_cast<int>(size));
    }

    return QByteArray();
}

QStringList supportedOsslSRTPProfiles()
{
    static const QStringList profiles = []() {
        QStringList result;
#if !defined(OPENSSL_NO_SRTP)
        for (const SRTPProfileInfo &profile : srtpProfileInfo) {
            SSL_CTX *testContext = SSL_CTX_new(DTLS_method());
            if (!testContext)
                break;

            const int setResult = SSL_CTX_set_tlsext_use_srtp(testContext, profile.opensslName);
            SSL_CTX_free(testContext);
            ERR_clear_error();

            if (setResult == 0)
                result += QLatin1String(profile.qcaName);
        }
#endif
        return result;
    }();

    return profiles;
}

QStringList BaseOsslTLSContext::supportedSRTPProfiles() const
{
    if (type() != QLatin1String("dtls"))
        return QStringList();

    return supportedOsslSRTPProfiles();
}

void BaseOsslTLSContext::setSRTPProfiles(const QStringList &profiles)
{
    srtpProfiles = profiles;
}

QString BaseOsslTLSContext::selectedSRTPProfile() const
{
#if !defined(OPENSSL_NO_SRTP)
    if (!ssl || mode != Active || type() != QLatin1String("dtls") || SSL_is_init_finished(ssl) != 1)
        return QString();

    const SRTP_PROTECTION_PROFILE *selected = SSL_get_selected_srtp_profile(ssl);
    if (!selected)
        return QString();

    const SRTPProfileInfo *profile = srtpProfileById(selected->id);
    return profile ? QLatin1String(profile->qcaName) : QString();
#else
    return QString();
#endif
}

TLS::SRTPKeyingMaterial BaseOsslTLSContext::srtpKeyingMaterial() const
{
#if !defined(OPENSSL_NO_SRTP)
    if (!ssl || mode != Active || type() != QLatin1String("dtls") || SSL_is_init_finished(ssl) != 1)
        return TLS::SRTPKeyingMaterial();

    const SRTP_PROTECTION_PROFILE *selected = SSL_get_selected_srtp_profile(ssl);
    if (!selected)
        return TLS::SRTPKeyingMaterial();

    const SRTPProfileInfo *profile = srtpProfileById(selected->id);
    if (!profile)
        return TLS::SRTPKeyingMaterial();

    const int halfLength  = profile->masterKeyLength + profile->masterSaltLength;
    const int totalLength = 2 * halfLength;
    if (totalLength <= 0)
        return TLS::SRTPKeyingMaterial();

    SecureArray       material(totalLength);
    static const char exporterLabel[] = "EXTRACTOR-dtls_srtp";
    if (SSL_export_keying_material(ssl,
                                   reinterpret_cast<unsigned char *>(material.data()),
                                   static_cast<size_t>(material.size()),
                                   exporterLabel,
                                   sizeof(exporterLabel) - 1,
                                   nullptr,
                                   0,
                                   0) != 1) {
        return TLS::SRTPKeyingMaterial();
    }

    int               offset          = 0;
    const SecureArray clientMasterKey = secureSlice(material, offset, profile->masterKeyLength);
    offset += profile->masterKeyLength;
    const SecureArray serverMasterKey = secureSlice(material, offset, profile->masterKeyLength);
    offset += profile->masterKeyLength;
    const SecureArray clientMasterSalt = secureSlice(material, offset, profile->masterSaltLength);
    offset += profile->masterSaltLength;
    const SecureArray serverMasterSalt = secureSlice(material, offset, profile->masterSaltLength);
    material.clear();

    if (serv) {
        return TLS::SRTPKeyingMaterial(
            QLatin1String(profile->qcaName), serverMasterKey, serverMasterSalt, clientMasterKey, clientMasterSalt);
    }

    return TLS::SRTPKeyingMaterial(
        QLatin1String(profile->qcaName), clientMasterKey, clientMasterSalt, serverMasterKey, serverMasterSalt);
#else
    return TLS::SRTPKeyingMaterial();
#endif
}

bool BaseOsslTLSContext::certificateRequested() const
{
    // TODO
    return false;
}

QList<CertificateInfoOrdered> BaseOsslTLSContext::issuerList() const
{
    // TODO
    return QList<CertificateInfoOrdered>();
}

bool BaseOsslTLSContext::waitForResultsReady(int msecs)
{
    // TODO: for now, all operations block anyway
    Q_UNUSED(msecs);
    return true;
}

TLSContext::Result BaseOsslTLSContext::result() const
{
    return result_result;
}

int BaseOsslTLSContext::encoded() const
{
    return result_encoded;
}

bool BaseOsslTLSContext::eof() const
{
    return v_eof;
}

Validity BaseOsslTLSContext::peerCertificateValidity() const
{
    return vr;
}

CertificateChain BaseOsslTLSContext::peerCertificateChain() const
{
    // TODO: support whole chain
    CertificateChain chain;
    if (!peercert.isNull())
        chain.append(peercert);
    return chain;
}

void BaseOsslTLSContext::doResultsReady()
{
    QMetaObject::invokeMethod(this, "resultsReady", Qt::QueuedConnection);
}

int BaseOsslTLSContext::doConnect()
{
    int ret = SSL_connect(ssl);
    if (ret < 0) {
        int x = SSL_get_error(ssl, ret);
        if (x == SSL_ERROR_WANT_CONNECT || x == SSL_ERROR_WANT_READ || x == SSL_ERROR_WANT_WRITE)
            return TryAgain;
        else
            return Bad;
    } else if (ret == 0)
        return Bad;
    return Good;
}

int BaseOsslTLSContext::doAccept()
{
    int ret = SSL_accept(ssl);
    if (ret < 0) {
        int x = SSL_get_error(ssl, ret);
        if (x == SSL_ERROR_WANT_CONNECT || x == SSL_ERROR_WANT_READ || x == SSL_ERROR_WANT_WRITE
#ifdef SSL_ERROR_WANT_CLIENT_HELLO_CB
            || x == SSL_ERROR_WANT_CLIENT_HELLO_CB
#endif
        )
            return TryAgain;
        else
            return Bad;
    } else if (ret == 0)
        return Bad;
    return Good;
}

int BaseOsslTLSContext::doHandshake()
{
    int ret = SSL_do_handshake(ssl);
    if (ret < 0) {
        int x = SSL_get_error(ssl, ret);
        if (x == SSL_ERROR_WANT_READ || x == SSL_ERROR_WANT_WRITE)
            return TryAgain;
        else
            return Bad;
    } else if (ret == 0)
        return Bad;
    return Good;
}

void BaseOsslTLSContext::shutdown()
{
    mode = Closing;
}

int BaseOsslTLSContext::doShutdown()
{
    int ret = SSL_shutdown(ssl);
    if (ret >= 1)
        return Good;
    else {
        if (ret == 0)
            return TryAgain;
        int x = SSL_get_error(ssl, ret);
        if (x == SSL_ERROR_WANT_READ || x == SSL_ERROR_WANT_WRITE)
            return TryAgain;
        return Bad;
    }
}

QByteArray BaseOsslTLSContext::unprocessed()
{
    QByteArray a;
    int        size = BIO_pending(rbio);
    if (size <= 0)
        return a;
    a.resize(size);

    int r = BIO_read(rbio, a.data(), size);
    if (r <= 0) {
        a.resize(0);
        return a;
    }
    if (r != size)
        a.resize(r);
    return a;
}

BIO *BaseOsslTLSContext::makeWriteBIO()
{
    return BIO_new(BIO_s_mem());
}

BIO *BaseOsslTLSContext::makeReadBIO()
{
    return BIO_new(BIO_s_mem());
}

void BaseOsslTLSContext::reset()
{
    if (ssl) {
        SSL_free(ssl);
        ssl  = nullptr;
        rbio = nullptr;
        wbio = nullptr;
    }
    if (context) {
        SSL_CTX_free(context);
        context = nullptr;
    }

    cert = Certificate();
    key  = PrivateKey();

    targetHostName.clear();
    receivedHostName.clear();
    srtpProfiles.clear();
    clientHelloSeen         = false;
    clientHelloRetryPending = false;

    mode     = Idle;
    peercert = Certificate();
    vr       = ErrorValidityUnknown;
    v_eof    = false;
}

int BaseOsslTLSContext::ssl_verify_callback(int preverify_ok, X509_STORE_CTX *x509_ctx)
{
    Q_UNUSED(preverify_ok);
    Q_UNUSED(x509_ctx);

    // don't terminate handshake in case of verification failure
    return 1;
}

int BaseOsslTLSContext::ssl_error_callback(const char *message, size_t len, void *user_data)
{
    Q_UNUSED(len)
    auto context = reinterpret_cast<BaseOsslTLSContext *>(user_data);
    QCA_logTextMessage(
        QStringLiteral("%1: ssl_error_callback: %2").arg(context->type(), QString::fromLocal8Bit(message, len)),
        Logger::Error);
    return 1;
}

QString BaseOsslTLSContext::clientHelloHostName(SSL *ssl)
{
#if OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined(LIBRESSL_VERSION_NUMBER) && defined(TLSEXT_TYPE_server_name)
    const unsigned char *extension       = nullptr;
    size_t               extensionLength = 0;

    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &extension, &extensionLength) != 1 ||
        extensionLength < 2) {
        return QString();
    }

    const size_t listLength = (static_cast<size_t>(extension[0]) << 8) | extension[1];
    if (listLength + 2 != extensionLength)
        return QString();

    const unsigned char *position  = extension + 2;
    size_t               remaining = listLength;

    while (remaining >= 3) {
        const unsigned int nameType   = position[0];
        const size_t       nameLength = (static_cast<size_t>(position[1]) << 8) | position[2];
        position += 3;
        remaining -= 3;

        if (nameLength > remaining)
            return QString();

        if (nameType == TLSEXT_NAMETYPE_host_name) {
            const QByteArray aceName(reinterpret_cast<const char *>(position), static_cast<int>(nameLength));
            if (aceName.isEmpty() || aceName.contains('\0'))
                return QString();
            return QUrl::fromAce(aceName);
        }

        position += nameLength;
        remaining -= nameLength;
    }
#else
    Q_UNUSED(ssl);
#endif

    return QString();
}

int BaseOsslTLSContext::ssl_client_hello_callback(SSL *ssl, int *alert, void *user_data)
{
    Q_UNUSED(alert);

#if OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined(LIBRESSL_VERSION_NUMBER) && defined(TLSEXT_TYPE_server_name)
    auto *tlsContext = static_cast<BaseOsslTLSContext *>(user_data);
    if (!tlsContext || !tlsContext->serv)
        return SSL_CLIENT_HELLO_SUCCESS;

    if (tlsContext->clientHelloRetryPending) {
        tlsContext->clientHelloRetryPending = false;
        return SSL_CLIENT_HELLO_SUCCESS;
    }

    if (tlsContext->clientHelloSeen)
        return SSL_CLIENT_HELLO_SUCCESS;

    tlsContext->clientHelloSeen  = true;
    tlsContext->receivedHostName = clientHelloHostName(ssl);

    if (tlsContext->receivedHostName.isEmpty())
        return SSL_CLIENT_HELLO_SUCCESS;

    tlsContext->clientHelloRetryPending = true;
    return SSL_CLIENT_HELLO_RETRY;
#else
    Q_UNUSED(ssl);
    Q_UNUSED(user_data);
    return 1;
#endif
}

int BaseOsslTLSContext::ssl_servername_callback(SSL *ssl, int *alert, void *user_data)
{
    Q_UNUSED(ssl);
    Q_UNUSED(alert);

    const auto *tlsContext = static_cast<const BaseOsslTLSContext *>(user_data);
    return tlsContext && !tlsContext->receivedHostName.isEmpty() ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
}

QStringList BaseOsslTLSContext::supportedCipherSuites(const TLS::Version &version) const
{
    SSL_CTX *ctx = nullptr;

    // most likely scenario first
    if ((version >= TLS::TLS_vMIN && version <= TLS::TLS_vMAX) || version == TLS::TLS_v1) {
        static struct
        {
            TLS::Version ver;
            int          ssl_ver;
        } limits[] = {
            {TLS::TLS_v1, TLS1_VERSION},
            {TLS::TLS_v1_1, TLS1_1_VERSION},
            {TLS::TLS_v1_2, TLS1_2_VERSION},
            {TLS::DTLS_v1, TLS1_1_VERSION},
            {TLS::DTLS_v1_2, TLS1_2_VERSION},
#ifdef TLS1_3_VERSION
            {TLS::TLS_v1_3, TLS1_3_VERSION},
            {TLS::DTLS_v1_3, TLS1_3_VERSION},
#endif
        };
        for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
            if (limits[i].ver == version) {
                auto method = (limits[i].ver >= TLS::DTLS_v1 && limits[i].ver <= TLS::DTLS_vMAX) ? DTLS_client_method()
                                                                                                 : TLS_client_method();
                ctx         = SSL_CTX_new(method);
                SSL_CTX_set_min_proto_version(ctx, limits[i].ssl_ver);
                SSL_CTX_set_max_proto_version(ctx, limits[i].ssl_ver);
                break;
            }
        }
    }
#if !defined(OPENSSL_NO_SSL3_METHOD) && (OPENSSL_VERSION_NUMBER < 0x40000000L)
    else if (version == TLS::SSL_v3) {
        // Here should be used TLS_client_method() but on Fedora
        // it doesn't return any SSL ciphers.
        ctx = SSL_CTX_new(SSLv3_client_method());
        SSL_CTX_set_min_proto_version(ctx, SSL3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, SSL3_VERSION);
    }
#endif
    else {
        qWarning("Unexpected enum in cipherSuites");
        ctx = nullptr;
    }

    if (nullptr == ctx)
        return QStringList();

    SSL *ssl = SSL_new(ctx);
    if (nullptr == ssl) {
        SSL_CTX_free(ctx);
        return QStringList();
    }

    STACK_OF(SSL_CIPHER) *sk = SSL_get1_supported_ciphers(ssl);
    QStringList cipherList;
    for (int i = 0; i < sk_SSL_CIPHER_num(sk); ++i) {
        const SSL_CIPHER *thisCipher = sk_SSL_CIPHER_value(sk, i);
#ifndef LIBRESSL_VERSION_NUMBER
        cipherList += QString::fromLatin1(SSL_CIPHER_standard_name(thisCipher));
#else
        cipherList += QString::fromLatin1(SSL_CIPHER_get_name(thisCipher));
#endif
    }
    sk_SSL_CIPHER_free(sk);

    SSL_free(ssl);
    SSL_CTX_free(ctx);

    return cipherList;
}

TLSContext::SessionInfo BaseOsslTLSContext::sessionInfo() const
{
    SessionInfo sessInfo;

    SSL_SESSION *session  = SSL_get0_session(ssl);
    sessInfo.isCompressed = (0 != SSL_SESSION_get_compress_id(session));
    int ssl_version       = SSL_version(ssl);

#ifdef TLS1_3_VERSION
    if (ssl_version == TLS1_3_VERSION)
        sessInfo.version = TLS::TLS_v1_3;
    else
#endif
        if (ssl_version == TLS1_2_VERSION)
        sessInfo.version = TLS::TLS_v1_2;
    else if (ssl_version == TLS1_1_VERSION)
        sessInfo.version = TLS::TLS_v1_1;
    else if (ssl_version == DTLS1_VERSION)
        sessInfo.version = TLS::DTLS_v1;
#ifdef DTLS1_2_VERSION
    else if (ssl_version == DTLS1_2_VERSION)
        sessInfo.version = TLS::DTLS_v1_2;
#endif
    else if (ssl_version == TLS1_VERSION)
        sessInfo.version = TLS::TLS_v1;
    else if (ssl_version == SSL3_VERSION)
        sessInfo.version = TLS::SSL_v3;
    else if (ssl_version == SSL2_VERSION)
        sessInfo.version = TLS::SSL_v2;
    else {
        qDebug("unexpected version response: %s", SSL_get_version(ssl));
        sessInfo.version = TLS::TLS_v1_2;
    }

#ifndef LIBRESSL_VERSION_NUMBER
    sessInfo.cipherSuite = QString::fromLatin1(SSL_CIPHER_standard_name(SSL_get_current_cipher(ssl)));
#else
    sessInfo.cipherSuite = QString::fromLatin1(SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
#endif

    sessInfo.cipherMaxBits = SSL_get_cipher_bits(ssl, &(sessInfo.cipherBits));

    sessInfo.id = nullptr; // TODO: session resuming

    return sessInfo;
}

bool BaseOsslTLSContext::applyCertificate()
{
    if (!ssl || cert.isNull() || key.isNull())
        return true;

    PrivateKey nkey = key;

    const PKeyContext *tmp_kc = static_cast<const PKeyContext *>(nkey.context());

    if (!tmp_kc->sameProvider(this)) {
        EVP_PKEY *pkey = createPkeyFromExisting(key.toRSA());
        if (!pkey)
            return false;

        MyPKeyContext *pk = new MyPKeyContext(provider());
        PKeyBase      *k  = pk->pkeyToBase(pkey, true); // does an EVP_PKEY_free()
        if (!k) {
            delete pk;
            return false;
        }
        pk->k = k;
        nkey.change(pk);
    }

    const MyCertContext *cc = static_cast<const MyCertContext *>(cert.context());
    const MyPKeyContext *kc = static_cast<const MyPKeyContext *>(nkey.context());

    return SSL_use_certificate(ssl, cc->item.cert) == 1 && SSL_use_PrivateKey(ssl, kc->get_pkey()) == 1 &&
        SSL_check_private_key(ssl) == 1;
}

bool BaseOsslTLSContext::applyConstraints()
{
    if (constraintSSF && (constraintMin < 0 || (constraintMax != -1 && constraintMax < constraintMin)))
        return false;
    QByteArray  legacy, modern;
    QStringList matched;
    const auto  append = [&](const SSL_CIPHER *cipher) {
        const char *name  = SSL_CIPHER_get_name(cipher);
        const bool  tls13 = qstrcmp(SSL_CIPHER_get_version(cipher), "TLSv1.3") == 0;
        if (tls13 && type() == QLatin1String("dtls"))
            return;
        auto &list = tls13 ? modern : legacy;
        if (!list.isEmpty())
            list += ':';
        list += name;
    };
    // Restrict OpenSSL's defaults; never lower its configured security level.
    STACK_OF(SSL_CIPHER) *available = SSL_CTX_get_ciphers(context);
    if (constraintSSF) {
        for (int i = 0; i < sk_SSL_CIPHER_num(available); ++i) {
            const auto *cipher = sk_SSL_CIPHER_value(available, i);
            const int   bits   = SSL_CIPHER_get_bits(cipher, nullptr);
            if (bits >= constraintMin && (constraintMax == -1 || bits <= constraintMax))
                append(cipher);
        }
    } else {
        // Preserve caller preference order and reject unknown names atomically.
        for (const auto &requested : constraintSuites) {
            bool found = false;
            for (int i = 0; i < sk_SSL_CIPHER_num(available); ++i) {
                const auto *cipher = sk_SSL_CIPHER_value(available, i);
                if (type() == QLatin1String("dtls") && qstrcmp(SSL_CIPHER_get_version(cipher), "TLSv1.3") == 0)
                    continue;
                bool matches = requested == QLatin1String(SSL_CIPHER_get_name(cipher));
#if OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined(LIBRESSL_VERSION_NUMBER)
                const char *standard = SSL_CIPHER_standard_name(cipher);
                matches              = matches || (standard && requested == QLatin1String(standard));
#endif
                if (matches) {
                    if (!matched.contains(requested))
                        append(cipher);
                    matched.append(requested);
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    if (legacy.isEmpty() && modern.isEmpty())
        return false;
    const int legacyResult = SSL_CTX_set_cipher_list(context, legacy.constData());
    if (!legacy.isEmpty() && legacyResult != 1)
        return false;
    if (legacy.isEmpty())
        ERR_clear_error(); // OpenSSL reports no legacy match; TLS 1.3 may still be allowed.
#if OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined(LIBRESSL_VERSION_NUMBER)
    // Empty explicitly disables TLS 1.3 suites, preventing bypass of a legacy-only list.
    if (SSL_CTX_set_ciphersuites(context, modern.constData()) != 1)
        return false;
    if (type() != QLatin1String("dtls")) {
        if (modern.isEmpty() && SSL_CTX_set_max_proto_version(context, TLS1_2_VERSION) != 1)
            return false;
        if (legacy.isEmpty() && SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1)
            return false;
    }
#endif
    return true;
}

bool BaseOsslTLSContext::init()
{
    context = SSL_CTX_new(method);
    if (!context)
        return false;
    if (!applyConstraints()) {
        SSL_CTX_free(context);
        context = nullptr;
        return false;
    }

#if OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined(LIBRESSL_VERSION_NUMBER) && defined(TLSEXT_TYPE_server_name)
    if (serv) {
        SSL_CTX_set_client_hello_cb(context, &BaseOsslTLSContext::ssl_client_hello_callback, this);
        SSL_CTX_set_tlsext_servername_callback(context, &BaseOsslTLSContext::ssl_servername_callback);
        SSL_CTX_set_tlsext_servername_arg(context, this);
    }
#endif

#if !defined(OPENSSL_NO_SRTP)
    if (!srtpProfiles.isEmpty()) {
        if (type() != QLatin1String("dtls")) {
            SSL_CTX_free(context);
            context = nullptr;
            return false;
        }

        QByteArray profileList;
        for (const QString &name : srtpProfiles) {
            const SRTPProfileInfo *profile = srtpProfileByQCAName(name);
            if (!profile) {
                SSL_CTX_free(context);
                context = nullptr;
                return false;
            }
            if (!profileList.isEmpty())
                profileList += ':';
            profileList += profile->opensslName;
        }

        if (SSL_CTX_set_tlsext_use_srtp(context, profileList.constData()) != 0) {
            ERR_print_errors_cb(&BaseOsslTLSContext::ssl_error_callback, this);
            SSL_CTX_free(context);
            context = nullptr;
            return false;
        }
    }
#else
    if (!srtpProfiles.isEmpty()) {
        SSL_CTX_free(context);
        context = nullptr;
        return false;
    }
#endif

    // setup the cert store
    {
        X509_STORE              *store     = SSL_CTX_get_cert_store(context);
        const QList<Certificate> cert_list = trusted.certificates();
        const QList<CRL>         crl_list  = trusted.crls();
        int                      n;
        for (n = 0; n < cert_list.count(); ++n) {
            const MyCertContext *cc = static_cast<const MyCertContext *>(cert_list[n].context());
            X509                *x  = cc->item.cert;
            // X509_STORE_add_cert() increments the certificate reference count.
            X509_STORE_add_cert(store, x);
        }
        for (n = 0; n < crl_list.count(); ++n) {
            const MyCRLContext *cc = static_cast<const MyCRLContext *>(crl_list[n].context());
            X509_CRL           *x  = cc->item.crl;
            // X509_STORE_add_crl() increments the CRL reference count.
            X509_STORE_add_crl(store, x);
        }
    }

    ssl = SSL_new(context);
    if (!ssl) {
        SSL_CTX_free(context);
        context = nullptr;
        return false;
    }
    SSL_set_ssl_method(ssl, method); // can this return error?

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    if (!targetHostName.isEmpty()) {
        const QByteArray hostname = QUrl::toAce(targetHostName);
        if (SSL_set_tlsext_host_name(ssl, hostname.constData()) != 1) {
            SSL_free(ssl);
            ssl  = nullptr;
            rbio = nullptr;
            wbio = nullptr;
            SSL_CTX_free(context);
            context = nullptr;
            return false;
        }
    }
#endif

    rbio = makeReadBIO();
    wbio = makeWriteBIO();
    if (!rbio || !wbio) {
        BIO_free(rbio);
        BIO_free(wbio);
        rbio = nullptr;
        wbio = nullptr;
        SSL_free(ssl);
        ssl = nullptr;
        SSL_CTX_free(context);
        context = nullptr;
        return false;
    }

    // this passes control of the bios to ssl. we don't need to free them.
    SSL_set_bio(ssl, rbio, wbio);

    if (!applyCertificate()) {
        SSL_free(ssl);
        ssl  = nullptr;
        rbio = nullptr;
        wbio = nullptr;
        SSL_CTX_free(context);
        context = nullptr;
        return false;
    }

    // request a certificate from the client, if in server mode
    if (serv)
        SSL_set_verify(ssl, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, ssl_verify_callback);

    return true;
}

void BaseOsslTLSContext::getCert()
{
    // verify the certificate
    Validity code           = ErrorValidityUnknown;
    STACK_OF(X509) *x_chain = SSL_get_peer_cert_chain(ssl);
    // X509 *x = SSL_get_peer_certificate(ssl);
    if (x_chain) {
        CertificateChain chain;

        if (serv) {
            X509 *x = SSL_get_peer_certificate(ssl);
            if (!x) {
                vr       = code;
                peercert = Certificate();
                return;
            }
            MyCertContext *cc = new MyCertContext(provider());
            cc->fromX509(x);
            // SSL_get_peer_certificate() returns an owned reference. fromX509()
            // takes its own reference, so release the one returned by OpenSSL.
            X509_free(x);
            Certificate cert;
            cert.change(cc);
            chain += cert;
        }

        for (int n = 0; n < sk_X509_num(x_chain); ++n) {
            X509          *x  = sk_X509_value(x_chain, n);
            MyCertContext *cc = new MyCertContext(provider());
            cc->fromX509(x);
            Certificate cert;
            cert.change(cc);
            chain += cert;
        }

        peercert = chain.primary();

#ifdef Q_OS_MACOS
        code = chain.validate(trusted);
#else
        int ret = SSL_get_verify_result(ssl);
        if (ret == X509_V_OK)
            code = ValidityGood;
        else {
            QCA_logTextMessage(
                QStringLiteral("%1: getCert: SSL_get_verify_result failed: %2").arg(type(), QString::number(ret)),
                Logger::Warning);
            code = convert_verify_error(ret);
        }
#endif
    } else {
        peercert = Certificate();
    }
    vr = code;
}

QByteArray BaseOsslTLSContext::readOutgoing()
{
    QByteArray a;
    int        size = BIO_pending(wbio);
    if (size <= 0)
        return a;
    a.resize(size);

    int r = BIO_read(wbio, a.data(), size);
    if (r <= 0) {
        a.resize(0);
        return a;
    }
    if (r != size)
        a.resize(r);
    return a;
}

}
