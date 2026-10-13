/*
 * jingle-jet.cpp - stream cipher of Jingle Encrypted Transports (XEP-0391)
 * Copyright (C) 2026  Sergey Ilinykh
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "jingle-jet_p.h"

#include <QtCrypto>

namespace XMPP { namespace Jingle { namespace JET {
    namespace {
        QString algorithm(int keySize) { return keySize == 32 ? QStringLiteral("aes256") : QStringLiteral("aes128"); }

        QCA::Cipher gcm(const QByteArray &key, const QByteArray &iv)
        {
            // QCA takes the requested tag size from the AuthTag given to an encoder.
            return QCA::Cipher(algorithm(int(key.size())), QCA::Cipher::GCM, QCA::Cipher::NoPadding, QCA::Encode,
                               QCA::SymmetricKey(key), QCA::InitializationVector(iv),
                               QCA::AuthTag(QByteArray(StreamCipher::TagSize, '\0')));
        }

        class Encryptor final : public StreamCipher {
        public:
            Encryptor(const QByteArray &key, const QByteArray &iv) : cipher(gcm(key, iv)) { }
            QByteArray process(const QByteArray &data) override { return cipher.update(data).toByteArray(); }
            QByteArray finish() override
            {
                auto rest = cipher.final().toByteArray(); // empty for a stream mode
                return cipher.ok() ? rest + cipher.tag().toByteArray() : QByteArray();
            }
            bool verify() override { return false; }

        private:
            QCA::Cipher cipher;
        };

        // GCM encrypts with AES-CTR starting at the counter block IV || 2 (for a 96-bit IV). The tag is
        // checked by encrypting the decrypted data again with GCM, which yields the same ciphertext and tag.
        class Decryptor final : public StreamCipher {
        public:
            Decryptor(const QByteArray &key, const QByteArray &iv) :
                ctr(algorithm(int(key.size())), QCA::Cipher::CTR, QCA::Cipher::NoPadding, QCA::Decode,
                    QCA::SymmetricKey(key), QCA::InitializationVector(iv + QByteArray::fromHex("00000002"))),
                check(gcm(key, iv))
            {
            }
            QByteArray process(const QByteArray &data) override
            {
                held += data;
                if (held.size() <= TagSize)
                    return {};
                const auto ciphertext = held.left(held.size() - TagSize);
                held                  = held.right(TagSize);
                const auto plaintext  = ctr.update(ciphertext).toByteArray();
                // ok() of a QCA cipher is only meaningful after an operation.
                failed = failed || !ctr.ok() || plaintext.size() != ciphertext.size();
                check.update(plaintext);
                return plaintext;
            }
            QByteArray finish() override { return {}; }
            bool       verify() override
            {
                if (verified)
                    return *verified;
                check.final();
                verified = held.size() == TagSize && !failed && check.ok() && check.tag().toByteArray() == held;
                return *verified;
            }

        private:
            QCA::Cipher         ctr;
            QCA::Cipher         check;
            QByteArray          held;
            bool                failed = false;
            std::optional<bool> verified;
        };
    }

    std::optional<int> keySize(const QString &cipher)
    {
        auto name = cipher;
        if (name.endsWith(QLatin1String(":0")))
            name.chop(2);
        if (name == QLatin1String("urn:xmpp:ciphers:aes-128-gcm-nopadding"))
            return 16;
        if (name == QLatin1String("urn:xmpp:ciphers:aes-256-gcm-nopadding"))
            return 32;
        return std::nullopt;
    }

    QString cipherFor(int keySize)
    {
        return keySize == 32 ? QStringLiteral("urn:xmpp:ciphers:aes-256-gcm-nopadding")
                             : QStringLiteral("urn:xmpp:ciphers:aes-128-gcm-nopadding");
    }

    bool StreamCipher::isSupported(int keySize)
    {
        const auto name = algorithm(keySize);
        return (keySize == 16 || keySize == 32) && QCA::isSupported(qPrintable(name + QStringLiteral("-gcm")))
            && QCA::isSupported(qPrintable(name + QStringLiteral("-ctr")));
    }

    std::unique_ptr<StreamCipher> StreamCipher::encryptor(const QByteArray &key, const QByteArray &iv)
    {
        if (!isSupported(int(key.size())) || iv.size() != 12)
            return {};
        return std::make_unique<Encryptor>(key, iv);
    }

    std::unique_ptr<StreamCipher> StreamCipher::decryptor(const QByteArray &key, const QByteArray &iv)
    {
        // The CTR shortcut needs the 96-bit IV of JET.
        if (!isSupported(int(key.size())) || iv.size() != 12)
            return {};
        return std::make_unique<Decryptor>(key, iv);
    }
}}}
