/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#pragma once

#include <QByteArray>
#include <QLibrary>
#include <QList>
#include <QMutex>
#include <QString>
#include <QtEndian>
#include <climits>
#include <memory>

#include <windows.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>

namespace wingssQCAPlugin {

using MySecurityFunctionTableW  = SecurityFunctionTableW;
using PMySecurityFunctionTableW = PSecurityFunctionTableW;

//----------------------------------------------------------------------------
// SSPI helper API
//----------------------------------------------------------------------------

typedef void (*sspi_logger_func)(const QString &str);

class SspiPackage
{
public:
    QString name;
    quint32 caps;
    quint32 maxtok;
    quint16 version;
    quint16 rpcid;
    QString comment;
};

// logger can be set even when sspi is not loaded (this is needed so logging
//   during sspi_load/unload can be captured).  pass 0 to disable.
void sspi_set_logger(sspi_logger_func p);

void sspi_log(const QString &str);

bool sspi_load();
void sspi_unload();

// returns the available security packages.  only the first call actually
//   queries the sspi subsystem.  subsequent calls return a cached result.
QList<SspiPackage> sspi_get_packagelist();

// refresh the package list cache.  call sspi_get_packagelist afterwards to
//   get the new list.
void sspi_refresh_packagelist();

// helper functions for logging
QString SECURITY_STATUS_toString(SECURITY_STATUS i);
QString ptr_toString(const void *p);

//----------------------------------------------------------------------------
// SSPI helper implementation
//----------------------------------------------------------------------------

Q_GLOBAL_STATIC(QMutex, sspi_mutex)
Q_GLOBAL_STATIC(QMutex, sspi_logger_mutex)

union SecurityFunctionTableUnion {
    PMySecurityFunctionTableW W;
    PSecurityFunctionTableA   A;
    void                     *ptr;
};

static QLibrary                  *sspi_lib   = 0;
static unsigned                   sspi_users = 0;
static SecurityFunctionTableUnion sspi;
static sspi_logger_func           sspi_logger;
static QList<SspiPackage>        *sspi_packagelist = 0;

void sspi_log(const QString &str)
{
    QMutexLocker locker(sspi_logger_mutex());

    if (sspi_logger)
        sspi_logger(str);
}

void sspi_set_logger(sspi_logger_func p)
{
    QMutexLocker locker(sspi_logger_mutex());

    sspi_logger = p;
}

#define CASE_SS_STRING(s)                                                                                              \
    case s:                                                                                                            \
        return #s;

static const char *SECURITY_STATUS_lookup(SECURITY_STATUS i)
{
    switch (i) {
        CASE_SS_STRING(SEC_E_OK);
        CASE_SS_STRING(SEC_I_COMPLETE_AND_CONTINUE);
        CASE_SS_STRING(SEC_I_COMPLETE_NEEDED);
        CASE_SS_STRING(SEC_I_CONTINUE_NEEDED);
        CASE_SS_STRING(SEC_I_INCOMPLETE_CREDENTIALS);
        CASE_SS_STRING(SEC_E_UNSUPPORTED_FUNCTION);
        CASE_SS_STRING(SEC_E_INVALID_TOKEN);
        CASE_SS_STRING(SEC_E_MESSAGE_ALTERED);
        CASE_SS_STRING(SEC_E_INSUFFICIENT_MEMORY);
        CASE_SS_STRING(SEC_E_INTERNAL_ERROR);
        CASE_SS_STRING(SEC_E_INVALID_HANDLE);
        CASE_SS_STRING(SEC_E_LOGON_DENIED);
        CASE_SS_STRING(SEC_E_NO_AUTHENTICATING_AUTHORITY);
        CASE_SS_STRING(SEC_E_NO_CREDENTIALS);
        CASE_SS_STRING(SEC_E_TARGET_UNKNOWN);
        CASE_SS_STRING(SEC_E_WRONG_PRINCIPAL);
        CASE_SS_STRING(SEC_E_BUFFER_TOO_SMALL);
        CASE_SS_STRING(SEC_E_CONTEXT_EXPIRED);
        CASE_SS_STRING(SEC_E_CRYPTO_SYSTEM_INVALID);
        CASE_SS_STRING(SEC_E_QOP_NOT_SUPPORTED);
        CASE_SS_STRING(SEC_E_INCOMPLETE_MESSAGE);
        CASE_SS_STRING(SEC_E_OUT_OF_SEQUENCE);
    default:
        break;
    }
    return 0;
}

QString SECURITY_STATUS_toString(SECURITY_STATUS i)
{
    const char *str = SECURITY_STATUS_lookup(i);
    if (str)
        return QString::fromLatin1(str);
    else
        return QString::number(i);
}

QString ptr_toString(const void *p)
{
    return QString::asprintf("%p", p);
}

bool sspi_load()
{
    QMutexLocker locker(sspi_mutex());
    if (sspi_lib) {
        ++sspi_users;
        return true;
    }

    wchar_t    systemDirectory[MAX_PATH];
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    sspi_lib = new QLibrary(QString::fromWCharArray(systemDirectory, int(length)) + QStringLiteral("/secur32.dll"));
    if (!sspi_lib->load()) {
        delete sspi_lib;
        sspi_lib = 0;
        return false;
    }

    union {
        INIT_SECURITY_INTERFACE_W W;
        INIT_SECURITY_INTERFACE_A A;
        void                     *ptr;
    } pInitSecurityInterface;
    pInitSecurityInterface.ptr = 0;

    QString securityEntrypoint;
    securityEntrypoint       = QStringLiteral("InitSecurityInterfaceW");
    pInitSecurityInterface.W = (INIT_SECURITY_INTERFACE_W)(sspi_lib->resolve(securityEntrypoint.toLatin1().data()));
    if (!pInitSecurityInterface.ptr) {
        sspi_lib->unload();
        delete sspi_lib;
        sspi_lib = 0;
        return false;
    }

    union {
        PMySecurityFunctionTableW W;
        PSecurityFunctionTableA   A;
        void                     *ptr;
    } funcs;
    funcs.ptr = 0;

    funcs.W = (PMySecurityFunctionTableW)pInitSecurityInterface.W();

    sspi_log(QStringLiteral("%1() = %2\n").arg(securityEntrypoint, ptr_toString(funcs.ptr)));
    if (!funcs.ptr) {
        sspi_lib->unload();
        delete sspi_lib;
        sspi_lib = 0;
        return false;
    }

    sspi.W     = funcs.W;
    sspi_users = 1;

    return true;
}

void sspi_unload()
{
    QMutexLocker locker(sspi_mutex());

    if (!sspi_lib || --sspi_users != 0)
        return;
    sspi_lib->unload();
    delete sspi_lib;
    sspi_lib = 0;
    sspi.ptr = 0;
    delete sspi_packagelist;
    sspi_packagelist = nullptr;
}

static QList<SspiPackage> sspi_get_packagelist_direct()
{
    QList<SspiPackage> out;

    ULONG           cPackages;
    SecPkgInfoW    *pPackageInfo;
    SECURITY_STATUS ret = sspi.W->EnumerateSecurityPackagesW(&cPackages, &pPackageInfo);
    sspi_log(QStringLiteral("EnumerateSecurityPackages() = %1\n").arg(SECURITY_STATUS_toString(ret)));
    if (ret != SEC_E_OK)
        return out;

    for (int n = 0; n < (int)cPackages; ++n) {
        SecPkgInfoW *p = &pPackageInfo[n];
        SspiPackage  i;
        i.name    = QString::fromWCharArray(p->Name);
        i.caps    = p->fCapabilities;
        i.version = p->wVersion;
        i.rpcid   = p->wRPCID;
        i.maxtok  = p->cbMaxToken;
        i.comment = QString::fromWCharArray(p->Comment);
        out += i;
    }

    ret = sspi.W->FreeContextBuffer(pPackageInfo);
    sspi_log(QStringLiteral("FreeContextBuffer() = %1\n").arg(SECURITY_STATUS_toString(ret)));

    return out;
}

static void sspi_refresh_packagelist_internal()
{
    if (sspi_packagelist)
        *sspi_packagelist = sspi_get_packagelist_direct();
    else
        sspi_packagelist = new QList<SspiPackage>(sspi_get_packagelist_direct());
}

QList<SspiPackage> sspi_get_packagelist()
{
    QMutexLocker locker(sspi_mutex());

    if (!sspi_packagelist)
        sspi_refresh_packagelist_internal();
    return *sspi_packagelist;
}

void sspi_refresh_packagelist()
{
    QMutexLocker locker(sspi_mutex());

    sspi_refresh_packagelist_internal();
}

template<typename T> inline T cap_to_int(const T &t)
{
    if (sizeof(int) <= sizeof(T))
        return (int)((t > INT_MAX) ? INT_MAX : t);
    else
        return (int)t;
}

//----------------------------------------------------------------------------
// KerberosSession
//----------------------------------------------------------------------------
// this class thinly wraps SSPI to perform kerberos.
class KerberosSession
{
public:
    enum ReturnCode
    {
        Success,
        NeedMoreData, // for decrypt
        ErrorInvalidSystem,
        ErrorKerberosNotFound,
        ErrorAcquireCredentials,
        ErrorInitialize,
        ErrorQueryContext,
        ErrorEncrypt,
        ErrorDecrypt
    };

