// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QtCrypto>

namespace wincryptoQCAPlugin {
QCA::Provider::Context *createCertificateContext(QCA::Provider *provider);
QCA::Provider::Context *createTLSContext(QCA::Provider *provider, bool datagram = false);
}
