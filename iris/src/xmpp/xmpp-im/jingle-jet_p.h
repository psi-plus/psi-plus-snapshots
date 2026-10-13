/*
 * jingle-jet_p.h - stream cipher of Jingle Encrypted Transports (XEP-0391)
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

#ifndef JINGLE_JET_P_H
#define JINGLE_JET_P_H

#include <iris/iris_export.h>

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

namespace XMPP { namespace Jingle { namespace JET {
    /// Key size of a JET cipher namespace, written with or without the ":0" suffix; nullopt if unknown.
    IRIS_EXPORT std::optional<int> keySize(const QString &cipher);
    /// The cipher namespace for a key size, as Conversations and Dino write it.
    IRIS_EXPORT QString cipherFor(int keySize);

    /**
     * AES-GCM over a whole file transfer with the 16-byte tag appended, as Conversations and Dino send
     * JET-protected files. The decryptor holds back the last 16 bytes it received and checks them as the
     * tag at the end; the data it returned before is not authenticated until verify() succeeds.
     */
    class IRIS_EXPORT StreamCipher {
    public:
        static bool                          isSupported(int keySize);
        static std::unique_ptr<StreamCipher> encryptor(const QByteArray &key, const QByteArray &iv);
        static std::unique_ptr<StreamCipher> decryptor(const QByteArray &key, const QByteArray &iv);
        virtual ~StreamCipher() = default;

        /// Encryptor: ciphertext of @a data. Decryptor: plaintext of the bytes before the last 16 received.
        virtual QByteArray process(const QByteArray &data) = 0;
        /// Encryptor: the tag to append. Decryptor: an empty array.
        virtual QByteArray finish() = 0;
        /// Decryptor: true if the last 16 bytes received are the tag of everything before them.
        virtual bool verify() = 0;
        /// Decryptor: bytes held back as the possible tag.
        static constexpr int TagSize = 16;
    };
}}}

#endif // JINGLE_JET_P_H