    SECURITY_STATUS lastErrorCode;

    quint32 maxtok;

    bool       initialized;
    bool       first_step;
    QByteArray first_out_token;
    bool       authed;

    QString spn;

    CredHandle user_cred;
    TimeStamp  user_cred_expiry;

    CtxtHandle                ctx;
    ULONG                     ctx_attr_req;
    ULONG                     ctx_attr;
    bool                      have_sizes;
    SecPkgContext_Sizes       ctx_sizes;
    SecPkgContext_StreamSizes ctx_streamSizes;

    KerberosSession()
        : lastErrorCode(SEC_E_OK)
        , maxtok(0)
        , initialized(false)
        , first_step(false)
        , authed(false)
        , ctx_attr_req(0)
        , ctx_attr(0)
        , have_sizes(false)
        , ctx_sizes {}
        , ctx_streamSizes {}
    {
        SecInvalidateHandle(&ctx);
        SecInvalidateHandle(&user_cred);
    }

    ~KerberosSession()
    {
        clear();
    }

    void clear()
    {
        if (SecIsValidHandle(&ctx)) {
            SECURITY_STATUS ret = sspi.W->DeleteSecurityContext(&ctx);
            sspi_log(QStringLiteral("DeleteSecurityContext() = %1\n").arg(SECURITY_STATUS_toString(ret)));
            SecInvalidateHandle(&ctx);
        }
        if (SecIsValidHandle(&user_cred)) {
            SECURITY_STATUS ret = sspi.W->FreeCredentialsHandle(&user_cred);
            sspi_log(QStringLiteral("FreeCredentialsHandle() = %1\n").arg(SECURITY_STATUS_toString(ret)));
            SecInvalidateHandle(&user_cred);
        }
        initialized = false;
        have_sizes  = false;
        first_out_token.clear();
    }

