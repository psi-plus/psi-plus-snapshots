// SPDX-License-Identifier: LGPL-2.1-or-later
// The JET stream cipher: streaming AES-GCM equals one-shot AES-GCM with the tag appended, the
// decryptor accepts any chunking and refuses tampered or truncated data.
#include "jingle-jet_p.h"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QtCrypto>

using namespace XMPP::Jingle::JET;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static QByteArray randomBytes(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (auto &byte : data)
        byte = char(QRandomGenerator::global()->bounded(256));
    return data;
}

static QList<QByteArray> chunks(const QByteArray &data)
{
    QList<QByteArray> result;
    for (int pos = 0; pos < data.size();) {
        const int size = 1 + QRandomGenerator::global()->bounded(4000);
        result.append(data.mid(pos, size));
        pos += size;
    }
    return result;
}

static QByteArray decryptAll(const QByteArray &key, const QByteArray &iv, const QByteArray &wire, bool *verified)
{
    auto       decryptor = StreamCipher::decryptor(key, iv);
    QByteArray plain;
    for (const auto &chunk : chunks(wire))
        plain += decryptor->process(chunk);
    plain += decryptor->finish();
    *verified = decryptor->verify();
    return plain;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    check(keySize(QStringLiteral("urn:xmpp:ciphers:aes-128-gcm-nopadding")) == 16
              && keySize(QStringLiteral("urn:xmpp:ciphers:aes-128-gcm-nopadding:0")) == 16
              && keySize(QStringLiteral("urn:xmpp:ciphers:aes-256-gcm-nopadding")) == 32
              && !keySize(QStringLiteral("urn:xmpp:ciphers:unknown")),
          "cipher namespaces are not recognised");

    for (int keyBytes : { 16, 32 }) {
        if (!StreamCipher::isSupported(keyBytes)) {
            qInfo("AES-%d GCM/CTR unavailable in QCA, skipped", keyBytes * 8);
            continue;
        }
        for (int size : { 0, 1, 15, 16, 17, 4096, 100000 }) {
            const auto key   = randomBytes(keyBytes);
            const auto iv    = randomBytes(12);
            const auto plain = randomBytes(size);

            auto       encryptor = StreamCipher::encryptor(key, iv);
            QByteArray wire;
            for (const auto &chunk : chunks(plain))
                wire += encryptor->process(chunk);
            wire += encryptor->finish();
            check(wire.size() == size + StreamCipher::TagSize, "the tag was not appended");

            // QCA's own GCM decoder, given our tag, must restore the data and accept the tag.
            if (size > 0) {
                QCA::Cipher reference(keyBytes == 32 ? QStringLiteral("aes256") : QStringLiteral("aes128"),
                                      QCA::Cipher::GCM, QCA::Cipher::NoPadding, QCA::Decode, QCA::SymmetricKey(key),
                                      QCA::InitializationVector(iv), QCA::AuthTag(wire.right(StreamCipher::TagSize)));
                const auto restored = reference.process(wire.left(size)).toByteArray();
                check(reference.ok() && restored == plain, "QCA's GCM decoder refused the stream");
                auto wrongTag = wire.right(StreamCipher::TagSize);
                wrongTag[0]   = char(wrongTag[0] ^ 1);
                QCA::Cipher negative(keyBytes == 32 ? QStringLiteral("aes256") : QStringLiteral("aes128"),
                                     QCA::Cipher::GCM, QCA::Cipher::NoPadding, QCA::Decode, QCA::SymmetricKey(key),
                                     QCA::InitializationVector(iv), QCA::AuthTag(wrongTag));
                negative.process(wire.left(size));
                check(!negative.ok(), "QCA's GCM decoder does not check tags, so it proves nothing");
            }

            bool verified = false;
            check(decryptAll(key, iv, wire, &verified) == plain && verified, "the stream was not decrypted");

            if (size > 0) {
                auto tampered      = wire;
                tampered[size / 2] = char(tampered[size / 2] ^ 1);
                decryptAll(key, iv, tampered, &verified);
                check(!verified, "a tampered stream was verified");
            }
            auto badTag               = wire;
            badTag[badTag.size() - 1] = char(badTag[badTag.size() - 1] ^ 1);
            decryptAll(key, iv, badTag, &verified);
            check(!verified, "a stream with a wrong tag was verified");
            decryptAll(key, iv, wire.left(wire.size() - 1), &verified);
            check(!verified, "a truncated stream was verified");
        }
    }
    check(!StreamCipher::decryptor(randomBytes(16), randomBytes(16)), "a 128-bit IV was accepted");
    qInfo("JET stream cipher tests passed");
    return 0;
}
