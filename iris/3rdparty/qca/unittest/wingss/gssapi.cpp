// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gssapi_p.h"
#include <QSet>
#include <QTest>
#include <cstdlib>
#include <cstring>

using namespace wingssQCAPlugin;

namespace {
struct FakeSspi
{
    SecurityFunctionTableW table {};
    QSet<void *>           allocations;
    int                    invalidFrees     = 0;
    int                    contextsFreed    = 0;
    int                    credentialsFreed = 0;
    int                    initializeCalls  = 0;
    int                    decryptCalls     = 0;
    int                    largestToken     = 0;
    bool                   failInitialize   = false;
    bool                   firstComplete    = false;
    bool                   mutateStream     = false;
    bool                   encrypted        = false;
    ULONG attributes = ISC_RET_INTEGRITY | ISC_RET_MUTUAL_AUTH | ISC_RET_SEQUENCE_DETECT | ISC_RET_CONFIDENTIALITY;
} fake;

void *allocate(size_t size)
{
    void *value = std::malloc(size);
    fake.allocations.insert(value);
    return value;
}

SECURITY_STATUS SEC_ENTRY releaseBuffer(void *buffer)
{
    if (!fake.allocations.remove(buffer)) {
        ++fake.invalidFrees;
        return SEC_E_INVALID_TOKEN;
    }
    std::free(buffer);
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY enumerate(ULONG *count, PSecPkgInfoW *packages)
{
    *count                     = 1;
    *packages                  = static_cast<PSecPkgInfoW>(allocate(sizeof(SecPkgInfoW)));
    **packages                 = {};
    (*packages)->Name          = const_cast<wchar_t *>(L"Kerberos");
    (*packages)->Comment       = const_cast<wchar_t *>(L"Test package");
    (*packages)->fCapabilities = SECPKG_FLAG_GSS_COMPATIBLE;
    (*packages)->cbMaxToken    = 65536;
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY
acquire(SEC_WCHAR *, SEC_WCHAR *, ULONG, void *, void *, SEC_GET_KEY_FN, void *, PCredHandle credential, PTimeStamp)
{
    credential->dwLower = 1;
    credential->dwUpper = 0;
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY initialize(PCredHandle,
                                     PCtxtHandle,
                                     SEC_WCHAR *,
                                     ULONG,
                                     ULONG,
                                     ULONG,
                                     PSecBufferDesc,
                                     ULONG,
                                     PCtxtHandle    context,
                                     PSecBufferDesc output,
                                     PULONG         attributes,
                                     PTimeStamp)
{
    context->dwLower = 2;
    context->dwUpper = 0;
    *attributes      = fake.attributes;
    auto &buffer     = output->pBuffers[0];
    buffer.pvBuffer  = allocate(4);
    buffer.cbBuffer  = 4;
    std::memcpy(buffer.pvBuffer, "auth", 4);
    ++fake.initializeCalls;
    if (fake.failInitialize)
        return SEC_E_INVALID_TOKEN;
    return fake.firstComplete || fake.initializeCalls % 2 == 0 ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
}

SECURITY_STATUS SEC_ENTRY deleteContext(PCtxtHandle)
{
    ++fake.contextsFreed;
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY freeCredentials(PCredHandle)
{
    ++fake.credentialsFreed;
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY query(PCtxtHandle, ULONG attribute, void *result)
{
    if (attribute != SECPKG_ATTR_SIZES)
        return SEC_E_UNSUPPORTED_FUNCTION;
    *static_cast<SecPkgContext_Sizes *>(result) = {65536, 0, 1, 3};
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY encrypt(PCtxtHandle, ULONG, PSecBufferDesc message, ULONG)
{
    std::memcpy(message->pBuffers[0].pvBuffer, "hdr", 3);
    message->pBuffers[0].cbBuffer = 3;
    message->pBuffers[2].cbBuffer = 0;
    return SEC_E_OK;
}

SECURITY_STATUS SEC_ENTRY decrypt(PCtxtHandle, PSecBufferDesc message, ULONG, PULONG quality)
{
    ++fake.decryptCalls;
    auto &stream      = message->pBuffers[1];
    auto &data        = message->pBuffers[0];
    fake.largestToken = qMax(fake.largestToken, int(stream.cbBuffer));
    char     *bytes   = static_cast<char *>(stream.pvBuffer);
    const int prefix  = stream.cbBuffer >= 3 && std::memcmp(bytes, "hdr", 3) == 0 ? 3 : 0;
    data.pvBuffer     = bytes + prefix;
    data.cbBuffer     = stream.cbBuffer - prefix;
    if (fake.mutateStream && data.cbBuffer)
        static_cast<char *>(data.pvBuffer)[0] = 'A';
    *quality = fake.encrypted ? 0 : SECQOP_WRAP_NO_ENCRYPT;
    return SEC_E_OK;
}

bool authenticate(SaslGssapiSession &session,
                  int                flags  = SaslGssapiSession::RequireConf,
                  quint32            limit  = 8192,
                  char               layers = 7)
{
    if (session.init(QStringLiteral("service"), QStringLiteral("host.test"), flags) != SaslGssapiSession::Success)
        return false;
    QByteArray output;
    bool       authenticated = false;
    if (session.step({}, &output, &authenticated) != SaslGssapiSession::Success || output != "auth")
        return false;
    if (session.step("server", &output, &authenticated) != SaslGssapiSession::Success)
        return false;
    QByteArray challenge(4, 0);
    challenge[0] = layers;
    challenge[1] = char(limit >> 16);
    challenge[2] = char(limit >> 8);
    challenge[3] = char(limit);
    if (session.step(challenge, &output, &authenticated) != SaslGssapiSession::Success || !authenticated)
        return false;
    fake.encrypted = true;
    return true;
}
}

class WinGssSessionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void init()
    {
        fake                                  = {};
        fake.table.EnumerateSecurityPackagesW = enumerate;
        fake.table.AcquireCredentialsHandleW  = acquire;
        fake.table.InitializeSecurityContextW = initialize;
        fake.table.FreeContextBuffer          = releaseBuffer;
        fake.table.FreeCredentialsHandle      = freeCredentials;
        fake.table.DeleteSecurityContext      = deleteContext;
        fake.table.QueryContextAttributesW    = query;
        fake.table.EncryptMessage             = encrypt;
        fake.table.DecryptMessage             = decrypt;
        sspi.W                                = &fake.table;
    }

    void cleanup()
    {
        delete sspi_packagelist;
        sspi_packagelist = nullptr;
        sspi.ptr         = nullptr;
        QCOMPARE(fake.invalidFrees, 0);
        QCOMPARE(fake.allocations.size(), 0);
    }

    void buffersAndHandles()
    {
        {
            KerberosSession session;
            QCOMPARE(session.init(QStringLiteral("service/host.test")), KerberosSession::Success);
            static const char bytes[] = "abc";
            const QByteArray  input   = QByteArray::fromRawData(bytes, 3);
            fake.mutateStream         = true;
            QByteArray output;
            bool       encrypted = false;
            QCOMPARE(session.decode(input, &output, &encrypted), KerberosSession::Success);
            QCOMPARE(input, QByteArray("abc"));
            QCOMPARE(output, QByteArray("Abc"));
        }
        QCOMPARE(fake.contextsFreed, 1);
        QCOMPARE(fake.credentialsFreed, 1);
    }

    void nativeLibraryAndPackageList()
    {
        struct ReleaseLibrary
        {
            ~ReleaseLibrary()
            {
                while (sspi_users)
                    sspi_unload();
            }
        } release;
        QVERIFY(sspi_load());
        const auto table = sspi.W;
        QVERIFY(sspi_load());
        QCOMPARE(sspi.W, table);
        const auto packages = sspi_get_packagelist();
        bool       kerberos = false;
        for (const auto &package : packages)
            kerberos |= package.name == QStringLiteral("Kerberos");
        QVERIFY(kerberos);
        sspi_unload();
        QVERIFY(sspi_lib);
        sspi_unload();
        QVERIFY(!sspi_lib);
        QVERIFY(!sspi_packagelist);
    }

    void failureReleasesOutputAndHandles()
    {
        fake.failInitialize = true;
        {
            KerberosSession session;
            QCOMPARE(session.init(QStringLiteral("service/host.test")), KerberosSession::ErrorInitialize);
        }
        QCOMPARE(fake.contextsFreed, 1);
        QCOMPARE(fake.credentialsFreed, 1);
    }

    void initialSuccessPreservesToken()
    {
        fake.firstComplete = true;
        KerberosSession session;
        QCOMPARE(session.init(QStringLiteral("service/host.test")), KerberosSession::Success);
        QByteArray output;
        bool       authenticated = false;
        QCOMPARE(session.step({}, &output, &authenticated), KerberosSession::Success);
        QVERIFY(authenticated);
        QCOMPARE(output, QByteArray("auth"));
    }

    void largeFramedStream()
    {
        SaslGssapiSession session;
        QVERIFY(authenticate(session));
        const QByteArray input(8 * 1024 * 1024, 'x');
        QByteArray       wire, output;
        QCOMPARE(session.encode(input, &wire), SaslGssapiSession::Success);
        QCOMPARE(session.decode(wire, &output), SaslGssapiSession::Success);
        QCOMPARE(output, input);
        QVERIFY(fake.largestToken <= 8192);
        QVERIFY(fake.decryptCalls > 1000);
    }

    void fragmentedFrames()
    {
        SaslGssapiSession session;
        QVERIFY(authenticate(session, SaslGssapiSession::RequireConf, 32));
        const QByteArray input(128, 'x');
        QByteArray       wire, output, total;
        QCOMPARE(session.encode(input, &wire), SaslGssapiSession::Success);
        for (char byte : wire) {
            QCOMPARE(session.decode(QByteArray(1, byte), &output), SaslGssapiSession::Success);
            total += output;
        }
        QCOMPARE(total, input);
        QVERIFY(fake.largestToken <= 32);
    }

    void oversizedHeader()
    {
        SaslGssapiSession session;
        QVERIFY(authenticate(session));
        const int  calls = fake.decryptCalls;
        QByteArray output("old"), wire(4, char(0xff));
        wire += QByteArray(1024 * 1024, 'x');
        QCOMPARE(session.decode(wire, &output), SaslGssapiSession::ErrorDecodeTooLarge);
        QVERIFY(output.isEmpty());
        QCOMPARE(fake.decryptCalls, calls);
    }

    void confidentialityFailureDiscardsOutput()
    {
        SaslGssapiSession session;
        QVERIFY(authenticate(session));
        QByteArray wire, output("old");
        QCOMPARE(session.encode("secret", &wire), SaslGssapiSession::Success);
        fake.encrypted = false;
        QCOMPARE(session.decode(wire, &output), SaslGssapiSession::ErrorDecodeNotEncrypted);
        QVERIFY(output.isEmpty());
        QCOMPARE(session.encode("more data", &output), SaslGssapiSession::ErrorEncode);
    }

    void noSecurityLayer()
    {
        SaslGssapiSession session;
        QVERIFY(authenticate(session, SaslGssapiSession::DisableConf | SaslGssapiSession::DisableInt));
        QVERIFY(!session.do_layer);
        QByteArray output;
        QCOMPARE(session.encode("data", &output), SaslGssapiSession::Success);
        QCOMPARE(output, QByteArray("data"));
    }

    void rejectsWeakLayers()
    {
        SaslGssapiSession session;
        QVERIFY(!authenticate(session, SaslGssapiSession::RequireConf, 8192, 1));
    }

    void rejectsMissingMutualAuthentication()
    {
        fake.attributes &= ~ISC_RET_MUTUAL_AUTH;
        SaslGssapiSession session;
        QVERIFY(!authenticate(session, SaslGssapiSession::RequireMutualAuth));
        QCOMPARE(fake.decryptCalls, 0);
    }

    void rejectsUnusablePeerLimit()
    {
        SaslGssapiSession session;
        QVERIFY(!authenticate(session, SaslGssapiSession::RequireConf, 1));
    }

    void rejectsDataBeforeAuthentication()
    {
        SaslGssapiSession session;
        QByteArray        output("old");
        QCOMPARE(session.encode("secret", &output), SaslGssapiSession::ErrorEncode);
        QVERIFY(output.isEmpty());
        QCOMPARE(session.decode("wire", &output), SaslGssapiSession::ErrorDecode);
        QVERIFY(output.isEmpty());
    }
};

QTEST_GUILESS_MAIN(WinGssSessionTest)
#include "gssapi.moc"