    ReturnCode init(const QString &_spn)
    {
        clear();
        // ensure kerberos is available
        bool               found    = false;
        quint32            _maxtok  = 0;
        QList<SspiPackage> packages = sspi_get_packagelist();
        sspi_log(QStringLiteral("SSPI packages:\n"));
        foreach (const SspiPackage &p, packages) {
            bool gss = false;
            if (p.caps & SECPKG_FLAG_GSS_COMPATIBLE)
                gss = true;

            if (p.name == QStringLiteral("Kerberos") && gss) {
                found   = true;
                _maxtok = p.maxtok;
            }

            QString gssstr = gss ? QStringLiteral("yes") : QStringLiteral("no");
            sspi_log(QStringLiteral("  %1 (gss=%2, maxtok=%3)\n").arg(p.name, gssstr, QString::number(p.maxtok)));
        }

        if (!found)
            return ErrorKerberosNotFound;

        // get the logged-in user's credentials
        SECURITY_STATUS ret = sspi.W->AcquireCredentialsHandleW((SEC_WCHAR *)0, // we want creds of logged-in user
                                                                (SEC_WCHAR *)QStringLiteral("Kerberos").utf16(),
                                                                SECPKG_CRED_OUTBOUND,
                                                                0, // don't need a LUID
                                                                0, // default credentials for kerberos
                                                                0, // not used
                                                                0, // not used
                                                                &user_cred,
                                                                &user_cred_expiry);
        sspi_log(QStringLiteral("AcquireCredentialsHandle() = %1\n").arg(SECURITY_STATUS_toString(ret)));
        if (ret != SEC_E_OK) {
            lastErrorCode = ret;
            return ErrorAcquireCredentials;
        }

        maxtok = _maxtok;
        authed = false;
        spn    = _spn;

        SecBuffer outbuf;
        outbuf.BufferType = SECBUFFER_TOKEN;
        outbuf.cbBuffer   = 0;
        outbuf.pvBuffer   = NULL;

        SecBufferDesc outbufdesc;
        outbufdesc.ulVersion = SECBUFFER_VERSION;
        outbufdesc.cBuffers  = 1;
        outbufdesc.pBuffers  = &outbuf;

        ctx_attr_req = 0;

        // not strictly required, but some SSPI calls seem to always
        //   allocate memory, so for consistency we'll explicity
        //   request to have it that way all the time
        ctx_attr_req |= ISC_REQ_ALLOCATE_MEMORY;

        // required by SASL GSSAPI RFC
        ctx_attr_req |= ISC_REQ_INTEGRITY;

        // required for security layer
        ctx_attr_req |= ISC_REQ_MUTUAL_AUTH;
        ctx_attr_req |= ISC_REQ_SEQUENCE_DETECT;

        // required for encryption
        ctx_attr_req |= ISC_REQ_CONFIDENTIALITY;

        // other options that may be of use, but we currently aren't
        //   using:
        // ISC_REQ_DELEGATE
        // ISC_REQ_REPLAY_DETECT

        ret = sspi.W->InitializeSecurityContextW(&user_cred,
                                                 0, // NULL for the first call
                                                 (SEC_WCHAR *)spn.utf16(),
                                                 ctx_attr_req,
                                                 0,
                                                 SECURITY_NETWORK_DREP,
                                                 0, // NULL for first call
                                                 0,
                                                 &ctx,
                                                 &outbufdesc,
                                                 &ctx_attr,
                                                 0); // don't care about expiration
        sspi_log(QStringLiteral("InitializeSecurityContext(*, 0, ...) = %1\n").arg(SECURITY_STATUS_toString(ret)));
        const std::unique_ptr<void, FREE_CONTEXT_BUFFER_FN> output(outbuf.pvBuffer, sspi.W->FreeContextBuffer);
        if (ret == SEC_E_OK || ret == SEC_I_CONTINUE_NEEDED) {
            if (outbuf.pvBuffer) {
                if (outbuf.cbBuffer > INT_MAX) {
                    clear();
                    return ErrorInitialize;
                }
                first_out_token.resize(outbuf.cbBuffer);
                memcpy(first_out_token.data(), outbuf.pvBuffer, outbuf.cbBuffer);
            }

            if (ret == SEC_E_OK)
                authed = true;
        } else {
            // ret was an error, or some unexpected value like
            //   SEC_I_COMPLETE_NEEDED or
            //   SEC_I_COMPLETE_AND_CONTINUE, which i believe are
            //   not used for kerberos

            lastErrorCode = ret;

            clear();
            return ErrorInitialize;
        }

        initialized = true;
        first_step  = true;

        return Success;
    }

