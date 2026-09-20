// SPDX-License-Identifier: LGPL-2.1-or-later
// Synthetic benchmark only: fixed/reused IVs MUST NOT be used in real encryption.
#include <QCoreApplication>
#include <QtCrypto>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <openssl/core_names.h>
#include <openssl/evp.h>

static void require(bool ok)
{
    if (!ok)
        qFatal("crypto operation or correctness check failed");
}
static volatile unsigned sink = 0;

template <class F>
void measure(const char *algorithm, int size, const char *mode, const char *backend, double seconds, F operation)
{
    for (int i = 0; i < 32; ++i)
        operation();
    const auto cpuStart = std::clock();
    const auto start    = std::chrono::steady_clock::now();
    quint64    count    = 0;
    double     elapsed;
    do {
        for (int i = 0; i < 64; ++i)
            operation();
        count += 64;
        elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    } while (elapsed < seconds);
    const double cpu = double(std::clock() - cpuStart) / CLOCKS_PER_SEC;
    std::printf("%s,%d,%s,%s,%llu,%.6f,%.6f,%.1f,%.1f,%.1f\n", algorithm, size, mode, backend,
                static_cast<unsigned long long>(count), elapsed, cpu, elapsed * 1e9 / count, cpu * 1e9 / count,
                double(size) * count / elapsed / 1048576.0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer init;
    const double     seconds = argc > 1 ? QByteArray(argv[1]).toDouble() : 0.2;
    const bool       reverse = argc > 2 && QByteArray(argv[2]) == "reverse";
    require(seconds >= 0.05 && seconds <= 10);
    const QString provider = QStringLiteral("qca-ossl");
    require(QCA::isSupported("aes128-ctr", provider));
    require(QCA::isSupported("hmac(sha1)", provider));
    std::fprintf(stderr, "OpenSSL=%s; Qt=%s; QCA provider=qca-ossl; seconds/case=%.2f\n",
                 OpenSSL_version(OPENSSL_VERSION), qVersion(), seconds);
    std::puts("algorithm,bytes,context,backend,operations,wall_s,cpu_s,wall_ns_op,cpu_ns_op,MiB_s");
    const QByteArray                keyBytes(16, 'k'), ivBytes(16, 'i');
    const QCA::SymmetricKey         key(keyBytes);
    const QCA::InitializationVector iv(ivBytes);
    const auto                     *rawKey = reinterpret_cast<const unsigned char *>(keyBytes.constData());
    const auto                     *rawIv  = reinterpret_cast<const unsigned char *>(ivBytes.constData());
    std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)> mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);
    require(bool(mac));
    char       digestName[] = "SHA1";
    OSSL_PARAM params[]
        = { OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestName, 0), OSSL_PARAM_construct_end() };
    for (int size : { 160, 1200, 16384 }) {
        const QByteArray bytes(size, 'x');
        // Allocate input outside timing. QCA owns output allocation; EVP uses a
        // reusable caller buffer. That difference is deliberately measured.
        const QCA::MemoryRegion input(bytes);
        QByteArray              output(size + 32, '\0');
        const auto             *raw = reinterpret_cast<const unsigned char *>(bytes.constData());
        auto                   *out = reinterpret_cast<unsigned char *>(output.data());
        for (bool fresh : { false, true }) {
            const char *mode = fresh ? "fresh" : "reuse";
            QCA::Cipher cipher("aes128", QCA::Cipher::CTR, QCA::Cipher::NoPadding, QCA::Encode, key, iv, provider);
            std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                                EVP_CIPHER_CTX_free);
            require(bool(ctx));
            QCA::MemoryRegion encrypted;
            auto              qcaAes = [&]() {
                std::unique_ptr<QCA::Cipher> temporary;
                auto                        *c = &cipher;
                if (fresh) {
                    temporary = std::make_unique<QCA::Cipher>("aes128", QCA::Cipher::CTR, QCA::Cipher::NoPadding,
                                                                           QCA::Encode, key, iv, provider);
                    c = temporary.get();
                } else
                    c->setup(QCA::Encode, key, iv);
                encrypted = c->update(input);
                require(c->ok());
                const auto tail = c->final();
                require(c->ok() && tail.size() == 0 && encrypted.size() == size);
                sink = unsigned(encrypted.constData()[0]);
            };
            auto directAes = [&]() {
                auto *c = fresh ? EVP_CIPHER_CTX_new() : ctx.get();
                require(c);
                int written = 0, tail = 0;
                require(EVP_EncryptInit_ex(c, EVP_aes_128_ctr(), nullptr, rawKey, rawIv) == 1);
                require(EVP_CIPHER_CTX_set_padding(c, 0) == 1);
                require(EVP_EncryptUpdate(c, out, &written, raw, size) == 1);
                require(EVP_EncryptFinal_ex(c, out + written, &tail) == 1 && written + tail == size);
                sink = out[0];
                if (fresh)
                    EVP_CIPHER_CTX_free(c);
            };
            qcaAes();
            directAes();
            require(std::memcmp(encrypted.constData(), output.constData(), size) == 0);
            if (!reverse)
                measure("AES128-CTR", size, mode, "EVP", seconds, directAes);
            measure("AES128-CTR", size, mode, "QCA", seconds, qcaAes);
            if (reverse)
                measure("AES128-CTR", size, mode, "EVP", seconds, directAes);

            QCA::MessageAuthenticationCode                            hmac("hmac(sha1)", key, provider);
            std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> mc(EVP_MAC_CTX_new(mac.get()), EVP_MAC_CTX_free);
            require(bool(mc));
            QCA::MemoryRegion tag;
            auto              qcaMac = [&]() {
                std::unique_ptr<QCA::MessageAuthenticationCode> temporary;
                auto                                           *m = &hmac;
                if (fresh) {
                    temporary = std::make_unique<QCA::MessageAuthenticationCode>("hmac(sha1)", key, provider);
                    m = temporary.get();
                } else
                    m->clear();
                m->update(input);
                tag = m->final();
                require(tag.size() == 20);
                sink = unsigned(tag.constData()[0]);
            };
            auto directMac = [&]() {
                auto *m = fresh ? EVP_MAC_CTX_new(mac.get()) : mc.get();
                require(m);
                size_t written = 0;
                require(EVP_MAC_init(m, rawKey, size_t(keyBytes.size()), params) == 1);
                require(EVP_MAC_update(m, raw, size_t(size)) == 1);
                require(EVP_MAC_final(m, out, &written, size_t(output.size())) == 1 && written == 20);
                sink = out[0];
                if (fresh)
                    EVP_MAC_CTX_free(m);
            };
            qcaMac();
            directMac();
            require(std::memcmp(tag.constData(), output.constData(), 20) == 0);
            if (!reverse)
                measure("HMAC-SHA1", size, mode, "EVP", seconds, directMac);
            measure("HMAC-SHA1", size, mode, "QCA", seconds, qcaMac);
            if (reverse)
                measure("HMAC-SHA1", size, mode, "EVP", seconds, directMac);
        }
    }
}
