// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QtCrypto>
#include <windows.h>

#include <memory>
#include <ncrypt.h>
#include <wincrypt.h>

namespace wincryptoQCAPlugin {
QCA::Provider::Context *createRSAContext(QCA::Provider *provider);
QCA::Provider::Context *createPKeyContext(QCA::Provider *provider);
// The caller must keep the PKeyContext alive while using this borrowed handle.
NCRYPT_KEY_HANDLE nativePrivateKey(const QCA::PKeyContext &key);
QByteArray        createSelfSignedCertificate(const QCA::CertificateOptions &options, const QCA::PKeyContext &key);
// Delete the temporary persisted copy immediately after Schannel credentials open.
struct CredentialCertificate
{
    PCCERT_CONTEXT    certificate  = nullptr;
    HCERTSTORE        store        = nullptr;
    NCRYPT_KEY_HANDLE temporaryKey = 0;
    ~CredentialCertificate();
    bool removeTemporaryKey();
};
std::unique_ptr<CredentialCertificate> createCredentialCertificate(const QCA::CertificateChain &chain,
                                                                   const QCA::PrivateKey       &key);
}