    ReturnCode step(const QByteArray &in, QByteArray *out, bool *authenticated)
    {
        out->clear();
        if (!initialized)
            return ErrorInitialize;
        if (quint64(in.size()) > maxtok)
            return ErrorInitialize;
        if (authed && !first_step) {
            out->clear();
            *authenticated = true;
            return Success;
        }

        if (first_step) {
            // ignore 'in'

            *out = first_out_token;
            first_out_token.clear();

            first_step = false;
        } else {
            SecBuffer outbuf;
            outbuf.BufferType = SECBUFFER_TOKEN;
            outbuf.cbBuffer   = 0;
            outbuf.pvBuffer   = NULL;

            SecBufferDesc outbufdesc;
            outbufdesc.ulVersion = SECBUFFER_VERSION;
            outbufdesc.cBuffers  = 1;
            outbufdesc.pBuffers  = &outbuf;

            SecBuffer inbuf;
            inbuf.BufferType = SECBUFFER_TOKEN;
            inbuf.cbBuffer   = in.size();
            inbuf.pvBuffer   = (void *)in.data();

            SecBufferDesc inbufdesc;
            inbufdesc.ulVersion = SECBUFFER_VERSION;
            inbufdesc.cBuffers  = 1;
            inbufdesc.pBuffers  = &inbuf;

            SECURITY_STATUS ret = sspi.W->InitializeSecurityContextW(&user_cred,
                                                                     &ctx,
                                                                     (SEC_WCHAR *)spn.utf16(),
                                                                     ctx_attr_req,
                                                                     0,
                                                                     SECURITY_NETWORK_DREP,
                                                                     &inbufdesc,
                                                                     0,
                                                                     &ctx,
                                                                     &outbufdesc,
                                                                     &ctx_attr,
                                                                     0); // don't care about expiration
            sspi_log(
                QStringLiteral("InitializeSecurityContext(*, ctx, ...) = %1\n").arg(SECURITY_STATUS_toString(ret)));
            const std::unique_ptr<void, FREE_CONTEXT_BUFFER_FN> output(outbuf.pvBuffer, sspi.W->FreeContextBuffer);
            if (ret == SEC_E_OK || ret == SEC_I_CONTINUE_NEEDED) {
                if (outbuf.pvBuffer) {
                    if (outbuf.cbBuffer > INT_MAX) {
                        clear();
                        return ErrorInitialize;
                    }
                    out->resize(outbuf.cbBuffer);
                    memcpy(out->data(), outbuf.pvBuffer, outbuf.cbBuffer);
                } else
                    out->clear();

                if (ret == SEC_E_OK)
                    authed = true;
            } else {
                // ret was an error, or some unexpected value like
                //   SEC_I_COMPLETE_NEEDED or
                //   SEC_I_COMPLETE_AND_CONTINUE, which i believe are
                //   not used for kerberos

                lastErrorCode = ret;

                clear();
                return ErrorInitialize;
            }
        }

        *authenticated = authed;
        return Success;
    }

