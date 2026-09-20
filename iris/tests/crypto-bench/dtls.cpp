// SPDX-License-Identifier: LGPL-2.1-or-later
// In-process DTLS record benchmark. No sockets, SCTP, ICE or packet loss.
#include <QCoreApplication>
#include <QDateTime>
#include <QtCrypto>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

static void check(bool ok, const char *why)
{
    if (!ok) {
        ERR_print_errors_fp(stderr);
        qFatal("%s", why);
    }
}
using Clock                   = std::chrono::steady_clock;
static const char   *suite    = "ECDHE-RSA-AES256-GCM-SHA384";
static const char   *qcaSuite = "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384";
static constexpr int mtu      = 1400;

struct QcaPair {
    QCA::TLS sender { QCA::TLS::Datagram, nullptr, QStringLiteral("qca-ossl") };
    QCA::TLS receiver { QCA::TLS::Datagram, nullptr, QStringLiteral("qca-ossl") };
    bool     clientReady = false, serverReady = false;
    explicit QcaPair(const QCA::Certificate &cert, const QCA::PrivateKey &key)
    {
        for (auto *tls : { &sender, &receiver }) {
            tls->setCertificate(QCA::CertificateChain(cert), key);
            tls->setConstraints(QStringList { QString::fromLatin1(qcaSuite) });
            tls->setPacketMTU(mtu);
            QObject::connect(tls, &QCA::TLS::error, tls, [] { qFatal("QCA DTLS error"); });
            QObject::connect(tls, &QCA::TLS::certificateRequested, tls, &QCA::TLS::continueAfterStep);
            QObject::connect(tls, &QCA::TLS::handshaken, tls, [this, tls, cert] {
                // Pin the generated peer certificate outside the timed workload.
                if (tls == &sender) {
                    check(!tls->peerCertificateChain().isEmpty()
                              && tls->peerCertificateChain().primary().toDER() == cert.toDER(),
                          "QCA peer certificate");
                    clientReady = true;
                } else
                    serverReady = true;
                tls->continueAfterStep();
            });
        }
        receiver.startServer();
        sender.startClient();
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!clientReady || !serverReady) {
            pump();
            check(Clock::now() < deadline, "QCA handshake timeout");
        }
        pump();
        std::fprintf(stderr, "QCA cipher=%s version=%d MTU=%d\n", qPrintable(sender.cipherSuite()),
                     int(sender.version()), sender.packetMTU());
        check(sender.version() == QCA::TLS::DTLS_v1_2 && sender.cipherSuite() == QString::fromLatin1(qcaSuite),
              "QCA cipher mismatch");
    }
    void pump()
    {
        QCoreApplication::processEvents();
        while (sender.packetsOutgoingAvailable())
            receiver.writeIncoming(sender.readOutgoing());
        while (receiver.packetsOutgoingAvailable())
            sender.writeIncoming(receiver.readOutgoing());
    }
    void transfer(const QByteArray &payload)
    {
        sender.write(payload);
        int spins = 0;
        while (!receiver.packetsAvailable()) {
            pump();
            check(++spins < 10000, "QCA record stalled");
        }
        check(receiver.read() == payload, "QCA plaintext mismatch");
    }
};

