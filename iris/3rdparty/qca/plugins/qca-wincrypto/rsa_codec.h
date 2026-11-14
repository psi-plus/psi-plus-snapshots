// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QtCrypto>

namespace wincryptoQCAPlugin {
struct RsaParts
{
    QCA::SecureArray n, e, d, p, q, dp, dq, qi;
};
bool               decodePublicRSA(const QCA::MemoryRegion &der, RsaParts *parts);
QCA::ConvertResult decodePrivateRSA(const QCA::SecureArray &der,
                                    const QCA::SecureArray &passphrase,
                                    const QString          &provider,
                                    RsaParts               *parts);
QCA::SecureArray   encodePublicRSA(const RsaParts &parts);
QCA::SecureArray   protectPrivateRSA(const QCA::SecureArray &der,
                                     const QCA::SecureArray &passphrase,
                                     QCA::PBEAlgorithm       algorithm,
                                     const QString          &provider);
QString            encodeKeyPEM(const QCA::SecureArray &der, const QString &label);
bool               decodeKeyPEM(const QString &pem, bool privateKey, QCA::SecureArray *der);
}