    // private
    bool ensure_sizes_cached()
    {
        if (!initialized)
            return false;
        if (!have_sizes) {
            SECURITY_STATUS ret = sspi.W->QueryContextAttributesW(&ctx, SECPKG_ATTR_SIZES, &ctx_sizes);
            sspi_log(QStringLiteral("QueryContextAttributes(ctx, SECPKG_ATTR_SIZES, ...) = %1\n")
                         .arg(SECURITY_STATUS_toString(ret)));
            if (ret != SEC_E_OK) {
                lastErrorCode = ret;
                return false;
            }

            // for some reason, querying the stream sizes returns
            //   SEC_E_UNSUPPORTED_FUNCTION on my system, even
            //   though the docs say it should work and putty
            //   wingss also calls it.

            // all we really need is cbMaximumMessage, and since
            //   we can't query for it, we'll hard code some
            //   value.  according to putty wingss, the max size
            //   is essentially unbounded anyway, so this should
            //   be safe to do.
            ctx_streamSizes.cbMaximumMessage = 8192;

            // ret = sspi.W->QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &ctx_streamSizes);
            // sspi_log(QStringLiteral("QueryContextAttributes(ctx, SECPKG_ATTR_STREAM_SIZES, ...) =
            // %1\n").arg(SECURITY_STATUS_toString(ret))); if(ret != SEC_E_OK)
            //{
            //    lastErrorCode = ret;
            //    return ErrorQueryContext;
            //}

            have_sizes = true;
        }

        return true;
    }

    ReturnCode get_max_encrypt_size(int *max, int maxTokenSize)
    {
        if (!ensure_sizes_cached())
            return ErrorQueryContext;
        const quint64 overhead = quint64(ctx_sizes.cbSecurityTrailer) + ctx_sizes.cbBlockSize;
        if (maxTokenSize <= 0 || overhead >= quint64(maxTokenSize))
            return ErrorQueryContext;
        *max = qMin(int(ctx_streamSizes.cbMaximumMessage), int(quint64(maxTokenSize) - overhead));

        return Success;
    }

    ReturnCode encode(const QByteArray &in, QByteArray *out, bool encrypt)
    {
        out->clear();
        if (quint64(in.size()) > INT_MAX)
            return ErrorEncrypt;
        if (!ensure_sizes_cached())
            return ErrorQueryContext;
        if (quint64(ctx_sizes.cbSecurityTrailer) + ctx_sizes.cbBlockSize + quint64(in.size()) > INT_MAX)
            return ErrorEncrypt;

        QByteArray tokenbuf(ctx_sizes.cbSecurityTrailer, 0);
        QByteArray padbuf(ctx_sizes.cbBlockSize, 0);

        // we assume here, like putty wingss, that the output size is
        //   less than or equal to the input size.  honestly I don't
        //   see how this is clear from the SSPI documentation, but
        //   the code seems to work so we'll go with it...
        QByteArray databuf = in;

        SecBuffer buf[3];
        buf[0].BufferType = SECBUFFER_TOKEN;
        buf[0].cbBuffer   = tokenbuf.size();
        buf[0].pvBuffer   = tokenbuf.data();
        buf[1].BufferType = SECBUFFER_DATA;
        buf[1].cbBuffer   = databuf.size();
        buf[1].pvBuffer   = databuf.data();
        buf[2].BufferType = SECBUFFER_PADDING;
        buf[2].cbBuffer   = padbuf.size();
        buf[2].pvBuffer   = padbuf.data();

        SecBufferDesc bufdesc;
        bufdesc.ulVersion = SECBUFFER_VERSION;
        bufdesc.cBuffers  = 3;
        bufdesc.pBuffers  = buf;

        SECURITY_STATUS ret = sspi.W->EncryptMessage(&ctx, encrypt ? 0 : SECQOP_WRAP_NO_ENCRYPT, &bufdesc, 0);
        sspi_log(QStringLiteral("EncryptMessage() = %1\n").arg(SECURITY_STATUS_toString(ret)));
        if (ret != SEC_E_OK) {
            lastErrorCode = ret;
            return ErrorEncrypt;
        }

        QByteArray fullbuf;
        for (int i = 0; i < (int)bufdesc.cBuffers; ++i)
            fullbuf += QByteArray((const char *)bufdesc.pBuffers[i].pvBuffer, bufdesc.pBuffers[i].cbBuffer);

        *out = fullbuf;
        return Success;
    }