struct NativePair {
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context { SSL_CTX_new(DTLS_method()), SSL_CTX_free };
    std::unique_ptr<SSL, decltype(&SSL_free)>         sender { nullptr, SSL_free }, receiver { nullptr, SSL_free };
    explicit NativePair(const QCA::Certificate &cert, const QCA::PrivateKey &key)
    {
        const auto cpem = cert.toPEM().toUtf8(), kpem = key.toPEM().toUtf8();
        BIO  *cb = BIO_new_mem_buf(cpem.constData(), cpem.size()), *kb = BIO_new_mem_buf(kpem.constData(), kpem.size());
        X509 *certificate    = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
        EVP_PKEY *privateKey = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
        check(context && certificate && privateKey, "native identity");
        check(SSL_CTX_use_certificate(context.get(), certificate) == 1
                  && SSL_CTX_use_PrivateKey(context.get(), privateKey) == 1,
              "native credentials");
        check(SSL_CTX_set_min_proto_version(context.get(), DTLS1_2_VERSION) == 1
                  && SSL_CTX_set_max_proto_version(context.get(), DTLS1_2_VERSION) == 1
                  && SSL_CTX_set_cipher_list(context.get(), suite) == 1,
              "native constraints");
        EVP_PKEY_free(privateKey);
        BIO_free(cb);
        BIO_free(kb);
        sender.reset(SSL_new(context.get()));
        receiver.reset(SSL_new(context.get()));
        for (auto *ssl : { sender.get(), receiver.get() }) {
            check(ssl, "native SSL allocation");
            SSL_set_bio(ssl, BIO_new(BIO_s_dgram_mem()), BIO_new(BIO_s_dgram_mem()));
            SSL_set_options(ssl, SSL_OP_NO_QUERY_MTU);
            SSL_set_mtu(ssl, mtu);
        }
        SSL_set_connect_state(sender.get());
        SSL_set_accept_state(receiver.get());
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (!SSL_is_init_finished(sender.get()) || !SSL_is_init_finished(receiver.get())) {
            for (auto *ssl : { sender.get(), receiver.get() }) {
                if (!SSL_is_init_finished(ssl)) {
                    const int result = SSL_do_handshake(ssl);
                    const int error  = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
                    check(error == SSL_ERROR_NONE || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
                          "native handshake");
                }
            }
            pump();
            check(Clock::now() < deadline, "native handshake timeout");
        }
        X509 *peer = SSL_get1_peer_certificate(sender.get());
        check(peer && X509_cmp(peer, certificate) == 0, "native peer certificate");
        X509_free(peer);
        X509_free(certificate);
        std::fprintf(stderr, "OpenSSL cipher=%s version=%s MTU=%d\n", SSL_get_cipher_name(sender.get()),
                     SSL_get_version(sender.get()), mtu);
        check(std::strcmp(SSL_get_cipher_name(sender.get()), suite) == 0, "native cipher mismatch");
    }
    void pump()
    {
        char wire[65536];
        for (auto *ssl : { sender.get(), receiver.get() }) {
            auto *other = ssl == sender.get() ? receiver.get() : sender.get();
            while (BIO_ctrl_pending(SSL_get_wbio(ssl))) {
                const int n = BIO_read(SSL_get_wbio(ssl), wire, sizeof(wire));
                check(n > 0 && BIO_write(SSL_get_rbio(other), wire, n) == n, "native datagram relay");
            }
        }
    }
    void transfer(const QByteArray &payload)
    {
        char plaintext[16384];
        check(SSL_write(sender.get(), payload.constData(), payload.size()) == payload.size(), "native write");
        pump();
        const int n = SSL_read(receiver.get(), plaintext, sizeof(plaintext));
        check(n == payload.size() && std::memcmp(plaintext, payload.constData(), size_t(n)) == 0,
              "native plaintext mismatch");
    }
};

template <class Pair> void measure(Pair &pair, const char *backend, int size, double seconds)
{
    const QByteArray payload(size, 'x');
    for (int i = 0; i < 128; ++i)
        pair.transfer(payload);
    const auto cpu   = std::clock();
    const auto start = Clock::now();
    quint64    count = 0;
    double     elapsed;
    do {
        for (int i = 0; i < 32; ++i)
            pair.transfer(payload);
        count += 32;
        elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    } while (elapsed < seconds);
    const double cpuSeconds = double(std::clock() - cpu) / CLOCKS_PER_SEC;
    std::printf("%s,%d,%llu,%.6f,%.6f,%.1f,%.1f\n", backend, size, static_cast<unsigned long long>(count), elapsed,
                cpuSeconds, cpuSeconds * 1e9 / count, double(size) * count / elapsed / 1048576.0);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer init;
    const double     seconds = argc > 1 ? QByteArray(argv[1]).toDouble() : 0.5;
    check(seconds >= 0.05 && seconds <= 10, "invalid duration");
    QCA::CertificateOptions options;
    QCA::CertificateInfo    info;
    info.insert(QCA::CommonName, QStringLiteral("dtls-benchmark"));
    options.setInfo(info);
    options.setSerialNumber(QCA::BigInteger(1));
    options.setValidityPeriod(QDateTime::currentDateTimeUtc().addDays(-1), QDateTime::currentDateTimeUtc().addDays(1));
    const auto             key = QCA::KeyGenerator().createRSA(2048);
    const QCA::Certificate cert(options, key);
    check(!cert.isNull(), "certificate generation");
    QcaPair    qca(cert, key);
    NativePair native(cert, key);
    std::puts("backend,bytes,records,wall_s,cpu_s,cpu_ns_record,MiB_s");
    for (int size : { 160, 512, 1200 }) {
        if (argc <= 2)
            measure(native, "OpenSSL", size, seconds);
        measure(qca, "QCA", size, seconds);
        if (argc > 2)
            measure(native, "OpenSSL", size, seconds);
    }
}