    ReturnCode decode(const QByteArray &in, QByteArray *out, bool *encrypted)
    {
        out->clear();
        if (quint64(in.size()) > INT_MAX)
            return ErrorDecrypt;
        // DecryptMessage modifies the stream buffer and returns a view into it.
        QByteArray storage = in;
        SecBuffer  buf[2];
        buf[0].BufferType = SECBUFFER_DATA;
        buf[0].cbBuffer   = 0;
        buf[0].pvBuffer   = NULL;
        buf[1].BufferType = SECBUFFER_STREAM;
        buf[1].cbBuffer   = in.size();
        buf[1].pvBuffer   = storage.data();

        SecBufferDesc bufdesc;
        bufdesc.ulVersion = SECBUFFER_VERSION;
        bufdesc.cBuffers  = 2;
        bufdesc.pBuffers  = buf;

        ULONG           fQOP;
        SECURITY_STATUS ret = sspi.W->DecryptMessage(&ctx, &bufdesc, 0, &fQOP);
        sspi_log(QStringLiteral("DecryptMessage() = %1\n").arg(SECURITY_STATUS_toString(ret)));
        if (ret == SEC_E_INCOMPLETE_MESSAGE) {
            return NeedMoreData;
        } else if (ret != SEC_E_OK) {
            lastErrorCode = ret;
            return ErrorDecrypt;
        }

        if (buf[0].cbBuffer) {
            const quintptr base = reinterpret_cast<quintptr>(storage.constData());
            const quintptr data = reinterpret_cast<quintptr>(buf[0].pvBuffer);
            if (data < base || data - base > quintptr(storage.size()) ||
                buf[0].cbBuffer > quintptr(storage.size()) - (data - base))
                return ErrorDecrypt;
            *out = QByteArray(static_cast<const char *>(buf[0].pvBuffer), int(buf[0].cbBuffer));
        }

        if (fQOP & SECQOP_WRAP_NO_ENCRYPT)
            *encrypted = false;
        else
            *encrypted = true;

        return Success;
    }
};

//----------------------------------------------------------------------------
// SaslGssapiSession
//----------------------------------------------------------------------------
// this class wraps KerberosSession to perform SASL GSSAPI.  it hides away
//   any SSPI details, and is thus very simple to use.
class SaslGssapiSession
{
private:
    int             secflags;
    KerberosSession sess;
    int             mode; // 0 = kerberos tokens, 1 = app packets
    bool            authed;
    QByteArray      inbuf;

    int max_enc_size; // most we can encrypt to them
    int max_dec_size; // most we are expected to decrypt from them

public:
    enum SecurityFlags
    {
        // only one of these should be set
        RequireAtLeastInt = 0x0001,
        RequireConf       = 0x0002,
        DisableConf       = 0x0004,
        DisableInt        = 0x0008,
        RequireMutualAuth = 0x0010
    };

    enum ReturnCode
    {
        Success,
        ErrorInit,
        ErrorKerberosStep,
        ErrorAppTokenDecode,
        ErrorAppTokenIsEncrypted,
        ErrorAppTokenWrongSize,
        ErrorAppTokenInvalid,
        ErrorAppTokenEncode,
        ErrorLayerTooWeak,
        ErrorEncode,
        ErrorDecode,
        ErrorDecodeTooLarge,
        ErrorDecodeNotEncrypted
    };

    // set this before auth, if you want
    QString authzid;

    // read-only
    bool do_layer, do_conf;

    SaslGssapiSession()
        : secflags(0)
        , mode(0)
        , authed(false)
        , max_enc_size(0)
        , max_dec_size(0)
        , do_layer(false)
        , do_conf(false)
    {
    }

    ReturnCode init(const QString &proto, const QString &fqdn, int _secflags)
    {
        secflags = _secflags;
        mode     = 0; // kerberos tokens
        authed   = false;

        do_layer = false;
        do_conf  = false;
        inbuf.clear();

        if (sess.init(proto + QLatin1Char('/') + fqdn) != KerberosSession::Success)
            return ErrorInit;

        return Success;
    }

    ReturnCode step(const QByteArray &in, QByteArray *out, bool *authenticated)
    {
        if (authed) {
            out->clear();
            *authenticated = true;
            return Success;
        }

        if (mode == 0) // kerberos tokens
        {
            bool kerb_authed;
            if (sess.step(in, out, &kerb_authed) != KerberosSession::Success)
                return ErrorKerberosStep;

            if (kerb_authed)
                mode = 1; // switch to app packets

            *authenticated = false;
        } else if (mode == 1) {
            if ((secflags & RequireMutualAuth) && !(sess.ctx_attr & ISC_RET_MUTUAL_AUTH))
                return ErrorLayerTooWeak;
            bool layerPossible      = false;
            bool encryptionPossible = false;
            if (sess.ctx_attr & ISC_RET_INTEGRITY && sess.ctx_attr & ISC_RET_MUTUAL_AUTH &&
                sess.ctx_attr & ISC_RET_SEQUENCE_DETECT) {
                layerPossible = true;

                if (sess.ctx_attr & ISC_RET_CONFIDENTIALITY)
                    encryptionPossible = true;
            }

            if (layerPossible) {
                if (encryptionPossible)
                    sspi_log(QStringLiteral("Kerberos application data protection supported (with encryption)\n"));
                else
                    sspi_log(QStringLiteral("Kerberos application data protection supported (without encryption)\n"));
            } else
                sspi_log(QStringLiteral("No Kerberos application data protection supported\n"));

            QByteArray decbuf;
            bool       encrypted;
            if (sess.decode(in, &decbuf, &encrypted) != KerberosSession::Success) {
                sspi_log(QStringLiteral("Error decoding application token\n"));
                return ErrorAppTokenDecode;
            }

            // this packet is supposed to be not encrypted
            if (encrypted) {
                sspi_log(QStringLiteral("Error, application token is encrypted\n"));
                return ErrorAppTokenIsEncrypted;
            }

            // packet must be exactly 4 bytes
            if (decbuf.size() != 4) {
                sspi_log(QStringLiteral("Error, application token is the wrong size\n"));
                return ErrorAppTokenWrongSize;
            }

            sspi_log(QStringLiteral("Received application token: [%1]\n").arg(QString::fromLatin1(decbuf.toHex())));

            unsigned char layermask = decbuf[0];
            quint32       maxsize   = 0;
            maxsize += (unsigned char)decbuf[1];
            maxsize <<= 8;
            maxsize += (unsigned char)decbuf[2];
            maxsize <<= 8;
            maxsize += (unsigned char)decbuf[3];

            // if 'None' is all that is supported, then maxsize
            //   must be zero
            if (layermask == 1 && maxsize > 0) {
                sspi_log(QStringLiteral("Error, supports no security layer but the max buffer size is not zero\n"));
                return ErrorAppTokenInvalid;
            }

            // convert maxsize to a signed int, by capping it
            int _max_enc_size = cap_to_int<quint32>(maxsize);

            // parse out layermask
            bool        saslLayerNone = false;
            bool        saslLayerInt  = false;
            bool        saslLayerConf = false;
            QStringList saslLayerModes;
            if (layermask & 1) {
                saslLayerNone = true;
                saslLayerModes += QStringLiteral("None");
            }
            if (layermask & 2) {
                saslLayerInt = true;
                saslLayerModes += QStringLiteral("Int");
            }
            if (layermask & 4) {
                saslLayerConf = true;
                saslLayerModes += QStringLiteral("Conf");
            }

            sspi_log(
                QStringLiteral("Security layer modes supported: %1\n").arg(saslLayerModes.join(QStringLiteral(", "))));
            sspi_log(QStringLiteral("Security layer max packet size: %1\n").arg(maxsize));

            // create outbound application token
            QByteArray obuf(4, 0); // initially 4 bytes

            // set one of use_conf or use_int, but not both
            bool use_conf = false;
            bool use_int  = false;
            if (encryptionPossible && saslLayerConf && !(secflags & DisableConf))
                use_conf = true;
            else if (layerPossible && saslLayerInt && !(secflags & DisableInt))
                use_int = true;
            else if (!saslLayerNone) {
                sspi_log(QStringLiteral("Error, no compatible layer mode supported, not even 'None'\n"));
                return ErrorLayerTooWeak;
            }

            if ((secflags & RequireConf) && !use_conf) {
                sspi_log(QStringLiteral("Error, 'Conf' required but not supported\n"));
                return ErrorLayerTooWeak;
            }

            if ((secflags & RequireAtLeastInt) && !use_conf && !use_int) {
                sspi_log(QStringLiteral("Error, 'Conf' or 'Int' required but not supported\n"));
                return ErrorLayerTooWeak;
            }
            if (use_conf || use_int) {
                int maximum = 0;
                if (sess.get_max_encrypt_size(&maximum, _max_enc_size) != KerberosSession::Success || maximum <= 0)
                    return ErrorAppTokenInvalid;
            }

            if (use_conf) {
                sspi_log(QStringLiteral("Using 'Conf' layer\n"));
                obuf[0] = 4;
            } else if (use_int) {
                sspi_log(QStringLiteral("Using 'Int' layer\n"));
                obuf[0] = 2;
            } else {
                sspi_log(QStringLiteral("Using 'None' layer\n"));
                obuf[0] = 1;
            }

            // as far as i can tell, there is no max decrypt size
            //   with sspi.  so we'll just pick some number.
            //   a small one is good, to prevent excessive input
            //   buffering.
            // in other parts of the code, it is assumed this
            //   value is less than INT_MAX
            int _max_dec_size = 8192; // same as cyrus

            // max size must be zero if no security layer is used
            if (!use_conf && !use_int)
                _max_dec_size = 0;

            obuf[1] = (unsigned char)((_max_dec_size >> 16) & 0xff);
            obuf[2] = (unsigned char)((_max_dec_size >> 8) & 0xff);
            obuf[3] = (unsigned char)((_max_dec_size) & 0xff);

            if (!authzid.isEmpty())
                obuf += authzid.toUtf8();

            sspi_log(QStringLiteral("Sending application token (%1 bytes)\n").arg(obuf.size()));

            if (sess.encode(obuf, out, false) != KerberosSession::Success) {
                sspi_log(QStringLiteral("Error encoding application token\n"));
                return ErrorAppTokenEncode;
            }

            if (use_conf || use_int)
                do_layer = true;
            if (use_conf)
                do_conf = true;

            max_enc_size = _max_enc_size;
            max_dec_size = _max_dec_size;

            authed         = true;
            *authenticated = true;
        }

        return Success;
    }

    ReturnCode encode(const QByteArray &in, QByteArray *out)
    {
        out->clear();
        if (!authed)
            return ErrorEncode;
        if (!do_layer) {
            *out = in;
            return Success;
        }

        int local_encrypt_max;
        if (sess.get_max_encrypt_size(&local_encrypt_max, max_enc_size) != KerberosSession::Success)
            return ErrorEncode;

        // send no more per-packet than what our local system will
        //   encrypt AND no more than what the peer will accept.
        int chunk_max = qMin(local_encrypt_max, max_enc_size);
        if (chunk_max <= 0) {
            sspi_log(QStringLiteral("Error, chunk_max is ridiculously small\n"));
            return ErrorEncode;
        }

        QByteArray total_out;

        // break up into packets, if input exceeds max size
        qint64 encoded = 0;
        while (encoded < in.size()) {
            int        chunk_size = int(qMin<qint64>(in.size() - encoded, chunk_max));
            QByteArray kerb_in    = QByteArray::fromRawData(in.data() + encoded, chunk_size);
            QByteArray kerb_out;
            if (sess.encode(kerb_in, &kerb_out, do_conf) != KerberosSession::Success)
                return ErrorEncode;

            // SASL (not GSS!) uses a 4 byte length prefix
            quint32 len = kerb_out.size();
            if (len > quint32(max_enc_size))
                return ErrorEncode;
            char header[4];
            qToBigEndian(len, header);

            encoded += kerb_in.size();
            total_out.append(header, 4);
            total_out += kerb_out;
        }

        *out = total_out;
        return Success;
    }

    ReturnCode decode(const QByteArray &in, QByteArray *out)
    {
        out->clear();
        if (!authed)
            return ErrorDecode;
        if (!do_layer) {
            *out = in;
            return Success;
        }

        QByteArray total_out;
        qint64     offset = 0;
        while (offset < in.size()) {
            // Retain at most one advertised frame, including fragmented headers.
            if (inbuf.size() < 4) {
                const int take = int(qMin<qint64>(4 - inbuf.size(), in.size() - offset));
                inbuf.append(in.constData() + offset, take);
                offset += take;
                if (inbuf.size() < 4)
                    break;
            }
            const quint32 length = qFromBigEndian<quint32>(inbuf.constData());
            if (length == 0 || length > quint32(max_dec_size)) {
                inbuf.clear();
                authed = false;
                return ErrorDecodeTooLarge;
            }
            const int needed = int(length) + 4 - int(inbuf.size());
            const int take   = int(qMin<qint64>(needed, in.size() - offset));
            inbuf.append(in.constData() + offset, take);
            offset += take;
            if (inbuf.size() < int(length) + 4)
                break;

            const QByteArray token = QByteArray::fromRawData(inbuf.constData() + 4, int(length));
            QByteArray       plaintext;
            bool             encrypted = false;
            const auto       result    = sess.decode(token, &plaintext, &encrypted);
            inbuf.clear();
            if (result != KerberosSession::Success) {
                authed = false;
                return ErrorDecode;
            }
            if (do_conf && !encrypted) {
                authed = false;
                return ErrorDecodeNotEncrypted;
            }
            total_out += plaintext;
        }
        *out = total_out;
        return Success;
    }
};

}
