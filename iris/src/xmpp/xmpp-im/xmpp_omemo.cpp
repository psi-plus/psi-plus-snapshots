/*
 * xmpp_omemo.cpp - XEP-0384 OMEMO encryption method
 * Copyright (C) 2018-2024 Vyacheslav Karpukhin, Psi IM team
 * Copyright (C) 2020 Boris Pek
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#include "xmpp_omemo.h"

#include "xmpp_caps.h"
#include "xmpp_client.h"
#include "xmpp_pubsub.h"
#include "xmpp_sce.h"

#include <QtCrypto>

#include <QDateTime>
#include <QDomDocument>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

extern "C" {
#include <curve.h>
#include <key_helper.h>
#include <protocol.h>
#include <ratchet.h>
#include <session_builder.h>
#include <session_cipher.h>
#include <session_pre_key.h>
#include <signal_protocol.h>
}

namespace XMPP {
namespace {
    constexpr auto OmemoNs             = "urn:xmpp:omemo:2";
    constexpr auto DevicesNode         = "urn:xmpp:omemo:2:devices";
    constexpr auto BundlesNode         = "urn:xmpp:omemo:2:bundles";
    constexpr auto LegacyOmemoNs       = "eu.siacs.conversations.axolotl";
    constexpr auto LegacyDevicesNode   = "eu.siacs.conversations.axolotl.devicelist";
    constexpr auto LegacyBundlesPrefix = "eu.siacs.conversations.axolotl.bundles:";
    constexpr auto EmeNs               = "urn:xmpp:eme:0";
    constexpr auto HintsNs             = "urn:xmpp:hints";
    constexpr auto OmemoName           = "OMEMO";
    constexpr auto OmemoProtocolOption = "omemoProtocol";
    constexpr int  PreKeyTarget        = 100;
    constexpr int  PreKeyMinimum       = 25;

    QString localName(const QDomElement &element)
    {
        return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : element.localName();
    }

    QString protocolNamespace(OmemoProtocol protocol)
    {
        return protocol == OmemoProtocol::Legacy ? QLatin1String(LegacyOmemoNs) : QLatin1String(OmemoNs);
    }

    QString protocolDevicesNode(OmemoProtocol protocol)
    {
        return protocol == OmemoProtocol::Legacy ? QLatin1String(LegacyDevicesNode) : QLatin1String(DevicesNode);
    }

    QString protocolBundleNode(OmemoProtocol protocol, uint32_t deviceId)
    {
        if (protocol == OmemoProtocol::Legacy)
            return QLatin1String(LegacyBundlesPrefix) + QString::number(deviceId);
        return QLatin1String(BundlesNode);
    }

    QString protocolName(OmemoProtocol protocol)
    {
        return protocol == OmemoProtocol::Legacy ? QStringLiteral("legacy") : QStringLiteral("omemo2");
    }

    std::optional<OmemoProtocol> protocolFromName(const QVariant &value)
    {
        if (!value.isValid())
            return std::nullopt;
        const auto text = value.toString().trimmed().toLower();
        if (text == QLatin1String("legacy") || text == QLatin1String("omemo03") || text == QLatin1String("0.3"))
            return OmemoProtocol::Legacy;
        if (text == QLatin1String("omemo2") || text == QLatin1String("modern") || text == QLatin1String("2"))
            return OmemoProtocol::Omemo2;
        bool      ok  = false;
        const int raw = value.toInt(&ok);
        if (ok && raw == static_cast<int>(OmemoProtocol::Legacy))
            return OmemoProtocol::Legacy;
        if (ok && raw == static_cast<int>(OmemoProtocol::Omemo2))
            return OmemoProtocol::Omemo2;
        return std::nullopt;
    }

    QDomElement directChildNS(const QDomElement &parent, const QString &name, const QString &ns)
    {
        for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (localName(child) == name && child.namespaceURI() == ns)
                return child;
        }
        return {};
    }

    QList<QDomElement> directChildrenNS(const QDomElement &parent, const QString &name, const QString &ns)
    {
        QList<QDomElement> result;
        for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (localName(child) == name && child.namespaceURI() == ns)
                result.append(child);
        }
        return result;
    }

    QByteArray fromSignalBuffer(const signal_buffer *buffer)
    {
        if (!buffer)
            return {};
        return QByteArray(reinterpret_cast<const char *>(signal_buffer_data(const_cast<signal_buffer *>(buffer))),
                          static_cast<int>(signal_buffer_len(const_cast<signal_buffer *>(buffer))));
    }

    signal_buffer *toSignalBuffer(const QByteArray &data)
    {
        return signal_buffer_create(reinterpret_cast<const uint8_t *>(data.constData()),
                                    static_cast<size_t>(data.size()));
    }

    QByteArray bytes(const uint8_t *data, size_t size)
    {
        return data ? QByteArray(reinterpret_cast<const char *>(data), static_cast<int>(size)) : QByteArray();
    }

    QString canonicalBare(const Jid &jid) { return jid.isValid() ? jid.bare() : QString(); }
    QString addressName(const signal_protocol_address *address)
    {
        return address ? QString::fromUtf8(address->name, static_cast<qsizetype>(address->name_len)) : QString();
    }

    signal_protocol_address signalAddress(const QByteArray &bareJid, uint32_t deviceId)
    {
        signal_protocol_address address {};
        address.name      = bareJid.constData();
        address.name_len  = static_cast<size_t>(bareJid.size());
        address.device_id = static_cast<int32_t>(deviceId);
        return address;
    }

    QString cipherName(size_t keyLength)
    {
        switch (keyLength) {
        case 16:
            return QStringLiteral("aes128");
        case 24:
            return QStringLiteral("aes192");
        case 32:
            return QStringLiteral("aes256");
        default:
            return {};
        }
    }

    bool constantEqual(const QByteArray &left, const QByteArray &right)
    {
        if (left.size() != right.size())
            return false;
        unsigned char difference = 0;
        for (qsizetype i = 0; i < left.size(); ++i)
            difference |= static_cast<unsigned char>(left.at(i)) ^ static_cast<unsigned char>(right.at(i));
        return difference == 0;
    }

    QByteArray qcaHmacSha256(const QByteArray &key, const QByteArray &input)
    {
        QCA::MessageAuthenticationCode mac(QStringLiteral("hmac(sha256)"), QCA::SymmetricKey(key));
        mac.update(QCA::MemoryRegion(input));
        return mac.final().toByteArray();
    }

    QByteArray qcaHkdfSha256(const QByteArray &ikm, QByteArray salt, const QByteArray &info, int outputLength)
    {
        constexpr int HashLength = 32;
        if (outputLength <= 0 || outputLength > 255 * HashLength)
            return {};
        if (salt.isEmpty())
            salt = QByteArray(HashLength, '\0');

        const QByteArray prk = qcaHmacSha256(salt, ikm);
        if (prk.size() != HashLength)
            return {};

        QByteArray output;
        QByteArray previous;
        for (quint8 counter = 1; output.size() < outputLength; ++counter) {
            QByteArray input = previous;
            input += info;
            input.append(static_cast<char>(counter));
            previous = qcaHmacSha256(prk, input);
            if (previous.size() != HashLength)
                return {};
            output += previous;
        }
        output.truncate(outputLength);
        return output;
    }

    QByteArray qcaAesCbc(bool encrypt, const QByteArray &key, const QByteArray &iv, const QByteArray &input)
    {
        QCA::Cipher cipher(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7,
                           encrypt ? QCA::Encode : QCA::Decode, QCA::SymmetricKey(key), QCA::InitializationVector(iv));
        const auto  result = cipher.process(QCA::MemoryRegion(input));
        return cipher.ok() ? result.toByteArray() : QByteArray();
    }

    struct GcmResult {
        QByteArray data;
        QByteArray tag;
    };

    std::optional<GcmResult> qcaAesGcm(bool encrypt, const QByteArray &key, const QByteArray &iv,
                                       const QByteArray &input, const QByteArray &tag = {})
    {
        // QCA uses the supplied AuthTag size as the requested output tag size
        // when encoding.  An empty tag therefore makes encryption succeed but
        // leaves Cipher::tag() empty.
        const QByteArray authTag = encrypt && tag.isEmpty() ? QByteArray(16, '\0') : tag;
        QCA::Cipher      cipher(QStringLiteral("aes128"), QCA::Cipher::GCM, QCA::Cipher::NoPadding,
                           encrypt ? QCA::Encode : QCA::Decode, QCA::SymmetricKey(key), QCA::InitializationVector(iv),
                                QCA::AuthTag(authTag));
        const auto       result = cipher.process(QCA::MemoryRegion(input));
        if (!cipher.ok())
            return std::nullopt;
        return GcmResult { result.toByteArray(), cipher.tag().toByteArray() };
    }

    struct PayloadMaterial {
        QByteArray encryptionKey;
        QByteArray authenticationKey;
        QByteArray iv;
    };

    std::optional<PayloadMaterial> payloadMaterial(const QByteArray &key)
    {
        if (key.size() != 32)
            return std::nullopt;
        const auto material = qcaHkdfSha256(key, QByteArray(32, '\0'), QByteArrayLiteral("OMEMO Payload"), 80);
        if (material.size() != 80)
            return std::nullopt;
        return PayloadMaterial { material.mid(0, 32), material.mid(32, 32), material.mid(64, 16) };
    }

    QByteArray randomBytes(int length)
    {
        return length > 0 ? QCA::Random::randomArray(length).toByteArray() : QByteArray();
    }

    uint32_t randomIndex(uint32_t upperExclusive)
    {
        if (upperExclusive <= 1)
            return 0;
        const auto bytes = QCA::Random::randomArray(4).toByteArray();
        if (bytes.size() != 4)
            return 0;
        const auto value = (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(0))) << 24)
            | (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(1))) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(2))) << 8)
            | static_cast<uint32_t>(static_cast<unsigned char>(bytes.at(3)));
        return value % upperExclusive;
    }

    bool parsePositiveInt32(const QString &text, uint32_t *value)
    {
        bool       ok     = false;
        const auto number = text.toULongLong(&ok);
        if (!ok || number == 0 || number > static_cast<qulonglong>(INT32_MAX))
            return false;
        if (value)
            *value = static_cast<uint32_t>(number);
        return true;
    }

    QByteArray base64Element(const QDomElement &element, bool *ok = nullptr)
    {
        const QByteArray encoded = element.text().trimmed().toLatin1();
        const QByteArray decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        const bool       valid   = !encoded.isEmpty() && !decoded.isNull();
        if (ok)
            *ok = valid;
        return valid ? decoded : QByteArray();
    }

    QDomElement appendBase64(QDomDocument &document, QDomElement &parent, const QString &name, const QByteArray &value,
                             const QString &ns = QLatin1String(OmemoNs))
    {
        auto element = document.createElementNS(ns, name);
        element.appendChild(document.createTextNode(QString::fromLatin1(value.toBase64())));
        parent.appendChild(element);
        return element;
    }

    struct EncryptedElement {
        OmemoProtocol protocol = OmemoProtocol::Omemo2;
        QDomElement   element;
    };

    std::optional<EncryptedElement> findOmemoElement(const QDomElement &stanza)
    {
        auto element = directChildNS(stanza, QStringLiteral("encrypted"), QLatin1String(OmemoNs));
        if (!element.isNull())
            return EncryptedElement { OmemoProtocol::Omemo2, element };
        element = directChildNS(stanza, QStringLiteral("encrypted"), QLatin1String(LegacyOmemoNs));
        if (!element.isNull())
            return EncryptedElement { OmemoProtocol::Legacy, element };
        return std::nullopt;
    }

    EncryptionJob::Error signalErrorToJob(int error)
    {
        switch (error) {
        case SG_ERR_UNTRUSTED_IDENTITY:
            return EncryptionJob::Error::UntrustedIdentity;
        case SG_ERR_NO_SESSION:
        case SG_ERR_INVALID_KEY_ID:
            return EncryptionJob::Error::NoSession;
        case SG_ERR_INVALID_MESSAGE:
        case SG_ERR_INVALID_KEY:
        case SG_ERR_LEGACY_MESSAGE:
        case SG_ERR_DUPLICATE_MESSAGE:
            return EncryptionJob::Error::ProtocolError;
        default:
            return EncryptionJob::Error::CryptoError;
        }
    }

    QString signalErrorString(const QString &operation, int code)
    {
        return QStringLiteral("%1 failed in libomemo-c (error %2)").arg(operation).arg(code);
    }

} // namespace

class OmemoEncryption::Private {
public:
    struct Bundle {
        OmemoProtocol                      protocol       = OmemoProtocol::Omemo2;
        uint32_t                           signedPreKeyId = 0;
        QByteArray                         signedPreKey;
        QByteArray                         signedPreKeySignature;
        QByteArray                         identityKey;
        QList<QPair<uint32_t, QByteArray>> preKeys;
    };

    struct EncryptedKey {
        QString    owner;
        uint32_t   deviceId    = 0;
        bool       keyExchange = false;
        QByteArray data;
    };

    OmemoEncryption                              *q            = nullptr;
    Client                                       *client       = nullptr;
    OmemoStorage                                 *storage      = nullptr;
    EncryptionTrustStorage                       *trustStorage = nullptr;
    std::unique_ptr<MemoryOmemoStorage>           ownedStorage;
    std::unique_ptr<MemoryEncryptionTrustStorage> ownedTrustStorage;
    OmemoStorage::OmemoData                       data;
    EncryptionTrustLevels                         acceptedTrust = EncryptionTrustLevel::AutomaticallyTrusted
        | EncryptionTrustLevel::ManuallyTrusted | EncryptionTrustLevel::Authenticated;
    EncryptionTrustLevel newIdentityTrust = EncryptionTrustLevel::AutomaticallyTrusted;
    QSet<QString>        fetchedDeviceLists;
    int                  minimumEnvelopeSize = 256;
    bool                 ready               = false;
    OmemoProtocols       supportedProtocols;

    struct SignalStore {
        Private                       *owner    = nullptr;
        OmemoProtocol                  protocol = OmemoProtocol::Omemo2;
        signal_protocol_store_context *context  = nullptr;
    };

    signal_context            *signalContext = nullptr;
    std::array<SignalStore, 2> signalStores;
    std::recursive_mutex       signalMutex;

    explicit Private(OmemoEncryption *q_, Client *client_, OmemoStorage *storage_, EncryptionTrustStorage *trust_) :
        q(q_), client(client_), storage(storage_), trustStorage(trust_)
    {
        if (!storage) {
            ownedStorage = std::make_unique<MemoryOmemoStorage>();
            storage      = ownedStorage.get();
        }
        if (!trustStorage) {
            ownedTrustStorage = std::make_unique<MemoryEncryptionTrustStorage>();
            trustStorage      = ownedTrustStorage.get();
        }
        data = storage->allData();
        initSignal();
    }

    ~Private()
    {
        for (auto &store : signalStores) {
            if (store.context)
                signal_protocol_store_context_destroy(store.context);
        }
        if (signalContext)
            signal_context_destroy(signalContext);
    }

    static Private      *self(void *userData) { return static_cast<Private *>(userData); }
    static SignalStore  *signalStore(void *userData) { return static_cast<SignalStore *>(userData); }
    static Private      *storeSelf(void *userData) { return signalStore(userData)->owner; }
    static OmemoProtocol storeProtocol(void *userData) { return signalStore(userData)->protocol; }

    SignalStore &store(OmemoProtocol protocol) { return signalStores[protocol == OmemoProtocol::Legacy ? 0 : 1]; }

    const SignalStore &store(OmemoProtocol protocol) const
    {
        return signalStores[protocol == OmemoProtocol::Legacy ? 0 : 1];
    }

    static QString fetchedListKey(const QString &bare, OmemoProtocol protocol)
    {
        return bare + QLatin1Char('\n') + protocolName(protocol);
    }

    static OmemoStorage::DeviceProtocolState protocolState(const OmemoStorage::Device &device, OmemoProtocol protocol)
    {
        return device.protocols.value(protocol);
    }

    bool setDevice(const QString &owner, uint32_t id, const OmemoStorage::Device &device)
    {
        auto bare = Jid(owner).bare();
        if (bare.isEmpty())
            bare = owner;
        if (!storage->addDevice(bare, id, device))
            return false;
        data.devices[bare][id] = device;
        emit q->deviceChanged(Jid(bare), id);
        return true;
    }

    bool setOwn(const OmemoStorage::OwnDevice &device)
    {
        if (!storage->setOwnDevice(device))
            return false;
        data.ownDevice = device;
        updateReady();
        return true;
    }

    void updateReady()
    {
        const bool value = data.ownDevice.has_value() && data.ownDevice->id > 0
            && !data.ownDevice->privateIdentityKey.isEmpty() && !data.ownDevice->publicIdentityKey.isEmpty()
            && !data.signedPreKeyPairs.isEmpty() && data.preKeyPairs.size() >= PreKeyMinimum;
        if (ready == value)
            return;
        ready = value;
        emit q->readyChanged(ready);
    }

    static int cryptoRandom(uint8_t *out, size_t length, void *)
    {
        const auto array = QCA::Random::randomArray(static_cast<int>(length));
        if (array.size() != static_cast<int>(length))
            return SG_ERR_UNKNOWN;
        std::memcpy(out, array.constData(), length);
        return SG_SUCCESS;
    }

    static int hmacInit(void **context, const uint8_t *key, size_t keyLength, void *)
    {
        try {
            *context = new QCA::MessageAuthenticationCode(QStringLiteral("hmac(sha256)"),
                                                          QCA::SymmetricKey(bytes(key, keyLength)));
            return SG_SUCCESS;
        } catch (...) {
            return SG_ERR_NOMEM;
        }
    }
    static int hmacUpdate(void *context, const uint8_t *input, size_t length, void *)
    {
        if (!context)
            return SG_ERR_INVAL;
        static_cast<QCA::MessageAuthenticationCode *>(context)->update(QCA::MemoryRegion(bytes(input, length)));
        return SG_SUCCESS;
    }
    static int hmacFinal(void *context, signal_buffer **output, void *)
    {
        if (!context || !output)
            return SG_ERR_INVAL;
        const auto result = static_cast<QCA::MessageAuthenticationCode *>(context)->final().toByteArray();
        *output           = toSignalBuffer(result);
        return *output ? SG_SUCCESS : SG_ERR_NOMEM;
    }
    static void hmacCleanup(void *context, void *) { delete static_cast<QCA::MessageAuthenticationCode *>(context); }

    static int sha512Init(void **context, void *)
    {
        try {
            *context = new QCA::Hash(QStringLiteral("sha512"));
            return SG_SUCCESS;
        } catch (...) {
            return SG_ERR_NOMEM;
        }
    }
    static int sha512Update(void *context, const uint8_t *input, size_t length, void *)
    {
        if (!context)
            return SG_ERR_INVAL;
        static_cast<QCA::Hash *>(context)->update(QCA::MemoryRegion(bytes(input, length)));
        return SG_SUCCESS;
    }
    static int sha512Final(void *context, signal_buffer **output, void *)
    {
        if (!context || !output)
            return SG_ERR_INVAL;
        const auto result = static_cast<QCA::Hash *>(context)->final().toByteArray();
        *output           = toSignalBuffer(result);
        return *output ? SG_SUCCESS : SG_ERR_NOMEM;
    }
    static void sha512Cleanup(void *context, void *) { delete static_cast<QCA::Hash *>(context); }

    static int aes(bool encrypt, signal_buffer **output, int cipherMode, const uint8_t *key, size_t keyLength,
                   const uint8_t *iv, size_t ivLength, const uint8_t *input, size_t inputLength)
    {
        const auto algorithm = cipherName(keyLength);
        if (algorithm.isEmpty() || !output)
            return SG_ERR_INVAL;

        QCA::Cipher::Mode    mode;
        QCA::Cipher::Padding padding;
        switch (cipherMode) {
        case SG_CIPHER_AES_CBC_PKCS5:
            mode    = QCA::Cipher::CBC;
            padding = QCA::Cipher::PKCS7;
            break;
        case SG_CIPHER_AES_CTR_NOPADDING:
            mode    = QCA::Cipher::CTR;
            padding = QCA::Cipher::NoPadding;
            break;
        default:
            return SG_ERR_INVAL;
        }

        QCA::Cipher cipher(algorithm, mode, padding, encrypt ? QCA::Encode : QCA::Decode,
                           QCA::SymmetricKey(bytes(key, keyLength)), QCA::InitializationVector(bytes(iv, ivLength)));
        const auto  result = cipher.process(QCA::MemoryRegion(bytes(input, inputLength)));
        if (!cipher.ok())
            return SG_ERR_UNKNOWN;
        const auto array = result.toByteArray();
        *output          = toSignalBuffer(array);
        return *output ? SG_SUCCESS : SG_ERR_NOMEM;
    }
    static int aesEncrypt(signal_buffer **output, int cipher, const uint8_t *key, size_t keyLength, const uint8_t *iv,
                          size_t ivLength, const uint8_t *input, size_t inputLength, void *)
    {
        return aes(true, output, cipher, key, keyLength, iv, ivLength, input, inputLength);
    }
    static int aesDecrypt(signal_buffer **output, int cipher, const uint8_t *key, size_t keyLength, const uint8_t *iv,
                          size_t ivLength, const uint8_t *input, size_t inputLength, void *)
    {
        return aes(false, output, cipher, key, keyLength, iv, ivLength, input, inputLength);
    }

    static void lockSignal(void *userData) { self(userData)->signalMutex.lock(); }
    static void unlockSignal(void *userData) { self(userData)->signalMutex.unlock(); }
    static void signalLog(int level, const char *message, size_t length, void *userData)
    {
        if (level <= SG_LOG_WARNING) {
            emit self(userData)->q->warning(
                QStringLiteral("libomemo-c: %1").arg(QString::fromUtf8(message, static_cast<qsizetype>(length))));
        }
    }

    static int loadSession(signal_buffer **record, signal_buffer **userRecord, const signal_protocol_address *address,
                           void *userData)
    {
        if (userRecord)
            *userRecord = nullptr;
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(addressName(address)).bare();
        const auto state
            = d->data.devices.value(owner).value(static_cast<uint32_t>(address->device_id)).protocols.value(protocol);
        if (state.session.isEmpty())
            return 0;
        *record = toSignalBuffer(state.session);
        return *record ? 1 : SG_ERR_NOMEM;
    }

    static int getSubDeviceSessions(signal_int_list **sessions, const char *name, size_t nameLength, void *userData)
    {
        auto d        = storeSelf(userData);
        auto protocol = storeProtocol(userData);
        auto list     = signal_int_list_alloc();
        if (!list)
            return SG_ERR_NOMEM;
        const auto owner   = Jid(QString::fromUtf8(name, static_cast<qsizetype>(nameLength))).bare();
        int        count   = 0;
        const auto devices = d->data.devices.value(owner);
        for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
            if (!it.value().protocols.value(protocol).session.isEmpty()) {
                signal_int_list_push_back(list, static_cast<int>(it.key()));
                ++count;
            }
        }
        *sessions = list;
        return count;
    }

    static int storeSession(const signal_protocol_address *address, uint8_t *record, size_t recordLength, uint8_t *,
                            size_t, void *userData)
    {
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(addressName(address)).bare();
        const auto id       = static_cast<uint32_t>(address->device_id);
        auto       device   = d->data.devices.value(owner).value(id);
        auto       state    = device.protocols.value(protocol);
        state.session       = bytes(record, recordLength);
        device.protocols.insert(protocol, state);
        return d->setDevice(owner, id, device) ? SG_SUCCESS : SG_ERR_UNKNOWN;
    }

    static int containsSession(const signal_protocol_address *address, void *userData)
    {
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(addressName(address)).bare();
        return !d->data.devices.value(owner)
                    .value(static_cast<uint32_t>(address->device_id))
                    .protocols.value(protocol)
                    .session.isEmpty();
    }

    static int deleteSession(const signal_protocol_address *address, void *userData)
    {
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(addressName(address)).bare();
        const auto id       = static_cast<uint32_t>(address->device_id);
        auto       device   = d->data.devices.value(owner).value(id);
        auto       state    = device.protocols.value(protocol);
        if (state.session.isEmpty())
            return 0;
        state.session.clear();
        device.protocols.insert(protocol, state);
        return d->setDevice(owner, id, device) ? 1 : SG_ERR_UNKNOWN;
    }

    static int deleteAllSessions(const char *name, size_t nameLength, void *userData)
    {
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(QString::fromUtf8(name, static_cast<qsizetype>(nameLength))).bare();
        int        count    = 0;
        auto       devices  = d->data.devices.value(owner);
        for (auto it = devices.begin(); it != devices.end(); ++it) {
            auto state = it->protocols.value(protocol);
            if (!state.session.isEmpty()) {
                state.session.clear();
                it->protocols.insert(protocol, state);
                if (!d->setDevice(owner, it.key(), *it))
                    return SG_ERR_UNKNOWN;
                ++count;
            }
        }
        return count;
    }

    static int loadPreKey(signal_buffer **record, uint32_t id, void *userData)
    {
        const auto value = storeSelf(userData)->data.preKeyPairs.value(id);
        if (value.isEmpty())
            return SG_ERR_INVALID_KEY_ID;
        *record = toSignalBuffer(value);
        return *record ? SG_SUCCESS : SG_ERR_NOMEM;
    }
    static int storePreKey(uint32_t id, uint8_t *record, size_t length, void *userData)
    {
        auto                        d = storeSelf(userData);
        QHash<uint32_t, QByteArray> one;
        one.insert(id, bytes(record, length));
        if (!d->storage->addPreKeyPairs(one))
            return SG_ERR_UNKNOWN;
        d->data.preKeyPairs.insert(id, one.value(id));
        return SG_SUCCESS;
    }
    static int containsPreKey(uint32_t id, void *userData)
    {
        return storeSelf(userData)->data.preKeyPairs.contains(id);
    }
    static int removePreKey(uint32_t id, void *userData)
    {
        auto d = storeSelf(userData);
        if (!d->storage->removePreKeyPair(id))
            return SG_ERR_UNKNOWN;
        d->data.preKeyPairs.remove(id);
        d->updateReady();
        if (d->data.preKeyPairs.size() < PreKeyTarget) {
            QTimer::singleShot(0, d->q, [q = d->q]() {
                QString error;
                if (!q->d->ensurePreKeys(&error)) {
                    emit q->warning(QStringLiteral("OMEMO pre-key replenishment failed: %1").arg(error));
                    return;
                }
                auto job = q->publishOwnBundle();
                QObject::connect(job, &EncryptionJob::finished, q, [q, job]() {
                    if (!job->success())
                        emit q->warning(QStringLiteral("OMEMO bundle republish failed: %1").arg(job->errorString()));
                    job->deleteLater();
                });
            });
        }
        return SG_SUCCESS;
    }

    static int loadSignedPreKey(signal_buffer **record, uint32_t id, void *userData)
    {
        const auto value = storeSelf(userData)->data.signedPreKeyPairs.value(id).data;
        if (value.isEmpty())
            return SG_ERR_INVALID_KEY_ID;
        *record = toSignalBuffer(value);
        return *record ? SG_SUCCESS : SG_ERR_NOMEM;
    }
    static int storeSignedPreKey(uint32_t id, uint8_t *record, size_t length, void *userData)
    {
        auto                           d = storeSelf(userData);
        OmemoStorage::SignedPreKeyPair pair { QDateTime::currentDateTimeUtc(), bytes(record, length) };
        if (!d->storage->addSignedPreKeyPair(id, pair))
            return SG_ERR_UNKNOWN;
        d->data.signedPreKeyPairs.insert(id, pair);
        d->updateReady();
        return SG_SUCCESS;
    }
    static int containsSignedPreKey(uint32_t id, void *userData)
    {
        return storeSelf(userData)->data.signedPreKeyPairs.contains(id);
    }
    static int removeSignedPreKey(uint32_t id, void *userData)
    {
        auto d = storeSelf(userData);
        if (!d->storage->removeSignedPreKeyPair(id))
            return SG_ERR_UNKNOWN;
        d->data.signedPreKeyPairs.remove(id);
        d->updateReady();
        return SG_SUCCESS;
    }

    static int getIdentityKeyPair(signal_buffer **publicData, signal_buffer **privateData, void *userData)
    {
        auto d = storeSelf(userData);
        if (!d->data.ownDevice || d->data.ownDevice->publicIdentityKey.isEmpty()
            || d->data.ownDevice->privateIdentityKey.isEmpty()) {
            return SG_ERR_INVALID_KEY_ID;
        }
        *publicData  = toSignalBuffer(d->data.ownDevice->publicIdentityKey);
        *privateData = toSignalBuffer(d->data.ownDevice->privateIdentityKey);
        if (!*publicData || !*privateData) {
            if (*publicData)
                signal_buffer_free(*publicData);
            if (*privateData)
                signal_buffer_bzero_free(*privateData);
            *publicData  = nullptr;
            *privateData = nullptr;
            return SG_ERR_NOMEM;
        }
        return SG_SUCCESS;
    }
    static int getRegistrationId(void *userData, uint32_t *id)
    {
        auto d = storeSelf(userData);
        if (!d->data.ownDevice || d->data.ownDevice->id == 0)
            return SG_ERR_INVALID_KEY_ID;
        *id = d->data.ownDevice->id;
        return SG_SUCCESS;
    }
    static int saveIdentity(const signal_protocol_address *address, uint8_t *keyData, size_t keyLength, void *userData)
    {
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(addressName(address)).bare();
        const auto id       = static_cast<uint32_t>(address->device_id);
        auto       device   = d->data.devices.value(owner).value(id);
        auto       state    = device.protocols.value(protocol);
        if (!keyData) {
            state.keyId.clear();
            device.protocols.insert(protocol, state);
            return d->setDevice(owner, id, device) ? SG_SUCCESS : SG_ERR_UNKNOWN;
        }
        const QByteArray incoming = bytes(keyData, keyLength);
        if (!state.keyId.isEmpty() && state.keyId != incoming)
            return SG_ERR_UNTRUSTED_IDENTITY;
        state.keyId = incoming;
        device.protocols.insert(protocol, state);
        return d->setDevice(owner, id, device) ? SG_SUCCESS : SG_ERR_UNKNOWN;
    }
    static int isTrustedIdentity(const signal_protocol_address *address, uint8_t *keyData, size_t keyLength,
                                 void *userData)
    {
        auto       d        = storeSelf(userData);
        const auto protocol = storeProtocol(userData);
        const auto owner    = Jid(addressName(address)).bare();
        const auto existing = d->data.devices.value(owner)
                                  .value(static_cast<uint32_t>(address->device_id))
                                  .protocols.value(protocol)
                                  .keyId;
        return existing.isEmpty() || existing == bytes(keyData, keyLength);
    }

    bool initSignal()
    {
        const QStringList required {
            QStringLiteral("hmac(sha256)"),
            QStringLiteral("sha512"),
            QCA::Cipher::withAlgorithms(QStringLiteral("aes128"), QCA::Cipher::CBC, QCA::Cipher::PKCS7),
            QCA::Cipher::withAlgorithms(QStringLiteral("aes192"), QCA::Cipher::CBC, QCA::Cipher::PKCS7),
            QCA::Cipher::withAlgorithms(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7),
            QCA::Cipher::withAlgorithms(QStringLiteral("aes128"), QCA::Cipher::CTR, QCA::Cipher::NoPadding),
            QCA::Cipher::withAlgorithms(QStringLiteral("aes192"), QCA::Cipher::CTR, QCA::Cipher::NoPadding),
            QCA::Cipher::withAlgorithms(QStringLiteral("aes256"), QCA::Cipher::CTR, QCA::Cipher::NoPadding),
        };
        if (!QCA::isSupported(required)) {
            emit q->warning(QStringLiteral("OMEMO cannot start: required QCA algorithms are unavailable"));
            return false;
        }

        supportedProtocols = OmemoProtocol::Omemo2;
        const auto legacyCipher
            = QCA::Cipher::withAlgorithms(QStringLiteral("aes128"), QCA::Cipher::GCM, QCA::Cipher::NoPadding);
        if (QCA::Cipher::supportedTypes().contains(legacyCipher)) {
            supportedProtocols |= OmemoProtocol::Legacy;
        } else {
            emit q->warning(QStringLiteral("Legacy OMEMO is disabled: QCA AES-128-GCM is unavailable"));
        }

        if (signal_context_create(&signalContext, this) != SG_SUCCESS)
            return false;
        signal_crypto_provider cryptoProvider { &cryptoRandom,  &hmacInit,   &hmacUpdate,   &hmacFinal,
                                                &hmacCleanup,   &sha512Init, &sha512Update, &sha512Final,
                                                &sha512Cleanup, &aesEncrypt, &aesDecrypt,   this };
        if (signal_context_set_crypto_provider(signalContext, &cryptoProvider) != SG_SUCCESS
            || signal_context_set_locking_functions(signalContext, &lockSignal, &unlockSignal) != SG_SUCCESS
            || signal_context_set_log_function(signalContext, &signalLog) != SG_SUCCESS) {
            return false;
        }

        signalStores[0].owner    = this;
        signalStores[0].protocol = OmemoProtocol::Legacy;
        signalStores[1].owner    = this;
        signalStores[1].protocol = OmemoProtocol::Omemo2;
        for (auto &signalStore : signalStores) {
            if (signal_protocol_store_context_create(&signalStore.context, signalContext) != SG_SUCCESS)
                return false;

            signal_protocol_session_store        sessionStore { &loadSession,     &getSubDeviceSessions, &storeSession,
                                                         &containsSession, &deleteSession,        &deleteAllSessions,
                                                         nullptr,          &signalStore };
            signal_protocol_pre_key_store        preKeyStore { &loadPreKey,   &storePreKey, &containsPreKey,
                                                        &removePreKey, nullptr,      &signalStore };
            signal_protocol_signed_pre_key_store signedStore {
                &loadSignedPreKey, &storeSignedPreKey, &containsSignedPreKey, &removeSignedPreKey, nullptr, &signalStore
            };
            signal_protocol_identity_key_store identityStore {
                &getIdentityKeyPair, &getRegistrationId, &saveIdentity, &isTrustedIdentity, nullptr, &signalStore
            };
            if (signal_protocol_store_context_set_session_store(signalStore.context, &sessionStore) != SG_SUCCESS
                || signal_protocol_store_context_set_pre_key_store(signalStore.context, &preKeyStore) != SG_SUCCESS
                || signal_protocol_store_context_set_signed_pre_key_store(signalStore.context, &signedStore)
                    != SG_SUCCESS
                || signal_protocol_store_context_set_identity_key_store(signalStore.context, &identityStore)
                    != SG_SUCCESS) {
                return false;
            }
        }
        updateReady();
        return true;
    }

    QByteArray wireIdentityFromStored(const QByteArray &stored, OmemoProtocol protocol = OmemoProtocol::Omemo2) const
    {
        if (stored.isEmpty())
            return {};
        ec_public_key *key = nullptr;
        if (curve_decode_point(&key, reinterpret_cast<const uint8_t *>(stored.constData()),
                               static_cast<size_t>(stored.size()), signalContext)
            != SG_SUCCESS) {
            return {};
        }

        signal_buffer *buffer = nullptr;
        if (protocol == OmemoProtocol::Legacy) {
            if (ec_public_key_serialize(&buffer, key) != SG_SUCCESS)
                buffer = nullptr;
        } else {
            buffer = ec_public_key_get_ed(key);
        }
        const QByteArray result = fromSignalBuffer(buffer);
        if (buffer)
            signal_buffer_free(buffer);
        SIGNAL_UNREF(key);
        return result;
    }

    QByteArray storedIdentityFromWire(const QByteArray &wire, OmemoProtocol protocol) const
    {
        if (wire.isEmpty())
            return {};
        ec_public_key *key          = nullptr;
        const int      decodeResult = protocol == OmemoProtocol::Legacy
                 ? curve_decode_point(&key, reinterpret_cast<const uint8_t *>(wire.constData()),
                                      static_cast<size_t>(wire.size()), signalContext)
                 : curve_decode_point_ed(&key, reinterpret_cast<const uint8_t *>(wire.constData()),
                                         static_cast<size_t>(wire.size()), signalContext);
        if (decodeResult != SG_SUCCESS)
            return {};

        signal_buffer *buffer = nullptr;
        const int      result = ec_public_key_serialize(&buffer, key);
        SIGNAL_UNREF(key);
        if (result != SG_SUCCESS)
            return {};
        const QByteArray serialized = fromSignalBuffer(buffer);
        signal_buffer_free(buffer);
        return serialized;
    }

    QByteArray ownWireIdentity(OmemoProtocol protocol = OmemoProtocol::Omemo2) const
    {
        return data.ownDevice ? wireIdentityFromStored(data.ownDevice->publicIdentityKey, protocol) : QByteArray();
    }

    QByteArray canonicalIdentityFromWire(const QByteArray &wire, OmemoProtocol protocol) const
    {
        const auto stored = storedIdentityFromWire(wire, protocol);
        return wireIdentityFromStored(stored, OmemoProtocol::Omemo2);
    }

    EncryptionTrustLevel trustLevel(const QString &owner, const QByteArray &wireIdentity) const
    {
        if (owner.isEmpty() || wireIdentity.isEmpty())
            return EncryptionTrustLevel::Undecided;
        return trustStorage->trustLevel(OmemoEncryption::methodId(), Jid(owner).bare(), wireIdentity);
    }

    bool identityAccepted(const QString &owner, const QByteArray &wireIdentity) const
    {
        return acceptedTrust.testFlag(trustLevel(owner, wireIdentity));
    }

    bool createOwnIdentity(const QString &label, QString *error)
    {
        uint32_t deviceId = 0;
        if (signal_protocol_key_helper_generate_registration_id(&deviceId, 1, signalContext) != SG_SUCCESS
            || deviceId == 0 || deviceId > static_cast<uint32_t>(INT32_MAX)) {
            if (error)
                *error = QStringLiteral("Could not generate an OMEMO device id");
            return false;
        }

        ratchet_identity_key_pair *identity = nullptr;
        if (signal_protocol_key_helper_generate_identity_key_pair(&identity, signalContext) != SG_SUCCESS) {
            if (error)
                *error = QStringLiteral("Could not generate an OMEMO identity key");
            return false;
        }

        signal_buffer *publicBuffer  = nullptr;
        signal_buffer *privateBuffer = nullptr;
        const int pubResult = ec_public_key_serialize(&publicBuffer, ratchet_identity_key_pair_get_public(identity));
        const int privResult
            = ec_private_key_serialize(&privateBuffer, ratchet_identity_key_pair_get_private(identity));
        OmemoStorage::OwnDevice own;
        own.id                   = deviceId;
        own.label                = label;
        own.publicIdentityKey    = pubResult == SG_SUCCESS ? fromSignalBuffer(publicBuffer) : QByteArray();
        own.privateIdentityKey   = privResult == SG_SUCCESS ? fromSignalBuffer(privateBuffer) : QByteArray();
        own.latestSignedPreKeyId = 1;
        own.latestPreKeyId       = 1;
        signal_buffer_free(publicBuffer);
        signal_buffer_bzero_free(privateBuffer);
        SIGNAL_UNREF(identity);

        if (own.publicIdentityKey.isEmpty() || own.privateIdentityKey.isEmpty() || !setOwn(own)) {
            if (error)
                *error = QStringLiteral("Could not persist the OMEMO identity key");
            return false;
        }
        return true;
    }

    bool ensureSignedPreKey(QString *error)
    {
        if (!data.ownDevice)
            return false;
        if (data.signedPreKeyPairs.contains(data.ownDevice->latestSignedPreKeyId))
            return true;

        ratchet_identity_key_pair *identity = nullptr;
        int result = signal_protocol_identity_get_key_pair(store(OmemoProtocol::Omemo2).context, &identity);
        if (result != SG_SUCCESS) {
            if (error)
                *error = signalErrorString(QStringLiteral("load local identity"), result);
            return false;
        }

        const uint32_t          id           = std::max<uint32_t>(1, data.ownDevice->latestSignedPreKeyId);
        session_signed_pre_key *signedPreKey = nullptr;
        result                               = signal_protocol_key_helper_generate_signed_pre_key(
            &signedPreKey, identity, id, static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()), signalContext);
        SIGNAL_UNREF(identity);
        if (result != SG_SUCCESS) {
            if (error)
                *error = signalErrorString(QStringLiteral("generate signed pre-key"), result);
            return false;
        }

        signal_buffer *serialized = nullptr;
        result                    = session_signed_pre_key_serialize(&serialized, signedPreKey);
        SIGNAL_UNREF(signedPreKey);
        if (result != SG_SUCCESS || !serialized) {
            if (error)
                *error = signalErrorString(QStringLiteral("serialize signed pre-key"), result);
            return false;
        }
        OmemoStorage::SignedPreKeyPair pair { QDateTime::currentDateTimeUtc(), fromSignalBuffer(serialized) };
        signal_buffer_free(serialized);
        if (!storage->addSignedPreKeyPair(id, pair)) {
            if (error)
                *error = QStringLiteral("Could not persist the OMEMO signed pre-key");
            return false;
        }
        data.signedPreKeyPairs.insert(id, pair);
        auto own                 = *data.ownDevice;
        own.latestSignedPreKeyId = id;
        if (!setOwn(own)) {
            if (error)
                *error = QStringLiteral("Could not persist signed pre-key metadata");
            return false;
        }
        return true;
    }

    bool ensurePreKeys(QString *error)
    {
        if (!data.ownDevice)
            return false;
        if (data.preKeyPairs.size() >= PreKeyTarget)
            return true;

        const uint32_t count = static_cast<uint32_t>(PreKeyTarget - data.preKeyPairs.size());
        uint32_t       start = std::max<uint32_t>(1, data.ownDevice->latestPreKeyId + 1);
        if (start > PRE_KEY_MEDIUM_MAX_VALUE || count > PRE_KEY_MEDIUM_MAX_VALUE - start + 1)
            start = 1;

        // Avoid overwriting a live local pre-key when IDs eventually wrap.
        while (data.preKeyPairs.contains(start) && start < PRE_KEY_MEDIUM_MAX_VALUE)
            ++start;

        signal_protocol_key_helper_pre_key_list_node *head = nullptr;
        int result = signal_protocol_key_helper_generate_pre_keys(&head, start, count, signalContext);
        if (result != SG_SUCCESS) {
            if (error)
                *error = signalErrorString(QStringLiteral("generate pre-keys"), result);
            return false;
        }

        QHash<uint32_t, QByteArray> generated;
        uint32_t                    last = start;
        for (auto node = head; node; node = signal_protocol_key_helper_key_list_next(node)) {
            auto           preKey     = signal_protocol_key_helper_key_list_element(node);
            signal_buffer *serialized = nullptr;
            if (session_pre_key_serialize(&serialized, preKey) != SG_SUCCESS || !serialized) {
                signal_protocol_key_helper_key_list_free(head);
                if (error)
                    *error = QStringLiteral("Could not serialize an OMEMO pre-key");
                return false;
            }
            const auto id = session_pre_key_get_id(preKey);
            generated.insert(id, fromSignalBuffer(serialized));
            signal_buffer_free(serialized);
            last = id;
        }
        signal_protocol_key_helper_key_list_free(head);

        if (generated.isEmpty() || !storage->addPreKeyPairs(generated)) {
            if (error)
                *error = QStringLiteral("Could not persist OMEMO pre-keys");
            return false;
        }
        for (auto it = generated.cbegin(); it != generated.cend(); ++it)
            data.preKeyPairs.insert(it.key(), it.value());
        auto own           = *data.ownDevice;
        own.latestPreKeyId = last;
        if (!setOwn(own)) {
            if (error)
                *error = QStringLiteral("Could not persist pre-key metadata");
            return false;
        }
        updateReady();
        return true;
    }

    bool clearLocalKeyMaterial(QString *error)
    {
        const auto signedIds = data.signedPreKeyPairs.keys();
        for (const auto id : signedIds) {
            if (!storage->removeSignedPreKeyPair(id)) {
                if (error)
                    *error = QStringLiteral("Could not remove an old OMEMO signed pre-key");
                return false;
            }
        }
        const auto preKeyIds = data.preKeyPairs.keys();
        for (const auto id : preKeyIds) {
            if (!storage->removePreKeyPair(id)) {
                if (error)
                    *error = QStringLiteral("Could not remove an old OMEMO pre-key");
                return false;
            }
        }
        if (!storage->setOwnDevice(std::nullopt)) {
            if (error)
                *error = QStringLiteral("Could not clear the old OMEMO identity");
            return false;
        }
        data.signedPreKeyPairs.clear();
        data.preKeyPairs.clear();
        data.ownDevice.reset();
        updateReady();
        return true;
    }

    bool ensureLocalKeyMaterial(const QString &deviceLabel, QString *error)
    {
        if (!data.ownDevice) {
            if (!createOwnIdentity(deviceLabel, error))
                return false;
        } else if (!deviceLabel.isNull() && data.ownDevice->label != deviceLabel) {
            auto own  = *data.ownDevice;
            own.label = deviceLabel;
            if (!setOwn(own)) {
                if (error)
                    *error = QStringLiteral("Could not persist the OMEMO device label");
                return false;
            }
        }
        return ensureSignedPreKey(error) && ensurePreKeys(error);
    }

    QByteArray publicKeyWire(const ec_public_key *key, OmemoProtocol protocol) const
    {
        if (!key)
            return {};
        signal_buffer *buffer = nullptr;
        if (protocol == OmemoProtocol::Legacy) {
            if (ec_public_key_serialize(&buffer, const_cast<ec_public_key *>(key)) != SG_SUCCESS)
                return {};
        } else {
            buffer = ec_public_key_get_mont(const_cast<ec_public_key *>(key));
        }
        const auto result = fromSignalBuffer(buffer);
        if (buffer)
            signal_buffer_free(buffer);
        return result;
    }

    std::optional<Bundle> ownBundle(OmemoProtocol protocol, QString *error) const
    {
        if (!data.ownDevice || data.signedPreKeyPairs.isEmpty() || data.preKeyPairs.isEmpty()) {
            if (error)
                *error = QStringLiteral("Local OMEMO key material is incomplete");
            return std::nullopt;
        }

        Bundle bundle;
        bundle.protocol       = protocol;
        bundle.identityKey    = ownWireIdentity(protocol);
        bundle.signedPreKeyId = data.ownDevice->latestSignedPreKeyId;

        const auto              signedData   = data.signedPreKeyPairs.value(bundle.signedPreKeyId).data;
        session_signed_pre_key *signedPreKey = nullptr;
        int                     result       = session_signed_pre_key_deserialize(&signedPreKey,
                                                                                  reinterpret_cast<const uint8_t *>(signedData.constData()),
                                                                                  static_cast<size_t>(signedData.size()), signalContext);
        if (result != SG_SUCCESS) {
            if (error)
                *error = signalErrorString(QStringLiteral("deserialize signed pre-key"), result);
            return std::nullopt;
        }
        const auto signedPair = session_signed_pre_key_get_key_pair(signedPreKey);
        bundle.signedPreKey   = signedPair ? publicKeyWire(ec_key_pair_get_public(signedPair), protocol) : QByteArray();
        if (protocol == OmemoProtocol::Legacy) {
            bundle.signedPreKeySignature = bytes(session_signed_pre_key_get_signature(signedPreKey),
                                                 session_signed_pre_key_get_signature_len(signedPreKey));
        } else {
            bundle.signedPreKeySignature = bytes(session_signed_pre_key_get_signature_omemo(signedPreKey),
                                                 session_signed_pre_key_get_signature_omemo_len(signedPreKey));
        }
        SIGNAL_UNREF(signedPreKey);

        QList<uint32_t> ids = data.preKeyPairs.keys();
        std::sort(ids.begin(), ids.end());
        for (const auto id : ids) {
            const auto       record = data.preKeyPairs.value(id);
            session_pre_key *preKey = nullptr;
            result = session_pre_key_deserialize(&preKey, reinterpret_cast<const uint8_t *>(record.constData()),
                                                 static_cast<size_t>(record.size()), signalContext);
            if (result != SG_SUCCESS)
                continue;
            const auto pair = session_pre_key_get_key_pair(preKey);
            const auto wire = pair ? publicKeyWire(ec_key_pair_get_public(pair), protocol) : QByteArray();
            SIGNAL_UNREF(preKey);
            if (!wire.isEmpty())
                bundle.preKeys.append(qMakePair(id, wire));
        }

        const int expectedKeySize = protocol == OmemoProtocol::Legacy ? 33 : 32;
        if (bundle.identityKey.size() != expectedKeySize || bundle.signedPreKey.size() != expectedKeySize
            || bundle.signedPreKeySignature.size() != CURVE_SIGNATURE_LEN || bundle.preKeys.isEmpty()) {
            if (error)
                *error
                    = QStringLiteral("Local %1 OMEMO bundle contains invalid key material").arg(protocolName(protocol));
            return std::nullopt;
        }
        return bundle;
    }

    static std::optional<Bundle> parseBundle(const QDomElement &element, OmemoProtocol protocol, QString *error)
    {
        const auto ns = protocolNamespace(protocol);
        if (element.isNull() || localName(element) != QLatin1String("bundle") || element.namespaceURI() != ns) {
            if (error)
                *error = QStringLiteral("OMEMO PEP item is not a %1 bundle").arg(protocolName(protocol));
            return std::nullopt;
        }

        Bundle bundle;
        bundle.protocol = protocol;
        bool        ok  = false;
        QDomElement prekeys;
        const auto  malformed = [error, protocol]() -> std::optional<Bundle> {
            if (error)
                *error = QStringLiteral("%1 OMEMO bundle contains malformed key material").arg(protocolName(protocol));
            return std::nullopt;
        };
        const auto parseKey = [&ok](const QDomElement &keyElement, int expectedSize, QByteArray *key) {
            *key = base64Element(keyElement, &ok);
            return ok && key->size() == expectedSize;
        };
        if (protocol == OmemoProtocol::Legacy) {
            const auto spk  = directChildNS(element, QStringLiteral("signedPreKeyPublic"), ns);
            const auto spks = directChildNS(element, QStringLiteral("signedPreKeySignature"), ns);
            const auto ik   = directChildNS(element, QStringLiteral("identityKey"), ns);
            prekeys         = directChildNS(element, QStringLiteral("prekeys"), ns);
            if (spk.isNull() || spks.isNull() || ik.isNull() || prekeys.isNull()
                || !parsePositiveInt32(spk.attribute(QStringLiteral("signedPreKeyId")), &bundle.signedPreKeyId)) {
                if (error)
                    *error = QStringLiteral("Legacy OMEMO bundle is missing mandatory fields");
                return std::nullopt;
            }
            if (!parseKey(spk, 33, &bundle.signedPreKey)
                || !parseKey(spks, CURVE_SIGNATURE_LEN, &bundle.signedPreKeySignature)
                || !parseKey(ik, 33, &bundle.identityKey)) {
                return malformed();
            }
            for (const auto &pk : directChildrenNS(prekeys, QStringLiteral("preKeyPublic"), ns)) {
                uint32_t id = 0;
                if (!parsePositiveInt32(pk.attribute(QStringLiteral("preKeyId")), &id))
                    return malformed();
                QByteArray key;
                if (!parseKey(pk, 33, &key))
                    return malformed();
                bundle.preKeys.append(qMakePair(id, key));
            }
        } else {
            const auto spk  = directChildNS(element, QStringLiteral("spk"), ns);
            const auto spks = directChildNS(element, QStringLiteral("spks"), ns);
            const auto ik   = directChildNS(element, QStringLiteral("ik"), ns);
            prekeys         = directChildNS(element, QStringLiteral("prekeys"), ns);
            if (spk.isNull() || spks.isNull() || ik.isNull() || prekeys.isNull()
                || !parsePositiveInt32(spk.attribute(QStringLiteral("id")), &bundle.signedPreKeyId)) {
                if (error)
                    *error = QStringLiteral("OMEMO 2 bundle is missing mandatory fields");
                return std::nullopt;
            }
            if (!parseKey(spk, 32, &bundle.signedPreKey)
                || !parseKey(spks, CURVE_SIGNATURE_LEN, &bundle.signedPreKeySignature)
                || !parseKey(ik, 32, &bundle.identityKey)) {
                return malformed();
            }
            for (const auto &pk : directChildrenNS(prekeys, QStringLiteral("pk"), ns)) {
                uint32_t id = 0;
                if (!parsePositiveInt32(pk.attribute(QStringLiteral("id")), &id))
                    return malformed();
                QByteArray key;
                if (!parseKey(pk, 32, &key))
                    return malformed();
                bundle.preKeys.append(qMakePair(id, key));
            }
        }
        if (bundle.preKeys.isEmpty())
            return malformed();
        return bundle;
    }

    bool verifyDeviceLabel(const QByteArray &wireIdentity, const QString &label, const QByteArray &signature) const
    {
        if (label.isEmpty() || signature.size() != CURVE_SIGNATURE_LEN || wireIdentity.size() != 32)
            return false;
        ec_public_key *identity = nullptr;
        if (curve_decode_point_ed(&identity, reinterpret_cast<const uint8_t *>(wireIdentity.constData()),
                                  static_cast<size_t>(wireIdentity.size()), signalContext)
            != SG_SUCCESS) {
            return false;
        }
        const auto message = label.toUtf8();
        const int  result  = curve_verify_signature(
            identity, reinterpret_cast<const uint8_t *>(message.constData()), static_cast<size_t>(message.size()),
            reinterpret_cast<const uint8_t *>(signature.constData()), static_cast<size_t>(signature.size()));
        SIGNAL_UNREF(identity);
        return result == 1;
    }

    QByteArray signOwnLabel() const
    {
        if (!data.ownDevice || data.ownDevice->label.isEmpty())
            return {};
        ec_private_key *privateKey = nullptr;
        const auto      stored     = data.ownDevice->privateIdentityKey;
        if (curve_decode_private_point(&privateKey, reinterpret_cast<const uint8_t *>(stored.constData()),
                                       static_cast<size_t>(stored.size()), signalContext)
            != SG_SUCCESS) {
            return {};
        }
        signal_buffer *signature = nullptr;
        const auto     label     = data.ownDevice->label.toUtf8();
        const int      result    = curve_calculate_signature(signalContext, &signature, privateKey,
                                                             reinterpret_cast<const uint8_t *>(label.constData()),
                                                             static_cast<size_t>(label.size()));
        SIGNAL_UNREF(privateKey);
        if (result != SG_SUCCESS)
            return {};
        const auto output = fromSignalBuffer(signature);
        signal_buffer_free(signature);
        return output;
    }

    static bool protocolActive(const OmemoStorage::Device &device, OmemoProtocol protocol)
    {
        const auto it = device.protocols.constFind(protocol);
        return it != device.protocols.cend() && !it->removalFromDeviceListDate.isValid();
    }

    static bool deviceActive(const OmemoStorage::Device &device)
    {
        for (auto it = device.protocols.cbegin(); it != device.protocols.cend(); ++it) {
            if (!it->removalFromDeviceListDate.isValid())
                return true;
        }
        return false;
    }

    bool mergeDeviceList(const QString &owner, OmemoProtocol protocol, const QDomElement &devicesElement,
                         QString *error)
    {
        const auto ns       = protocolNamespace(protocol);
        const auto rootName = protocol == OmemoProtocol::Legacy ? QStringLiteral("list") : QStringLiteral("devices");
        if (devicesElement.isNull() || localName(devicesElement) != rootName || devicesElement.namespaceURI() != ns) {
            if (error)
                *error = QStringLiteral("%1 OMEMO device-list item has invalid XML").arg(protocolName(protocol));
            return false;
        }

        const QString bare = Jid(owner).bare();
        if (bare.isEmpty()) {
            if (error)
                *error = QStringLiteral("OMEMO device-list owner is invalid");
            return false;
        }
        auto           previous = data.devices.value(bare);
        auto           next     = previous;
        QSet<uint32_t> activeIds;

        for (const auto &deviceElement : directChildrenNS(devicesElement, QStringLiteral("device"), ns)) {
            uint32_t id = 0;
            if (!parsePositiveInt32(deviceElement.attribute(QStringLiteral("id")), &id))
                continue;
            activeIds.insert(id);
            auto device                     = previous.value(id);
            auto state                      = device.protocols.value(protocol);
            state.removalFromDeviceListDate = {};
            device.protocols.insert(protocol, state);

            const QString label = deviceElement.attribute(QStringLiteral("label"));
            if (protocol == OmemoProtocol::Omemo2) {
                const auto signature
                    = QByteArray::fromBase64(deviceElement.attribute(QStringLiteral("labelsig")).toLatin1(),
                                             QByteArray::AbortOnBase64DecodingErrors);
                if (!label.isEmpty() && signature.size() == CURVE_SIGNATURE_LEN) {
                    if (device.label != label || device.labelSignature != signature) {
                        device.label          = label;
                        device.labelSignature = signature;
                        device.labelVerified  = false;
                    }
                } else {
                    device.label.clear();
                    device.labelSignature.clear();
                    device.labelVerified = false;
                }
            } else if (!device.labelVerified && !label.isEmpty()) {
                device.label = label;
                device.labelSignature.clear();
            }
            next.insert(id, device);
        }

        const auto now = QDateTime::currentDateTimeUtc();
        for (auto it = next.begin(); it != next.end(); ++it) {
            auto stateIt = it->protocols.find(protocol);
            if (stateIt == it->protocols.end() || activeIds.contains(it.key())
                || stateIt->removalFromDeviceListDate.isValid()) {
                continue;
            }
            const bool wasActive               = deviceActive(*it);
            stateIt->removalFromDeviceListDate = now;
            if (wasActive && !deviceActive(*it))
                emit q->deviceRemoved(Jid(bare), it.key());
        }
        for (auto it = next.cbegin(); it != next.cend(); ++it) {
            if (!storage->addDevice(bare, it.key(), it.value())) {
                if (error)
                    *error = QStringLiteral("Could not persist the OMEMO device list");
                return false;
            }
        }
        data.devices.insert(bare, next);
        fetchedDeviceLists.insert(fetchedListKey(bare, protocol));
        for (const auto id : activeIds)
            emit q->deviceChanged(Jid(bare), id);
        return true;
    }

    QDomDocument makeDeviceListDocument(OmemoProtocol                  protocol,
                                        const std::optional<uint32_t> &excludedDeviceId = {}) const
    {
        QDomDocument document;
        const auto   ns       = protocolNamespace(protocol);
        const auto   rootName = protocol == OmemoProtocol::Legacy ? QStringLiteral("list") : QStringLiteral("devices");
        auto         devices  = document.createElementNS(ns, rootName);
        document.appendChild(devices);
        if (!data.ownDevice)
            return document;

        const QString  ownBare = client->jid().bare();
        const auto     known   = data.devices.value(ownBare);
        QSet<uint32_t> ids;
        ids.insert(data.ownDevice->id);
        for (auto it = known.cbegin(); it != known.cend(); ++it) {
            if (protocolActive(it.value(), protocol))
                ids.insert(it.key());
        }
        QList<uint32_t> sorted = ids.values();
        std::sort(sorted.begin(), sorted.end());
        const auto ownSignature = protocol == OmemoProtocol::Omemo2 ? signOwnLabel() : QByteArray();
        for (const auto id : sorted) {
            if (excludedDeviceId && id == *excludedDeviceId)
                continue;
            auto device = document.createElementNS(ns, QStringLiteral("device"));
            device.setAttribute(QStringLiteral("id"), QString::number(id));
            if (id == data.ownDevice->id && !data.ownDevice->label.isEmpty()) {
                device.setAttribute(QStringLiteral("label"), data.ownDevice->label);
                if (protocol == OmemoProtocol::Omemo2 && ownSignature.size() == CURVE_SIGNATURE_LEN)
                    device.setAttribute(QStringLiteral("labelsig"), QString::fromLatin1(ownSignature.toBase64()));
            } else {
                const auto stored = known.value(id);
                if (protocol == OmemoProtocol::Legacy) {
                    if (!stored.label.isEmpty())
                        device.setAttribute(QStringLiteral("label"), stored.label);
                } else if (stored.labelVerified && !stored.label.isEmpty()
                           && stored.labelSignature.size() == CURVE_SIGNATURE_LEN) {
                    device.setAttribute(QStringLiteral("label"), stored.label);
                    device.setAttribute(QStringLiteral("labelsig"),
                                        QString::fromLatin1(stored.labelSignature.toBase64()));
                }
            }
            devices.appendChild(device);
        }
        return document;
    }

    bool markOwnDeviceRetired(uint32_t deviceId, const QList<OmemoProtocol> &protocols, QString *error)
    {
        const auto ownBare = client->jid().bare();
        auto       devices = data.devices.value(ownBare);
        auto       it      = devices.find(deviceId);
        if (it == devices.end()) {
            if (error)
                *error = QStringLiteral("OMEMO device is not known for this account");
            return false;
        }

        auto       device    = it.value();
        bool       wasActive = deviceActive(device);
        const auto now       = QDateTime::currentDateTimeUtc();
        for (const auto protocol : protocols) {
            auto state                      = device.protocols.value(protocol);
            state.removalFromDeviceListDate = now;
            device.protocols.insert(protocol, state);
        }
        if (!storage->addDevice(ownBare, deviceId, device)) {
            if (error)
                *error = QStringLiteral("Could not persist the retired OMEMO device");
            return false;
        }
        it.value() = device;
        data.devices.insert(ownBare, devices);
        if (wasActive && !deviceActive(device))
            emit q->deviceRemoved(Jid(ownBare), deviceId);
        else
            emit q->deviceChanged(Jid(ownBare), deviceId);
        return true;
    }

    QDomDocument makeBundleDocument(OmemoProtocol protocol, QString *error) const
    {
        QDomDocument document;
        const auto   bundleValue = ownBundle(protocol, error);
        if (!bundleValue)
            return document;
        const auto &value  = *bundleValue;
        const auto  ns     = protocolNamespace(protocol);
        auto        bundle = document.createElementNS(ns, QStringLiteral("bundle"));
        document.appendChild(bundle);

        if (protocol == OmemoProtocol::Legacy) {
            auto spk = appendBase64(document, bundle, QStringLiteral("signedPreKeyPublic"), value.signedPreKey, ns);
            spk.setAttribute(QStringLiteral("signedPreKeyId"), QString::number(value.signedPreKeyId));
            appendBase64(document, bundle, QStringLiteral("signedPreKeySignature"), value.signedPreKeySignature, ns);
            appendBase64(document, bundle, QStringLiteral("identityKey"), value.identityKey, ns);
            auto prekeys = document.createElementNS(ns, QStringLiteral("prekeys"));
            bundle.appendChild(prekeys);
            for (const auto &entry : value.preKeys) {
                auto pk = appendBase64(document, prekeys, QStringLiteral("preKeyPublic"), entry.second, ns);
                pk.setAttribute(QStringLiteral("preKeyId"), QString::number(entry.first));
            }
        } else {
            auto spk = appendBase64(document, bundle, QStringLiteral("spk"), value.signedPreKey, ns);
            spk.setAttribute(QStringLiteral("id"), QString::number(value.signedPreKeyId));
            appendBase64(document, bundle, QStringLiteral("spks"), value.signedPreKeySignature, ns);
            appendBase64(document, bundle, QStringLiteral("ik"), value.identityKey, ns);
            auto prekeys = document.createElementNS(ns, QStringLiteral("prekeys"));
            bundle.appendChild(prekeys);
            for (const auto &entry : value.preKeys) {
                auto pk = appendBase64(document, prekeys, QStringLiteral("pk"), entry.second, ns);
                pk.setAttribute(QStringLiteral("id"), QString::number(entry.first));
            }
        }
        return document;
    }

    bool hasSession(const QString &owner, uint32_t id, OmemoProtocol protocol) const
    {
        const auto device = data.devices.value(Jid(owner).bare()).value(id);
        return !device.protocols.value(protocol).session.isEmpty();
    }

    int buildSession(const QString &owner, uint32_t id, OmemoProtocol protocol, const Bundle &bundle, QString *error)
    {
        const QString bare = Jid(owner).bare();
        if (bare.isEmpty() || id == 0 || id > static_cast<uint32_t>(INT32_MAX) || bundle.protocol != protocol)
            return SG_ERR_INVAL;

        const auto storedIdentity    = storedIdentityFromWire(bundle.identityKey, protocol);
        const auto canonicalIdentity = wireIdentityFromStored(storedIdentity, OmemoProtocol::Omemo2);
        if (storedIdentity.isEmpty() || canonicalIdentity.isEmpty()) {
            if (error)
                *error = QStringLiteral("Remote OMEMO identity key could not be decoded");
            return SG_ERR_INVALID_KEY;
        }
        auto device = data.devices.value(bare).value(id);
        auto state  = device.protocols.value(protocol);
        if (!state.keyId.isEmpty() && state.keyId != storedIdentity) {
            if (error)
                *error = QStringLiteral("Remote %1 OMEMO identity key changed").arg(protocolName(protocol));
            return SG_ERR_UNTRUSTED_IDENTITY;
        }
        state.keyId = storedIdentity;
        device.protocols.insert(protocol, state);
        if (protocol == OmemoProtocol::Omemo2 && !device.label.isEmpty() && !device.labelSignature.isEmpty())
            device.labelVerified = verifyDeviceLabel(bundle.identityKey, device.label, device.labelSignature);
        if (!setDevice(bare, id, device)) {
            if (error)
                *error = QStringLiteral("Could not persist the remote OMEMO identity");
            return SG_ERR_UNKNOWN;
        }

        if (!identityAccepted(bare, canonicalIdentity)) {
            if (error)
                *error = QStringLiteral("Remote OMEMO identity is not trusted by policy");
            return SG_ERR_UNTRUSTED_IDENTITY;
        }

        ec_public_key *identity     = nullptr;
        ec_public_key *signedPreKey = nullptr;
        ec_public_key *preKey       = nullptr;
        int            result       = SG_SUCCESS;
        if (protocol == OmemoProtocol::Legacy) {
            result = curve_decode_point(&identity, reinterpret_cast<const uint8_t *>(bundle.identityKey.constData()),
                                        static_cast<size_t>(bundle.identityKey.size()), signalContext);
            if (result == SG_SUCCESS)
                result = curve_decode_point(&signedPreKey,
                                            reinterpret_cast<const uint8_t *>(bundle.signedPreKey.constData()),
                                            static_cast<size_t>(bundle.signedPreKey.size()), signalContext);
        } else {
            result = curve_decode_point_ed(&identity, reinterpret_cast<const uint8_t *>(bundle.identityKey.constData()),
                                           static_cast<size_t>(bundle.identityKey.size()), signalContext);
            if (result == SG_SUCCESS)
                result = curve_decode_point_mont(&signedPreKey,
                                                 reinterpret_cast<const uint8_t *>(bundle.signedPreKey.constData()),
                                                 static_cast<size_t>(bundle.signedPreKey.size()), signalContext);
        }

        const auto chosen
            = bundle.preKeys.at(static_cast<int>(randomIndex(static_cast<uint32_t>(bundle.preKeys.size()))));
        if (result == SG_SUCCESS) {
            result = protocol == OmemoProtocol::Legacy
                ? curve_decode_point(&preKey, reinterpret_cast<const uint8_t *>(chosen.second.constData()),
                                     static_cast<size_t>(chosen.second.size()), signalContext)
                : curve_decode_point_mont(&preKey, reinterpret_cast<const uint8_t *>(chosen.second.constData()),
                                          static_cast<size_t>(chosen.second.size()), signalContext);
        }

        session_pre_key_bundle *signalBundle = nullptr;
        if (result == SG_SUCCESS) {
            result = session_pre_key_bundle_create(
                &signalBundle, id, 0, chosen.first, preKey, bundle.signedPreKeyId, signedPreKey,
                reinterpret_cast<const uint8_t *>(bundle.signedPreKeySignature.constData()),
                static_cast<size_t>(bundle.signedPreKeySignature.size()), identity);
        }
        SIGNAL_UNREF(identity);
        SIGNAL_UNREF(signedPreKey);
        SIGNAL_UNREF(preKey);
        if (result != SG_SUCCESS) {
            if (error)
                *error = signalErrorString(QStringLiteral("create remote pre-key bundle"), result);
            return result;
        }

        const auto       name    = bare.toUtf8();
        auto             address = signalAddress(name, id);
        session_builder *builder = nullptr;
        result                   = session_builder_create(&builder, store(protocol).context, &address, signalContext);
        if (result == SG_SUCCESS) {
            if (protocol == OmemoProtocol::Omemo2)
                session_builder_set_version(builder, CIPHERTEXT_OMEMO_VERSION);
            result = session_builder_process_pre_key_bundle(builder, signalBundle);
        }
        session_builder_free(builder);
        SIGNAL_UNREF(signalBundle);
        if (result != SG_SUCCESS && error)
            *error = signalErrorString(QStringLiteral("build %1 OMEMO session").arg(protocolName(protocol)), result);
        return result;
    }

    int encryptKey(const QString &owner, uint32_t id, OmemoProtocol protocol, const QByteArray &keyMaterial,
                   EncryptedKey *result, QString *error)
    {
        const auto      bare    = Jid(owner).bare();
        const auto      name    = bare.toUtf8();
        auto            address = signalAddress(name, id);
        session_cipher *cipher  = nullptr;
        int             code    = session_cipher_create(&cipher, store(protocol).context, &address, signalContext);
        if (code == SG_SUCCESS) {
            if (protocol == OmemoProtocol::Omemo2)
                session_cipher_set_version(cipher, CIPHERTEXT_OMEMO_VERSION);
            ciphertext_message *message = nullptr;
            code = session_cipher_encrypt(cipher, reinterpret_cast<const uint8_t *>(keyMaterial.constData()),
                                          static_cast<size_t>(keyMaterial.size()), &message);
            if (code == SG_SUCCESS && message) {
                if (result) {
                    result->owner       = bare;
                    result->deviceId    = id;
                    result->keyExchange = ciphertext_message_get_type(message) == CIPHERTEXT_PREKEY_TYPE;
                    result->data        = fromSignalBuffer(ciphertext_message_get_serialized(message));
                }
                SIGNAL_UNREF(message);
            }
        }
        session_cipher_free(cipher);
        if (code != SG_SUCCESS && error)
            *error
                = signalErrorString(QStringLiteral("encrypt %1 OMEMO content key").arg(protocolName(protocol)), code);
        return code;
    }

    int decryptKey(const QString &owner, uint32_t id, OmemoProtocol protocol, bool keyExchange,
                   const QByteArray &ciphertext, QByteArray *plaintext, uint32_t *messageCounter,
                   QByteArray *ratchetKey, QString *error)
    {
        const auto      bare    = Jid(owner).bare();
        const auto      name    = bare.toUtf8();
        auto            address = signalAddress(name, id);
        session_cipher *cipher  = nullptr;
        int             code    = session_cipher_create(&cipher, store(protocol).context, &address, signalContext);
        signal_buffer  *plain   = nullptr;
        if (messageCounter)
            *messageCounter = 0;
        if (ratchetKey)
            ratchetKey->clear();
        if (code == SG_SUCCESS) {
            if (protocol == OmemoProtocol::Omemo2)
                session_cipher_set_version(cipher, CIPHERTEXT_OMEMO_VERSION);
            if (keyExchange) {
                pre_key_signal_message *message = nullptr;
                if (protocol == OmemoProtocol::Omemo2) {
                    code = pre_key_signal_message_deserialize_omemo(
                        &message, reinterpret_cast<const uint8_t *>(ciphertext.constData()),
                        static_cast<size_t>(ciphertext.size()), data.ownDevice ? data.ownDevice->id : 0, signalContext);
                } else {
                    code = pre_key_signal_message_deserialize(&message,
                                                              reinterpret_cast<const uint8_t *>(ciphertext.constData()),
                                                              static_cast<size_t>(ciphertext.size()), signalContext);
                }
                if (code == SG_SUCCESS && message) {
                    const auto inner = pre_key_signal_message_get_signal_message(message);
                    if (inner) {
                        if (messageCounter)
                            *messageCounter = signal_message_get_counter(inner);
                        if (ratchetKey && signal_message_get_sender_ratchet_key(inner)) {
                            signal_buffer *serializedRatchet = nullptr;
                            if (ec_public_key_serialize(&serializedRatchet,
                                                        signal_message_get_sender_ratchet_key(inner))
                                == SG_SUCCESS) {
                                *ratchetKey = fromSignalBuffer(serializedRatchet);
                            }
                            if (serializedRatchet)
                                signal_buffer_free(serializedRatchet);
                        }
                    }
                    code = session_cipher_decrypt_pre_key_signal_message(cipher, message, nullptr, &plain);
                }
                SIGNAL_UNREF(message);
            } else {
                signal_message *message = nullptr;
                if (protocol == OmemoProtocol::Omemo2) {
                    code = signal_message_deserialize_omemo(&message,
                                                            reinterpret_cast<const uint8_t *>(ciphertext.constData()),
                                                            static_cast<size_t>(ciphertext.size()), signalContext);
                } else {
                    code = signal_message_deserialize(&message,
                                                      reinterpret_cast<const uint8_t *>(ciphertext.constData()),
                                                      static_cast<size_t>(ciphertext.size()), signalContext);
                }
                if (code == SG_SUCCESS && message) {
                    if (messageCounter)
                        *messageCounter = signal_message_get_counter(message);
                    if (ratchetKey && signal_message_get_sender_ratchet_key(message)) {
                        signal_buffer *serializedRatchet = nullptr;
                        if (ec_public_key_serialize(&serializedRatchet, signal_message_get_sender_ratchet_key(message))
                            == SG_SUCCESS) {
                            *ratchetKey = fromSignalBuffer(serializedRatchet);
                        }
                        if (serializedRatchet)
                            signal_buffer_free(serializedRatchet);
                    }
                    code = session_cipher_decrypt_signal_message(cipher, message, nullptr, &plain);
                }
                SIGNAL_UNREF(message);
            }
        }
        session_cipher_free(cipher);
        if (code == SG_SUCCESS && plaintext)
            *plaintext = fromSignalBuffer(plain);
        if (plain)
            signal_buffer_bzero_free(plain);
        if (code != SG_SUCCESS && error)
            *error
                = signalErrorString(QStringLiteral("decrypt %1 OMEMO content key").arg(protocolName(protocol)), code);
        return code;
    }

    bool encryptPayload(const QByteArray &plaintext, QByteArray *contentKeyAndMac, QByteArray *ciphertext,
                        QString *error) const
    {
        const auto contentKey = randomBytes(32);
        const auto material   = payloadMaterial(contentKey);
        if (!material) {
            if (error)
                *error = QStringLiteral("Could not derive OMEMO payload keys");
            return false;
        }
        const auto encrypted = qcaAesCbc(true, material->encryptionKey, material->iv, plaintext);
        if (encrypted.isEmpty()) {
            if (error)
                *error = QStringLiteral("QCA AES-256-CBC encryption failed");
            return false;
        }
        const auto mac = qcaHmacSha256(material->authenticationKey, encrypted).left(16);
        if (mac.size() != 16) {
            if (error)
                *error = QStringLiteral("QCA HMAC-SHA256 calculation failed");
            return false;
        }
        if (contentKeyAndMac)
            *contentKeyAndMac = contentKey + mac;
        if (ciphertext)
            *ciphertext = encrypted;
        return true;
    }

    bool decryptPayload(const QByteArray &contentKeyAndMac, const QByteArray &ciphertext, QByteArray *plaintext,
                        QString *error) const
    {
        if (contentKeyAndMac.size() != 48) {
            if (error)
                *error = QStringLiteral("OMEMO content key has an invalid size");
            return false;
        }
        const auto material = payloadMaterial(contentKeyAndMac.left(32));
        if (!material)
            return false;
        const auto expected = qcaHmacSha256(material->authenticationKey, ciphertext).left(16);
        if (!constantEqual(expected, contentKeyAndMac.mid(32, 16))) {
            if (error)
                *error = QStringLiteral("OMEMO payload authentication failed");
            return false;
        }
        const auto plain = qcaAesCbc(false, material->encryptionKey, material->iv, ciphertext);
        if (plain.isNull()) {
            if (error)
                *error = QStringLiteral("QCA AES-256-CBC decryption failed");
            return false;
        }
        if (plaintext)
            *plaintext = plain;
        return true;
    }

    OmemoProtocols knownProtocolsFor(const QString &owner) const
    {
        OmemoProtocols result;
        const auto     bare    = Jid(owner).bare();
        const auto     devices = data.devices.value(bare);
        for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
            if (supportedProtocols.testFlag(OmemoProtocol::Omemo2)
                && protocolActive(it.value(), OmemoProtocol::Omemo2)) {
                result |= OmemoProtocol::Omemo2;
            }
            if (supportedProtocols.testFlag(OmemoProtocol::Legacy)
                && protocolActive(it.value(), OmemoProtocol::Legacy)) {
                result |= OmemoProtocol::Legacy;
            }
        }
        return result;
    }

    std::optional<OmemoProtocol> preferredKnownProtocolFor(const QString &owner) const
    {
        const auto protocols = knownProtocolsFor(owner);
        if (protocols.testFlag(OmemoProtocol::Omemo2))
            return OmemoProtocol::Omemo2;
        if (protocols.testFlag(OmemoProtocol::Legacy))
            return OmemoProtocol::Legacy;
        return std::nullopt;
    }

    QList<QPair<QString, uint32_t>> activeDevicesFor(const QString &owner, OmemoProtocol protocol) const
    {
        QList<QPair<QString, uint32_t>> result;
        const auto                      bare    = Jid(owner).bare();
        const auto                      devices = data.devices.value(bare);
        for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
            if (!protocolActive(it.value(), protocol))
                continue;

            // An explicitly distrusted device is not a recipient. Keep
            // unknown/undecided devices in the target set so fetching their
            // bundle can surface the identity to the application's trust UI.
            const auto state = it->protocols.value(protocol);
            const auto wire  = wireIdentityFromStored(state.keyId, OmemoProtocol::Omemo2);
            if (!wire.isEmpty() && trustLevel(bare, wire) == EncryptionTrustLevel::Distrusted)
                continue;

            result.append(qMakePair(bare, it.key()));
        }
        if (data.ownDevice && bare == client->jid().bare() && !result.contains(qMakePair(bare, data.ownDevice->id)))
            result.append(qMakePair(bare, data.ownDevice->id));
        return result;
    }

    void publishPepItem(const QString &node, const PubSubItem &item, const PubSubOptions &options,
                        const std::function<void(bool, QString)> &callback)
    {
        const auto service = client->jid().bare();
        auto       first   = client->pubSubManager()->publish(service, node, item, options);
        connect(first, &Task::finished, q, [this, first, service, node, item, options, callback]() {
            if (first->success()) {
                callback(true, {});
                return;
            }
            const auto firstError = first->statusString();
            if (options.isEmpty()) {
                callback(false, firstError.isEmpty() ? QStringLiteral("Could not publish PEP item") : firstError);
                return;
            }
            auto configure = client->pubSubManager()->configureNode(service, node, options);
            connect(
                configure, &Task::finished, q, [this, configure, service, node, item, options, callback, firstError]() {
                    if (!configure->success()) {
                        callback(false,
                                 firstError.isEmpty() ? QStringLiteral("Could not publish or configure PEP node")
                                                      : firstError);
                        return;
                    }
                    auto retry = client->pubSubManager()->publish(service, node, item, options);
                    connect(retry, &Task::finished, q, [retry, callback]() {
                        callback(retry->success(),
                                 retry->success()
                                     ? QString()
                                     : (retry->statusString().isEmpty() ? QStringLiteral("Could not publish PEP item")
                                                                        : retry->statusString()));
                    });
                    retry->go(true);
                });
            configure->go(true);
        });
        first->go(true);
    }

    QDomDocument emptyDeviceList(OmemoProtocol protocol) const
    {
        QDomDocument document;
        const auto   rootName = protocol == OmemoProtocol::Legacy ? QStringLiteral("list") : QStringLiteral("devices");
        document.appendChild(document.createElementNS(protocolNamespace(protocol), rootName));
        return document;
    }

    void fetchDeviceList(const QString &owner, OmemoProtocol protocol,
                         const std::function<void(bool, QString)> &callback)
    {
        const QStringList ids
            = protocol == OmemoProtocol::Omemo2 ? QStringList { QStringLiteral("current") } : QStringList {};
        const int maxItems = protocol == OmemoProtocol::Legacy ? 1 : 0;
        auto task = client->pubSubManager()->items(Jid(owner).bare(), protocolDevicesNode(protocol), ids, maxItems);
        connect(task, &Task::finished, q, [this, task, owner, protocol, callback]() {
            if (!task->success()) {
                if (task->error().condition == Stanza::Error::ErrorCond::ItemNotFound) {
                    auto       empty = emptyDeviceList(protocol);
                    QString    mergeError;
                    const bool ok = mergeDeviceList(owner, protocol, empty.documentElement(), &mergeError);
                    callback(ok, mergeError);
                    return;
                }
                callback(false,
                         task->statusString().isEmpty()
                             ? QStringLiteral("Could not fetch %1 OMEMO device list").arg(protocolName(protocol))
                             : task->statusString());
                return;
            }
            QDomElement payload;
            for (const auto &item : task->items()) {
                if ((protocol == OmemoProtocol::Omemo2 && item.id() == QLatin1String("current")) || payload.isNull())
                    payload = item.payload();
            }
            if (payload.isNull()) {
                auto empty = emptyDeviceList(protocol);
                payload    = empty.documentElement();
                QString    mergeError;
                const bool ok = mergeDeviceList(owner, protocol, payload, &mergeError);
                callback(ok, mergeError);
                return;
            }
            QString    mergeError;
            const bool ok = mergeDeviceList(owner, protocol, payload, &mergeError);
            callback(ok, mergeError);
        });
        task->go(true);
    }

    void fetchBundle(const QString &owner, uint32_t id, OmemoProtocol protocol, bool build,
                     const std::function<void(bool, EncryptionJob::Error, QString)> &callback)
    {
        const QStringList ids
            = protocol == OmemoProtocol::Omemo2 ? QStringList { QString::number(id) } : QStringList {};
        const int maxItems = protocol == OmemoProtocol::Legacy ? 1 : 0;
        auto task = client->pubSubManager()->items(Jid(owner).bare(), protocolBundleNode(protocol, id), ids, maxItems);
        connect(task, &Task::finished, q, [this, task, owner, id, protocol, build, callback]() {
            if (!task->success()) {
                callback(false, EncryptionJob::Error::NetworkError,
                         task->statusString().isEmpty()
                             ? QStringLiteral("Could not fetch %1 OMEMO bundle").arg(protocolName(protocol))
                             : task->statusString());
                return;
            }
            QDomElement payload;
            for (const auto &item : task->items()) {
                if ((protocol == OmemoProtocol::Omemo2 && item.id() == QString::number(id)) || payload.isNull())
                    payload = item.payload();
            }
            if (payload.isNull()) {
                callback(false, EncryptionJob::Error::ProtocolError,
                         QStringLiteral("No %1 OMEMO bundle is published for device %2")
                             .arg(protocolName(protocol), QString::number(id)));
                return;
            }
            QString    parseError;
            const auto bundle = parseBundle(payload, protocol, &parseError);
            if (!bundle) {
                callback(false, EncryptionJob::Error::ProtocolError, parseError);
                return;
            }

            const auto bare      = Jid(owner).bare();
            const auto stored    = storedIdentityFromWire(bundle->identityKey, protocol);
            const auto canonical = wireIdentityFromStored(stored, OmemoProtocol::Omemo2);
            if (stored.isEmpty() || canonical.isEmpty()) {
                callback(false, EncryptionJob::Error::ProtocolError,
                         QStringLiteral("Remote OMEMO identity key could not be decoded"));
                return;
            }
            auto device = data.devices.value(bare).value(id);
            auto state  = device.protocols.value(protocol);
            if (!state.keyId.isEmpty() && state.keyId != stored) {
                callback(false, EncryptionJob::Error::AuthenticationFailed,
                         QStringLiteral("Remote %1 OMEMO identity key changed").arg(protocolName(protocol)));
                return;
            }
            if (state.keyId.isEmpty()) {
                state.keyId = stored;
                device.protocols.insert(protocol, state);
                if (protocol == OmemoProtocol::Omemo2 && !device.label.isEmpty())
                    device.labelVerified = verifyDeviceLabel(bundle->identityKey, device.label, device.labelSignature);
                if (!setDevice(bare, id, device)) {
                    callback(false, EncryptionJob::Error::StorageError,
                             QStringLiteral("Could not persist remote OMEMO identity"));
                    return;
                }
                if (trustLevel(bare, canonical) == EncryptionTrustLevel::Undecided
                    && newIdentityTrust != EncryptionTrustLevel::Undecided) {
                    if (!trustStorage->setTrustLevel(OmemoEncryption::methodId(), Jid(bare), canonical,
                                                     newIdentityTrust)) {
                        callback(false, EncryptionJob::Error::StorageError,
                                 QStringLiteral("Could not persist OMEMO trust state"));
                        return;
                    }
                    emit q->trustChanged(Jid(bare), canonical, newIdentityTrust);
                }
            }
            if (!build) {
                callback(true, EncryptionJob::Error::None, {});
                return;
            }
            if (!identityAccepted(bare, canonical)) {
                callback(false, EncryptionJob::Error::UntrustedIdentity,
                         QStringLiteral("Remote OMEMO identity is not trusted by policy"));
                return;
            }
            QString   buildError;
            const int code = buildSession(owner, id, protocol, *bundle, &buildError);
            callback(code == SG_SUCCESS, signalErrorToJob(code), buildError);
        });
        task->go(true);
    }

    void
    resolveRecoveryTarget(const EncryptionMetadata &metadata, bool buildSession,
                          const std::function<void(bool, EncryptionJob::Error, QString, EncryptionMetadata)> &callback)
    {
        if (metadata.methodId != OmemoEncryption::methodId() || !metadata.sender.isValid()
            || metadata.senderDeviceId == 0) {
            callback(false, EncryptionJob::Error::InvalidInput, QStringLiteral("Invalid OMEMO recovery context"),
                     metadata);
            return;
        }

        const auto protocol = protocolFromName(metadata.details.value(QLatin1String(OmemoProtocolOption)));
        if (!protocol || !supportedProtocols.testFlag(*protocol)) {
            callback(false, EncryptionJob::Error::Unsupported,
                     QStringLiteral("OMEMO recovery context has no supported wire profile"), metadata);
            return;
        }

        const Jid      sender   = metadata.sender;
        const uint32_t deviceId = metadata.senderDeviceId;
        if (data.ownDevice && sender.bare() == client->jid().bare() && deviceId == data.ownDevice->id) {
            callback(false, EncryptionJob::Error::InvalidInput,
                     QStringLiteral("Cannot recover an OMEMO session with the local device itself"), metadata);
            return;
        }

        fetchDeviceList(
            sender.bare(), *protocol,
            [this, metadata, sender, deviceId, protocol = *protocol, buildSession, callback](bool           listOk,
                                                                                             const QString &listError) {
                const auto target = qMakePair(sender.bare(), deviceId);
                if (!listOk) {
                    callback(false, EncryptionJob::Error::NetworkError, listError, metadata);
                    return;
                }
                if (!activeDevicesFor(sender.bare(), protocol).contains(target)) {
                    callback(false, EncryptionJob::Error::NoRecipients,
                             QStringLiteral("OMEMO recovery device is not in the sender's active device list"),
                             metadata);
                    return;
                }

                fetchBundle(sender.bare(), deviceId, protocol, buildSession,
                            [this, metadata, sender, deviceId, protocol, callback](bool ok, EncryptionJob::Error error,
                                                                                   const QString &message) {
                                if (!ok) {
                                    callback(false, error, message, metadata);
                                    return;
                                }

                                auto       prepared = metadata;
                                const auto device   = data.devices.value(sender.bare()).value(deviceId);
                                const auto state    = device.protocols.value(protocol);
                                const auto identity = wireIdentityFromStored(state.keyId, OmemoProtocol::Omemo2);
                                if (identity.isEmpty()) {
                                    callback(false, EncryptionJob::Error::ProtocolError,
                                             QStringLiteral("OMEMO recovery device has no usable identity key"),
                                             metadata);
                                    return;
                                }
                                prepared.senderKey = identity;
                                prepared.details.insert(QStringLiteral("trustLevel"),
                                                        static_cast<int>(trustLevel(sender.bare(), identity)));
                                if (device.labelVerified && !device.label.isEmpty())
                                    prepared.details.insert(QStringLiteral("deviceLabel"), device.label);
                                callback(true, EncryptionJob::Error::None, {}, prepared);
                            });
            });
    }

    void ensureDeviceLists(const QStringList &owners, OmemoProtocol protocol, int index,
                           const std::function<void(bool, QString)> &callback)
    {
        if (index >= owners.size()) {
            callback(true, {});
            return;
        }
        const auto owner = Jid(owners.at(index)).bare();
        if (fetchedDeviceLists.contains(fetchedListKey(owner, protocol))) {
            ensureDeviceLists(owners, protocol, index + 1, callback);
            return;
        }
        fetchDeviceList(owner, protocol, [this, owners, protocol, index, callback](bool ok, const QString &error) {
            if (!ok) {
                callback(false, error);
                return;
            }
            ensureDeviceLists(owners, protocol, index + 1, callback);
        });
    }

    void ensureSessions(const QList<QPair<QString, uint32_t>> &targets, OmemoProtocol protocol, int index,
                        bool                                    skipUnusableBundles,
                        const std::function<void(bool, EncryptionJob::Error, QString, QList<QPair<QString, uint32_t>>,
                                                 QStringList)> &callback,
                        QList<QPair<QString, uint32_t>> readyTargets = {}, QStringList skippedTargets = {})
    {
        if (index >= targets.size()) {
            callback(true, EncryptionJob::Error::None, {}, readyTargets, skippedTargets);
            return;
        }
        const auto target = targets.at(index);
        if (data.ownDevice && target.first == client->jid().bare() && target.second == data.ownDevice->id) {
            ensureSessions(targets, protocol, index + 1, skipUnusableBundles, callback, readyTargets, skippedTargets);
            return;
        }
        if (hasSession(target.first, target.second, protocol)) {
            const auto device = data.devices.value(target.first).value(target.second);
            const auto state  = device.protocols.value(protocol);
            const auto wire   = wireIdentityFromStored(state.keyId, OmemoProtocol::Omemo2);
            if (!wire.isEmpty() && identityAccepted(target.first, wire)) {
                readyTargets.append(target);
                ensureSessions(targets, protocol, index + 1, skipUnusableBundles, callback, readyTargets,
                               skippedTargets);
                return;
            }
            callback(
                false, EncryptionJob::Error::UntrustedIdentity,
                QStringLiteral("An existing %1 OMEMO session is not trusted by policy").arg(protocolName(protocol)),
                readyTargets, skippedTargets);
            return;
        }
        fetchBundle(
            target.first, target.second, protocol, true,
            [this, targets, protocol, index, skipUnusableBundles, callback, readyTargets, skippedTargets,
             target](bool ok, EncryptionJob::Error jobError, const QString &error) mutable {
                if (!ok) {
                    // A stale device id without a bundle cannot receive a new OMEMO session.  It is safe to omit
                    // it from this stanza as long as every logical remote recipient still has another target.
                    if (skipUnusableBundles && jobError == EncryptionJob::Error::ProtocolError) {
                        skippedTargets.append(
                            QStringLiteral("%1/%2: %3").arg(target.first, QString::number(target.second), error));
                        ensureSessions(targets, protocol, index + 1, skipUnusableBundles, callback, readyTargets,
                                       skippedTargets);
                        return;
                    }
                    callback(false, jobError, error, readyTargets, skippedTargets);
                    return;
                }
                readyTargets.append(target);
                ensureSessions(targets, protocol, index + 1, skipUnusableBundles, callback, readyTargets,
                               skippedTargets);
            });
    }

    std::optional<OmemoProtocol> selectProtocol(const QDomElement &stanza, const EncryptionContext &context,
                                                QString *error) const
    {
        if (context.replyTo) {
            if (const auto explicitReply
                = protocolFromName(context.replyTo->details.value(QLatin1String(OmemoProtocolOption)))) {
                if (!supportedProtocols.testFlag(*explicitReply)) {
                    if (error)
                        *error = QStringLiteral("The requested OMEMO wire profile is unavailable locally");
                    return std::nullopt;
                }
                return explicitReply;
            }
        }
        if (const auto explicitProtocol = protocolFromName(context.options.value(QLatin1String(OmemoProtocolOption)))) {
            if (!supportedProtocols.testFlag(*explicitProtocol)) {
                if (error)
                    *error = QStringLiteral("The requested OMEMO wire profile is unavailable locally");
                return std::nullopt;
            }
            return explicitProtocol;
        }

        QList<Jid> resources;
        for (const auto &jid : context.recipients) {
            if (jid.isValid() && !jid.resource().isEmpty() && !resources.contains(jid))
                resources.append(jid);
        }
        if (resources.isEmpty()) {
            const Jid to(stanza.attribute(QStringLiteral("to")));
            if (to.isValid() && !to.resource().isEmpty())
                resources.append(to);
        }

        std::optional<OmemoProtocol> selected;
        for (const auto &resource : resources) {
            // A cached PEP device list is actual OMEMO discovery data, whereas
            // entity capabilities only say what this online resource can
            // implement. Prefer the known list so a dual-stack resource whose
            // OMEMO 2 node is absent can still use its legacy devices.
            auto current = preferredKnownProtocolFor(resource.bare());
            if (!current)
                current = q->preferredProtocolFor(resource);
            if (!current)
                continue;
            if (selected && *selected != *current) {
                if (error)
                    *error = QStringLiteral(
                        "Recipients require different OMEMO wire profiles; split the operation by full JID");
                return std::nullopt;
            }
            selected = current;
        }
        if (selected)
            return selected;

        // A bare/offline JID may still have a cached PEP device list.  Prefer
        // that information before choosing the modern profile as the default.
        QStringList owners;
        for (const auto &jid : context.recipients) {
            const auto bare = jid.bare();
            if (!bare.isEmpty() && !owners.contains(bare))
                owners.append(bare);
        }
        if (owners.isEmpty()) {
            const Jid to(stanza.attribute(QStringLiteral("to")));
            if (to.isValid())
                owners.append(to.bare());
        }
        for (const auto &owner : owners) {
            if (const auto known = preferredKnownProtocolFor(owner))
                return known;
        }

        if (supportedProtocols.testFlag(OmemoProtocol::Omemo2))
            return OmemoProtocol::Omemo2;
        if (supportedProtocols.testFlag(OmemoProtocol::Legacy))
            return OmemoProtocol::Legacy;
        if (error)
            *error = QStringLiteral("No OMEMO wire profile is available locally");
        return std::nullopt;
    }

    QList<QPair<QString, uint32_t>> selectTargets(const QDomElement &stanza, const EncryptionContext &context,
                                                  OmemoProtocol protocol, QStringList *owners, QString *error) const
    {
        QStringList logicalOwners;
        for (const auto &jid : context.recipients) {
            const auto bare = jid.bare();
            if (!bare.isEmpty() && !logicalOwners.contains(bare))
                logicalOwners.append(bare);
        }
        if (logicalOwners.isEmpty()) {
            const Jid to(stanza.attribute(QStringLiteral("to")));
            if (to.isValid())
                logicalOwners.append(to.bare());
        }
        if (logicalOwners.isEmpty()) {
            if (error)
                *error = QStringLiteral("OMEMO encryption has no logical recipient");
            return {};
        }
        const auto ownBare = client->jid().bare();
        if (!ownBare.isEmpty() && !logicalOwners.contains(ownBare))
            logicalOwners.append(ownBare);
        if (owners)
            *owners = logicalOwners;

        QList<QPair<QString, uint32_t>> targets;
        const bool                      replyOnlySender = context.replyTo.has_value()
            && context.options.value(QStringLiteral("replyOnlySenderDevice"), true).toBool();
        for (const auto &owner : logicalOwners) {
            if (replyOnlySender && context.replyTo && owner == context.replyTo->sender.bare()
                && context.replyTo->senderDeviceId > 0) {
                targets.append(qMakePair(owner, context.replyTo->senderDeviceId));
                continue;
            }
            for (const auto &target : activeDevicesFor(owner, protocol)) {
                if (data.ownDevice && target.first == ownBare && target.second == data.ownDevice->id)
                    continue;
                if (!targets.contains(target))
                    targets.append(target);
            }
        }

        // It is valid to have no secondary device for our own account, but an
        // outgoing OMEMO stanza must cover at least one device of every remote
        // logical recipient. Otherwise a contact whose devices were all
        // explicitly distrusted would receive an undecryptable stanza that was
        // encrypted only for our own carbon devices.
        for (const auto &owner : logicalOwners) {
            if (owner == ownBare)
                continue;
            const bool covered = std::any_of(targets.cbegin(), targets.cend(),
                                             [&owner](const auto &target) { return target.first == owner; });
            if (!covered) {
                if (error)
                    *error = QStringLiteral("No trusted or undecided active OMEMO devices for %1").arg(owner);
                return {};
            }
        }
        return targets;
    }
};

class OmemoEncryptedSession final : public EncryptedSession {
public:
    OmemoEncryptedSession(OmemoEncryption *method, const EncryptionContext &context) :
        EncryptedSession(OmemoEncryption::methodId(), context), method_(method)
    {
    }

    EncryptionJob *encrypt(const QDomElement &xml) override
    {
        const auto &context = this->context();
        auto        job     = new EncryptionJob(this);
        auto        d       = method_->d.get();
        if (!d->ready) {
            job->fail(EncryptionJob::Error::NoSession, QStringLiteral("OMEMO has not been set up locally"));
            return job;
        }
        if (xml.isNull()) {
            job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("Cannot OMEMO-encrypt a null stanza"));
            return job;
        }

        QString    protocolError;
        const auto protocolValue = d->selectProtocol(xml, context, &protocolError);
        if (!protocolValue) {
            job->fail(EncryptionJob::Error::Unsupported, protocolError);
            return job;
        }
        const auto protocol = *protocolValue;
        if (protocol == OmemoProtocol::Legacy && localName(xml) != QLatin1String("message")) {
            job->fail(EncryptionJob::Error::Unsupported, QStringLiteral("Legacy OMEMO supports message stanzas only"));
            return job;
        }

        QDomDocument inputDocument;
        auto         stanza = inputDocument.importNode(xml, true).toElement();
        inputDocument.appendChild(stanza);
        if (stanza.attribute(QStringLiteral("from")).isEmpty() && d->client->jid().isValid())
            stanza.setAttribute(QStringLiteral("from"), d->client->jid().full());

        QDomElement preparedOuter;
        QByteArray  plaintext;
        bool        legacyHasPayload = false;
        if (protocol == OmemoProtocol::Omemo2) {
            StanzaContentEncryption::Profile profile;
            profile.affixes = StanzaContentEncryption::RandomPadding | StanzaContentEncryption::From;
            if (!stanza.attribute(QStringLiteral("to")).isEmpty())
                profile.affixes |= StanzaContentEncryption::To;
            profile.minimumEnvelopeSize = d->minimumEnvelopeSize;
            QString sceError;
            auto    prepared = StanzaContentEncryption::prepare(stanza, profile, &sceError);
            if (!prepared) {
                job->fail(EncryptionJob::Error::InvalidInput, sceError);
                return job;
            }
            preparedOuter = prepared->outerDocument.documentElement();
            plaintext     = prepared->envelopeDocument.toByteArray(-1);
        } else {
            preparedOuter   = stanza;
            const auto body = stanza.firstChildElement(QStringLiteral("body"));
            if (!body.isNull()) {
                legacyHasPayload = true;
                plaintext        = body.firstChild().nodeValue().toUtf8();
            }
        }

        QStringList owners;
        QString     targetError;
        d->selectTargets(stanza, context, protocol, &owners, &targetError); // recomputed after PEP fetch.
        if (owners.isEmpty()) {
            job->fail(EncryptionJob::Error::NoRecipients, targetError);
            return job;
        }

        QPointer<EncryptionJob> guardedJob(job);
        d->ensureDeviceLists(
            owners, protocol, 0,
            [this, guardedJob, preparedOuter, plaintext, legacyHasPayload, context, owners,
             protocol](bool ok, const QString &error) {
                if (!guardedJob)
                    return;
                auto d = method_->d.get();
                if (!ok) {
                    guardedJob->fail(EncryptionJob::Error::NetworkError, error);
                    return;
                }
                QStringList ignored;
                QString     targetError;
                const auto  targets = d->selectTargets(preparedOuter, context, protocol, &ignored, &targetError);
                if (targets.isEmpty()) {
                    guardedJob->fail(EncryptionJob::Error::NoRecipients,
                                     targetError.isEmpty() ? QStringLiteral("No active OMEMO recipient devices")
                                                           : targetError);
                    return;
                }
                d->ensureSessions(
                    targets, protocol, 0, true,
                    [this, guardedJob, preparedOuter, plaintext, legacyHasPayload, owners,
                     protocol](bool sessionsOk, EncryptionJob::Error sessionError, const QString &sessionErrorString,
                               QList<QPair<QString, uint32_t>> readyTargets, const QStringList &skippedTargets) {
                        if (!guardedJob)
                            return;
                        auto d = method_->d.get();
                        if (!sessionsOk) {
                            guardedJob->fail(sessionError, sessionErrorString);
                            return;
                        }

                        const auto ownBare = d->client->jid().bare();
                        for (const auto &owner : owners) {
                            if (owner == ownBare)
                                continue;
                            const bool covered
                                = std::any_of(readyTargets.cbegin(), readyTargets.cend(),
                                              [&owner](const auto &target) { return target.first == owner; });
                            if (!covered) {
                                guardedJob->fail(EncryptionJob::Error::NoRecipients,
                                                 QStringLiteral("No usable OMEMO bundle for %1").arg(owner));
                                return;
                            }
                        }
                        if (!skippedTargets.isEmpty()) {
                            emit method_->warning(QStringLiteral("OMEMO skipped unusable recipient device(s): %1")
                                                      .arg(skippedTargets.join(QStringLiteral("; "))));
                        }

                        QByteArray keyMaterial;
                        QByteArray payload;
                        QByteArray legacyIv;
                        QString    cryptoError;
                        if (protocol == OmemoProtocol::Omemo2) {
                            if (!d->encryptPayload(plaintext, &keyMaterial, &payload, &cryptoError)) {
                                guardedJob->fail(EncryptionJob::Error::CryptoError, cryptoError);
                                return;
                            }
                        } else {
                            keyMaterial = randomBytes(16);
                            legacyIv    = randomBytes(12);
                            if (keyMaterial.size() != 16 || legacyIv.size() != 12) {
                                guardedJob->fail(EncryptionJob::Error::CryptoError,
                                                 QStringLiteral("Could not generate legacy OMEMO payload material"));
                                return;
                            }
                            if (legacyHasPayload) {
                                const auto encrypted = qcaAesGcm(true, keyMaterial, legacyIv, plaintext);
                                if (!encrypted || encrypted->tag.size() != 16) {
                                    guardedJob->fail(EncryptionJob::Error::CryptoError,
                                                     QStringLiteral("QCA AES-128-GCM encryption failed"));
                                    return;
                                }
                                payload = encrypted->data;
                                keyMaterial += encrypted->tag;
                            }
                        }

                        QList<OmemoEncryption::Private::EncryptedKey> keys;
                        for (const auto &target : readyTargets) {
                            OmemoEncryption::Private::EncryptedKey encrypted;
                            const int code = d->encryptKey(target.first, target.second, protocol, keyMaterial,
                                                           &encrypted, &cryptoError);
                            if (code != SG_SUCCESS) {
                                guardedJob->fail(signalErrorToJob(code), cryptoError);
                                return;
                            }
                            keys.append(encrypted);
                        }

                        QDomDocument outputDocument;
                        auto         outer = outputDocument.importNode(preparedOuter, true).toElement();
                        outputDocument.appendChild(outer);
                        const auto ns        = protocolNamespace(protocol);
                        auto       encrypted = outputDocument.createElementNS(ns, QStringLiteral("encrypted"));
                        auto       header    = outputDocument.createElementNS(ns, QStringLiteral("header"));
                        header.setAttribute(QStringLiteral("sid"), QString::number(d->data.ownDevice->id));

                        if (protocol == OmemoProtocol::Legacy) {
                            appendBase64(outputDocument, header, QStringLiteral("iv"), legacyIv, ns);
                            for (const auto &keyValue : keys) {
                                auto key = outputDocument.createElementNS(ns, QStringLiteral("key"));
                                key.setAttribute(QStringLiteral("rid"), QString::number(keyValue.deviceId));
                                if (keyValue.keyExchange)
                                    key.setAttribute(QStringLiteral("prekey"), QStringLiteral("true"));
                                key.appendChild(
                                    outputDocument.createTextNode(QString::fromLatin1(keyValue.data.toBase64())));
                                header.appendChild(key);
                            }
                        } else {
                            QMap<QString, QList<OmemoEncryption::Private::EncryptedKey>> byOwner;
                            for (const auto &key : keys)
                                byOwner[key.owner].append(key);
                            for (auto it = byOwner.cbegin(); it != byOwner.cend(); ++it) {
                                auto ownerKeys = outputDocument.createElementNS(ns, QStringLiteral("keys"));
                                ownerKeys.setAttribute(QStringLiteral("jid"), it.key());
                                for (const auto &keyValue : it.value()) {
                                    auto key = outputDocument.createElementNS(ns, QStringLiteral("key"));
                                    key.setAttribute(QStringLiteral("rid"), QString::number(keyValue.deviceId));
                                    if (keyValue.keyExchange)
                                        key.setAttribute(QStringLiteral("kex"), QStringLiteral("true"));
                                    key.appendChild(
                                        outputDocument.createTextNode(QString::fromLatin1(keyValue.data.toBase64())));
                                    ownerKeys.appendChild(key);
                                }
                                header.appendChild(ownerKeys);
                            }
                        }
                        encrypted.appendChild(header);
                        if (protocol == OmemoProtocol::Omemo2 || legacyHasPayload)
                            appendBase64(outputDocument, encrypted, QStringLiteral("payload"), payload, ns);

                        if (protocol == OmemoProtocol::Legacy && legacyHasPayload) {
                            const auto body = outer.firstChildElement(QStringLiteral("body"));
                            if (!body.isNull())
                                outer.removeChild(body);
                            const auto html = outer.firstChildElement(QStringLiteral("html"));
                            if (!html.isNull())
                                outer.removeChild(html);
                        }
                        outer.appendChild(encrypted);

                        if (directChildNS(outer, QStringLiteral("store"), QLatin1String(HintsNs)).isNull())
                            outer.appendChild(
                                outputDocument.createElementNS(QLatin1String(HintsNs), QStringLiteral("store")));
                        if (directChildNS(outer, QStringLiteral("encryption"), QLatin1String(EmeNs)).isNull()) {
                            auto eme
                                = outputDocument.createElementNS(QLatin1String(EmeNs), QStringLiteral("encryption"));
                            eme.setAttribute(QStringLiteral("namespace"), ns);
                            eme.setAttribute(QStringLiteral("name"), QLatin1String(OmemoName));
                            outer.appendChild(eme);
                        }
                        if (protocol == OmemoProtocol::Legacy && legacyHasPayload) {
                            auto fallback = outputDocument.createElement(QStringLiteral("body"));
                            fallback.appendChild(outputDocument.createTextNode(QStringLiteral(
                                "You received a message encrypted with OMEMO but your client does not support it.")));
                            outer.appendChild(fallback);
                        }

                        EncryptionMetadata metadata;
                        metadata.methodId     = OmemoEncryption::methodId();
                        metadata.protocolOnly = protocol == OmemoProtocol::Legacy && !legacyHasPayload;
                        metadata.details.insert(QLatin1String(OmemoProtocolOption), protocolName(protocol));
                        guardedJob->complete(outer, metadata);
                    });
            });
        return job;
    }

    EncryptionJob *decrypt(const QDomElement &xml) override
    {
        auto job = new EncryptionJob(this);
        auto d   = method_->d.get();
        if (!d->data.ownDevice) {
            job->fail(EncryptionJob::Error::NoSession, QStringLiteral("OMEMO local device is unavailable"));
            return job;
        }
        const auto encryptedInfo = findOmemoElement(xml);
        if (!encryptedInfo) {
            job->fail(EncryptionJob::Error::ProtocolError, QStringLiteral("Stanza does not contain OMEMO"));
            return job;
        }
        const auto protocol = encryptedInfo->protocol;
        if (!d->supportedProtocols.testFlag(protocol)) {
            job->fail(EncryptionJob::Error::Unsupported,
                      QStringLiteral("The incoming OMEMO wire profile is unavailable locally"));
            return job;
        }
        const auto encrypted      = encryptedInfo->element;
        const auto ns             = protocolNamespace(protocol);
        const auto header         = directChildNS(encrypted, QStringLiteral("header"), ns);
        const auto payloadElement = directChildNS(encrypted, QStringLiteral("payload"), ns);
        uint32_t   senderDevice   = 0;
        if (header.isNull() || !parsePositiveInt32(header.attribute(QStringLiteral("sid")), &senderDevice)) {
            job->fail(EncryptionJob::Error::ProtocolError, QStringLiteral("Malformed OMEMO encrypted stanza"));
            return job;
        }
        const Jid sender(xml.attribute(QStringLiteral("from")));
        if (!sender.isValid()) {
            job->fail(EncryptionJob::Error::ProtocolError, QStringLiteral("OMEMO stanza has no valid sender"));
            return job;
        }

        QDomElement ourKey;
        if (protocol == OmemoProtocol::Legacy) {
            for (const auto &key : directChildrenNS(header, QStringLiteral("key"), ns)) {
                uint32_t recipientDevice = 0;
                if (parsePositiveInt32(key.attribute(QStringLiteral("rid")), &recipientDevice)
                    && recipientDevice == d->data.ownDevice->id) {
                    ourKey = key;
                    break;
                }
            }
        } else {
            const auto ownBare = d->client->jid().bare();
            for (const auto &keys : directChildrenNS(header, QStringLiteral("keys"), ns)) {
                if (Jid(keys.attribute(QStringLiteral("jid"))).bare() != ownBare)
                    continue;
                for (const auto &key : directChildrenNS(keys, QStringLiteral("key"), ns)) {
                    uint32_t recipientDevice = 0;
                    if (parsePositiveInt32(key.attribute(QStringLiteral("rid")), &recipientDevice)
                        && recipientDevice == d->data.ownDevice->id) {
                        ourKey = key;
                        break;
                    }
                }
            }
        }
        if (ourKey.isNull()) {
            job->fail(EncryptionJob::Error::NoRecipients,
                      QStringLiteral("OMEMO stanza has no encrypted key for this device"));
            return job;
        }

        bool       keyOk        = false;
        const auto encryptedKey = base64Element(ourKey, &keyOk);
        QByteArray payload;
        bool       payloadOk = true;
        if (!payloadElement.isNull())
            payload = base64Element(payloadElement, &payloadOk);
        if (!keyOk || !payloadOk) {
            job->fail(EncryptionJob::Error::ProtocolError, QStringLiteral("OMEMO stanza contains invalid base64"));
            return job;
        }
        const bool keyExchange = protocol == OmemoProtocol::Legacy
            ? (ourKey.attribute(QStringLiteral("prekey")) == QLatin1String("true")
               || ourKey.attribute(QStringLiteral("prekey")) == QLatin1String("1"))
            : (ourKey.attribute(QStringLiteral("kex")) == QLatin1String("true")
               || ourKey.attribute(QStringLiteral("kex")) == QLatin1String("1"));

        EncryptionMetadata metadata;
        metadata.methodId       = OmemoEncryption::methodId();
        metadata.sender         = sender;
        metadata.senderDeviceId = senderDevice;
        metadata.details.insert(QStringLiteral("keyExchange"), keyExchange);
        metadata.details.insert(QLatin1String(OmemoProtocolOption), protocolName(protocol));

        QByteArray keyMaterial;
        uint32_t   messageCounter = 0;
        QByteArray ratchetKey;
        QString    cryptoError;
        const int  code = d->decryptKey(sender.bare(), senderDevice, protocol, keyExchange, encryptedKey, &keyMaterial,
                                        &messageCounter, &ratchetKey, &cryptoError);
        if (code != SG_SUCCESS) {
            job->fail(signalErrorToJob(code), cryptoError, metadata);
            return job;
        }

        QDomDocument restoredDocument;
        QDomElement  restoredStanza;
        bool         protocolOnly = false;
        if (protocol == OmemoProtocol::Legacy) {
            protocolOnly   = payloadElement.isNull();
            restoredStanza = restoredDocument.importNode(xml, true).toElement();
            restoredDocument.appendChild(restoredStanza);
            const auto encryptedCopy = directChildNS(restoredStanza, QStringLiteral("encrypted"), ns);
            if (!encryptedCopy.isNull())
                restoredStanza.removeChild(encryptedCopy);

            if (!payloadElement.isNull()) {
                const auto ivElement = directChildNS(header, QStringLiteral("iv"), ns);
                bool       ivOk      = false;
                const auto iv        = base64Element(ivElement, &ivOk);
                if (!ivOk || iv.size() != 12 || keyMaterial.size() < 16) {
                    job->fail(EncryptionJob::Error::ProtocolError,
                              QStringLiteral("Legacy OMEMO payload material is malformed"));
                    return job;
                }
                QByteArray key = keyMaterial.left(16);
                QByteArray tag;
                QByteArray encryptedPayload = payload;
                if (keyMaterial.size() > 16) {
                    tag = keyMaterial.mid(16);
                } else if (encryptedPayload.size() >= 16) {
                    tag = encryptedPayload.right(16);
                    encryptedPayload.chop(16);
                }
                if (tag.size() != 16) {
                    job->fail(EncryptionJob::Error::AuthenticationFailed,
                              QStringLiteral("Legacy OMEMO payload has no valid authentication tag"));
                    return job;
                }
                const auto decrypted = qcaAesGcm(false, key, iv, encryptedPayload, tag);
                if (!decrypted) {
                    job->fail(EncryptionJob::Error::AuthenticationFailed,
                              QStringLiteral("Legacy OMEMO payload authentication failed"));
                    return job;
                }
                const auto oldBody = restoredStanza.firstChildElement(QStringLiteral("body"));
                if (!oldBody.isNull())
                    restoredStanza.removeChild(oldBody);
                auto body = restoredDocument.createElement(QStringLiteral("body"));
                body.appendChild(restoredDocument.createTextNode(QString::fromUtf8(decrypted->data)));
                restoredStanza.appendChild(body);
            }
        } else if (payloadElement.isNull()) {
            protocolOnly = true;
            if (keyMaterial.size() != 32 || keyMaterial != QByteArray(32, '\0')) {
                job->fail(EncryptionJob::Error::AuthenticationFailed,
                          QStringLiteral("Malformed empty OMEMO protocol message"));
                return job;
            }
            restoredStanza = restoredDocument.importNode(xml, true).toElement();
            restoredDocument.appendChild(restoredStanza);
            const auto encryptedCopy = directChildNS(restoredStanza, QStringLiteral("encrypted"), ns);
            if (!encryptedCopy.isNull())
                restoredStanza.removeChild(encryptedCopy);
        } else {
            QByteArray envelopeBytes;
            if (!d->decryptPayload(keyMaterial, payload, &envelopeBytes, &cryptoError)) {
                job->fail(EncryptionJob::Error::AuthenticationFailed, cryptoError);
                return job;
            }
            QDomDocument envelopeDocument;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            const bool envelopeParsed = envelopeDocument.setContent(envelopeBytes, true);
#else
            const auto envelopeParsed
                = envelopeDocument.setContent(envelopeBytes, QDomDocument::ParseOption::UseNamespaceProcessing);
#endif
            if (!envelopeParsed) {
                job->fail(EncryptionJob::Error::ProtocolError,
                          QStringLiteral("Decrypted OMEMO payload is not valid XML"));
                return job;
            }

            StanzaContentEncryption::Profile profile;
            profile.affixes = StanzaContentEncryption::RandomPadding | StanzaContentEncryption::From;
            if (!xml.attribute(QStringLiteral("to")).isEmpty())
                profile.affixes |= StanzaContentEncryption::To;
            profile.minimumEnvelopeSize = d->minimumEnvelopeSize;
            QString sceError;
            auto    restored
                = StanzaContentEncryption::restore(xml, envelopeDocument.documentElement(), profile, &sceError);
            if (!restored) {
                job->fail(EncryptionJob::Error::AuthenticationFailed, sceError);
                return job;
            }
            restoredDocument = std::move(*restored);
            restoredStanza   = restoredDocument.documentElement();
        }

        auto       device          = d->data.devices.value(sender.bare()).value(senderDevice);
        auto       state           = device.protocols.value(protocol);
        const bool firstForRatchet = !ratchetKey.isEmpty() && state.lastReceivedRatchetKey != ratchetKey;
        bool       deviceChanged   = false;
        if (firstForRatchet) {
            state.lastReceivedRatchetKey = ratchetKey;
            device.protocols.insert(protocol, state);
            deviceChanged = true;
        }
        // Incoming messages already give us the sender's authenticated identity key.  Use it to verify a label
        // learned from the OMEMO 2 device list before the UI asks the user to trust this device.
        const auto wireIdentity = d->wireIdentityFromStored(state.keyId, OmemoProtocol::Omemo2);
        if (protocol == OmemoProtocol::Omemo2 && !device.labelVerified && !device.label.isEmpty()
            && !device.labelSignature.isEmpty() && !wireIdentity.isEmpty()) {
            device.labelVerified = d->verifyDeviceLabel(wireIdentity, device.label, device.labelSignature);
            deviceChanged        = true;
        }
        if (deviceChanged) {
            if (!d->setDevice(sender.bare(), senderDevice, device)) {
                job->fail(EncryptionJob::Error::StorageError, QStringLiteral("Could not persist OMEMO device state"));
                return job;
            }
        }
        if (!wireIdentity.isEmpty() && d->trustLevel(sender.bare(), wireIdentity) == EncryptionTrustLevel::Undecided
            && d->newIdentityTrust != EncryptionTrustLevel::Undecided) {
            if (!d->trustStorage->setTrustLevel(OmemoEncryption::methodId(), sender.bare(), wireIdentity,
                                                d->newIdentityTrust)) {
                job->fail(EncryptionJob::Error::StorageError, QStringLiteral("Could not persist OMEMO trust state"));
                return job;
            }
            emit method_->trustChanged(sender.bare(), wireIdentity, d->newIdentityTrust);
        }
        metadata.senderKey    = wireIdentity;
        metadata.protocolOnly = protocolOnly;
        metadata.details.insert(QStringLiteral("trustLevel"),
                                static_cast<int>(d->trustLevel(sender.bare(), wireIdentity)));
        metadata.details.insert(QStringLiteral("empty"), protocolOnly);
        metadata.details.insert(QStringLiteral("ratchetCounter"), messageCounter);
        job->complete(restoredStanza, metadata);

        if (protocol == OmemoProtocol::Omemo2) {
            const bool heartbeatRequired = firstForRatchet && messageCounter >= 53;
            if (keyExchange || heartbeatRequired) {
                QTimer::singleShot(0, method_, [method = method_, sender, senderDevice]() {
                    auto ack = method->sendEmptyMessage(sender.bare(), senderDevice);
                    if (!ack->success() && ack->isFinished())
                        emit method->warning(
                            QStringLiteral("OMEMO key-exchange acknowledgement failed: %1").arg(ack->errorString()));
                    else if (!ack->isFinished()) {
                        QObject::connect(ack, &EncryptionJob::finished, method, [method, ack]() {
                            if (!ack->success())
                                emit method->warning(QStringLiteral("OMEMO key-exchange acknowledgement failed: %1")
                                                         .arg(ack->errorString()));
                            ack->deleteLater();
                        });
                        return;
                    }
                    ack->deleteLater();
                });
            }
        }
        return job;
    }

private:
    OmemoEncryption *method_ = nullptr;
};

QString OmemoEncryption::methodId() { return QStringLiteral("omemo"); }
QString OmemoEncryption::namespaceUri() { return QString::fromLatin1(OmemoNs); }
QString OmemoEncryption::devicesNode() { return QString::fromLatin1(DevicesNode); }
QString OmemoEncryption::bundlesNode() { return QString::fromLatin1(BundlesNode); }
QString OmemoEncryption::legacyNamespaceUri() { return QString::fromLatin1(LegacyOmemoNs); }
QString OmemoEncryption::legacyDevicesNode() { return QString::fromLatin1(LegacyDevicesNode); }
QString OmemoEncryption::legacyBundleNode(uint32_t deviceId)
{
    return protocolBundleNode(OmemoProtocol::Legacy, deviceId);
}

OmemoEncryption::OmemoEncryption(Client *client, OmemoStorage *storage, EncryptionTrustStorage *trustStorage,
                                 QObject *parent) :
    EncryptionMethod(parent ? parent : client), d(std::make_unique<Private>(this, client, storage, trustStorage))
{
    if (!client) {
        emit warning(QStringLiteral("OMEMO requires an XMPP Client"));
        return;
    }
    client->encryptionManager()->registerMethod(this);
    connect(client->pubSubManager(), &PubSubManager::itemPublished, this,
            [this](const Jid &service, const QString &node, const PubSubItem &item) {
                std::optional<OmemoProtocol> protocol;
                if (node == devicesNode())
                    protocol = OmemoProtocol::Omemo2;
                else if (node == legacyDevicesNode())
                    protocol = OmemoProtocol::Legacy;
                if (!protocol)
                    return;

                const auto owner = service.bare();
                if (owner.isEmpty())
                    return;
                const auto ns                 = protocolNamespace(*protocol);
                bool       ownPresentInUpdate = true;
                if (d->data.ownDevice && owner == d->client->jid().bare()) {
                    ownPresentInUpdate = false;
                    for (const auto &device : directChildrenNS(item.payload(), QStringLiteral("device"), ns)) {
                        uint32_t id = 0;
                        if (parsePositiveInt32(device.attribute(QStringLiteral("id")), &id)
                            && id == d->data.ownDevice->id) {
                            ownPresentInUpdate = true;
                            break;
                        }
                    }
                }
                QString error;
                if (!d->mergeDeviceList(owner, *protocol, item.payload(), &error)) {
                    emit warning(error);
                    return;
                }
                if (d->data.ownDevice && owner == d->client->jid().bare() && !ownPresentInUpdate) {
                    auto job = publishOwnDevice(*protocol);
                    connect(job, &EncryptionJob::finished, this, [this, job, protocol]() {
                        if (!job->success())
                            emit warning(QStringLiteral("%1 OMEMO device reannouncement failed: %2")
                                             .arg(protocolName(*protocol), job->errorString()));
                        job->deleteLater();
                    });
                }
            });
}

OmemoEncryption::~OmemoEncryption()
{
    if (d && d->client && d->client->encryptionManager())
        d->client->encryptionManager()->unregisterMethod(this);
}

QString                        OmemoEncryption::id() const { return methodId(); }
QString                        OmemoEncryption::name() const { return QStringLiteral("OMEMO"); }
EncryptionMethod::Capabilities OmemoEncryption::capabilities() const { return XmppStanza; }
EncryptedSession              *OmemoEncryption::startSession(Capabilities caps, const EncryptionContext &context)
{
    if (!caps.testFlag(XmppStanza))
        return nullptr;
    return new OmemoEncryptedSession(this, context);
}
Features OmemoEncryption::features() const
{
    Features result;
    if (d->supportedProtocols.testFlag(OmemoProtocol::Omemo2)) {
        result.addFeature(namespaceUri());
        result.addFeature(devicesNode() + QStringLiteral("+notify"));
    }
    if (d->supportedProtocols.testFlag(OmemoProtocol::Legacy))
        result.addFeature(legacyDevicesNode() + QStringLiteral("+notify"));
    return result;
}

bool OmemoEncryption::canDecrypt(const QDomElement &stanza) const
{
    const auto encrypted = findOmemoElement(stanza);
    return encrypted && d->supportedProtocols.testFlag(encrypted->protocol);
}

OmemoProtocols OmemoEncryption::supportedProtocols() const { return d->supportedProtocols; }

OmemoProtocols OmemoEncryption::protocolsFor(const Jid &fullJid) const
{
    OmemoProtocols result;
    if (!d->client || !fullJid.isValid() || fullJid.resource().isEmpty())
        return result;
    const auto features = d->client->capsManager()->features(fullJid);
    if (d->supportedProtocols.testFlag(OmemoProtocol::Omemo2)
        && (features.test(namespaceUri()) || features.test(devicesNode() + QStringLiteral("+notify")))) {
        result |= OmemoProtocol::Omemo2;
    }
    if (d->supportedProtocols.testFlag(OmemoProtocol::Legacy)
        && features.test(legacyDevicesNode() + QStringLiteral("+notify"))) {
        result |= OmemoProtocol::Legacy;
    }
    return result;
}

std::optional<OmemoProtocol> OmemoEncryption::preferredProtocolFor(const Jid &fullJid) const
{
    const auto protocols = protocolsFor(fullJid);
    if (protocols.testFlag(OmemoProtocol::Omemo2))
        return OmemoProtocol::Omemo2;
    if (protocols.testFlag(OmemoProtocol::Legacy))
        return OmemoProtocol::Legacy;
    return std::nullopt;
}

bool       OmemoEncryption::isReady() const { return d->ready; }
uint32_t   OmemoEncryption::ownDeviceId() const { return d->data.ownDevice ? d->data.ownDevice->id : 0; }
QByteArray OmemoEncryption::ownIdentityKey() const { return d->ownWireIdentity(OmemoProtocol::Omemo2); }
QString    OmemoEncryption::ownDeviceLabel() const { return d->data.ownDevice ? d->data.ownDevice->label : QString(); }

QList<OmemoDeviceInfo> OmemoEncryption::devices(const Jid &owner) const
{
    QList<OmemoDeviceInfo> result;
    const auto             appendOwner
        = [this, &result](const QString &bare, const QHash<uint32_t, OmemoStorage::Device> &devices) {
              for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
                  OmemoDeviceInfo info;
                  info.owner  = Jid(bare);
                  info.id     = it.key();
                  info.label  = it->labelVerified ? it->label : QString();
                  info.active = Private::deviceActive(*it);
                  for (auto state = it->protocols.cbegin(); state != it->protocols.cend(); ++state) {
                      info.protocols |= state.key();
                      info.hasSession = info.hasSession || !state->session.isEmpty();
                  }
                  const auto modernState    = it->protocols.value(OmemoProtocol::Omemo2);
                  const auto legacyState    = it->protocols.value(OmemoProtocol::Legacy);
                  const auto storedIdentity = !modernState.keyId.isEmpty() ? modernState.keyId : legacyState.keyId;
                  info.identityKey          = d->wireIdentityFromStored(storedIdentity, OmemoProtocol::Omemo2);
                  info.trust                = d->trustLevel(bare, info.identityKey);
                  result.append(info);
              }
          };
    if (owner.isValid()) {
        const auto bare = owner.bare();
        appendOwner(bare, d->data.devices.value(bare));
    } else {
        for (auto it = d->data.devices.cbegin(); it != d->data.devices.cend(); ++it)
            appendOwner(it.key(), it.value());
    }
    return result;
}

EncryptionTrustLevels OmemoEncryption::acceptedSessionBuildingTrustLevels() const { return d->acceptedTrust; }
void OmemoEncryption::setAcceptedSessionBuildingTrustLevels(EncryptionTrustLevels levels) { d->acceptedTrust = levels; }
EncryptionTrustLevel OmemoEncryption::newIdentityTrustLevel() const { return d->newIdentityTrust; }
void OmemoEncryption::setNewIdentityTrustLevel(EncryptionTrustLevel level) { d->newIdentityTrust = level; }
EncryptionTrustLevel OmemoEncryption::trustLevel(const Jid &owner, const QByteArray &identityKey) const
{
    return d->trustLevel(owner.bare(), identityKey);
}
bool OmemoEncryption::setTrustLevel(const Jid &owner, const QByteArray &identityKey, EncryptionTrustLevel level)
{
    if (!d->trustStorage->setTrustLevel(methodId(), owner.bare(), identityKey, level))
        return false;
    emit trustChanged(owner.bare(), identityKey, level);
    return true;
}

int  OmemoEncryption::minimumEnvelopeSize() const { return d->minimumEnvelopeSize; }
void OmemoEncryption::setMinimumEnvelopeSize(int bytes) { d->minimumEnvelopeSize = std::max(0, bytes); }

EncryptionJob *OmemoEncryption::setUp(const QString &deviceLabel)
{
    auto job = new EncryptionJob(this);
    if (!d->signalContext || d->supportedProtocols == OmemoProtocols()) {
        job->fail(EncryptionJob::Error::Unsupported,
                  QStringLiteral("OMEMO is unavailable because its cryptographic backend did not initialize"));
        return job;
    }
    const bool freshIdentity = !d->data.ownDevice;
    QString    error;
    if (!d->ensureLocalKeyMaterial(deviceLabel, &error)) {
        job->fail(EncryptionJob::Error::StorageError, error);
        return job;
    }

    QPointer<EncryptionJob> guarded(job);
    auto                    publish = std::make_shared<std::function<void()>>();
    *publish                        = [this, guarded]() {
        if (!guarded)
            return;
        auto bundleJob = publishOwnBundle();
        connect(bundleJob, &EncryptionJob::finished, this, [this, bundleJob, guarded]() {
            if (!guarded)
                return;
            if (!bundleJob->success()) {
                guarded->fail(bundleJob->error(), bundleJob->errorString());
                bundleJob->deleteLater();
                return;
            }
            bundleJob->deleteLater();
            auto deviceJob = publishOwnDevice();
            connect(deviceJob, &EncryptionJob::finished, this, [deviceJob, guarded]() {
                if (!guarded)
                    return;
                if (deviceJob->success())
                    guarded->complete(QByteArray());
                else
                    guarded->fail(deviceJob->error(), deviceJob->errorString());
                deviceJob->deleteLater();
            });
        });
    };

    if (!freshIdentity) {
        (*publish)();
        return job;
    }

    const auto self                     = d->client->jid().bare();
    auto       checkCollisionAndPublish = [this, guarded, deviceLabel, self, publish]() {
        if (!guarded)
            return;

        constexpr int MaxCollisionRetries = 32;
        for (int attempt = 0; attempt < MaxCollisionRetries; ++attempt) {
            if (!d->data.ownDevice) {
                guarded->fail(EncryptionJob::Error::StorageError, QStringLiteral("OMEMO local device is unavailable"));
                return;
            }
            const auto known    = d->data.devices.value(self);
            const auto existing = known.constFind(d->data.ownDevice->id);
            bool collision = existing != known.cend() && Private::protocolActive(*existing, OmemoProtocol::Omemo2);
            if (d->supportedProtocols.testFlag(OmemoProtocol::Legacy))
                collision = collision
                    || (existing != known.cend() && Private::protocolActive(*existing, OmemoProtocol::Legacy));
            if (!collision) {
                (*publish)();
                return;
            }

            QString regenerateError;
            if (!d->clearLocalKeyMaterial(&regenerateError)
                || !d->ensureLocalKeyMaterial(deviceLabel, &regenerateError)) {
                guarded->fail(EncryptionJob::Error::StorageError, regenerateError);
                return;
            }
        }
        guarded->fail(EncryptionJob::Error::CryptoError, QStringLiteral("Could not generate a unique OMEMO device id"));
    };

    d->fetchDeviceList(self, OmemoProtocol::Omemo2,
                       [this, guarded, self, checkCollisionAndPublish](bool modernOk, const QString &modernError) {
                           if (!guarded)
                               return;
                           if (!modernOk) {
                               guarded->fail(EncryptionJob::Error::NetworkError, modernError);
                               return;
                           }
                           if (!d->supportedProtocols.testFlag(OmemoProtocol::Legacy)) {
                               checkCollisionAndPublish();
                               return;
                           }
                           d->fetchDeviceList(
                               self, OmemoProtocol::Legacy,
                               [guarded, checkCollisionAndPublish](bool legacyOk, const QString &legacyError) {
                                   if (!guarded)
                                       return;
                                   if (!legacyOk) {
                                       guarded->fail(EncryptionJob::Error::NetworkError, legacyError);
                                       return;
                                   }
                                   checkCollisionAndPublish();
                               });
                       });
    return job;
}

EncryptionJob *OmemoEncryption::refreshDevices(const Jid &owner, OmemoProtocol protocol, bool fetchBundles)
{
    auto job = new EncryptionJob(this);
    if (!d->supportedProtocols.testFlag(protocol)) {
        job->fail(EncryptionJob::Error::Unsupported,
                  QStringLiteral("The %1 OMEMO wire profile is unavailable locally").arg(protocolName(protocol)));
        return job;
    }
    const auto bare = owner.isValid() ? owner.bare() : d->client->jid().bare();
    d->fetchDeviceList(bare, protocol, [this, job, bare, protocol, fetchBundles](bool ok, const QString &error) {
        if (!ok) {
            job->fail(EncryptionJob::Error::NetworkError, error);
            return;
        }
        if (!fetchBundles) {
            job->complete(QByteArray());
            return;
        }
        const auto targets = d->activeDevicesFor(bare, protocol);
        d->ensureSessions(targets, protocol, 0, false,
                          [job](bool sessionsOk, EncryptionJob::Error jobError, const QString &message,
                                QList<QPair<QString, uint32_t>>, QStringList) {
                              if (sessionsOk)
                                  job->complete(QByteArray());
                              else
                                  job->fail(jobError, message);
                          });
    });
    return job;
}

EncryptionJob *OmemoEncryption::refreshDevices(const Jid &owner, bool fetchBundles)
{
    return refreshDevices(owner, OmemoProtocol::Omemo2, fetchBundles);
}

EncryptionJob *OmemoEncryption::refreshBundle(const Jid &owner, uint32_t deviceId, OmemoProtocol protocol,
                                              bool buildSession)
{
    auto job = new EncryptionJob(this);
    if (!d->supportedProtocols.testFlag(protocol)) {
        job->fail(EncryptionJob::Error::Unsupported,
                  QStringLiteral("The %1 OMEMO wire profile is unavailable locally").arg(protocolName(protocol)));
        return job;
    }
    if (!owner.isValid() || deviceId == 0) {
        job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("Invalid OMEMO bundle address"));
        return job;
    }
    d->fetchBundle(owner.bare(), deviceId, protocol, buildSession,
                   [job](bool ok, EncryptionJob::Error jobError, const QString &message) {
                       if (ok)
                           job->complete(QByteArray());
                       else
                           job->fail(jobError, message);
                   });
    return job;
}

EncryptionJob *OmemoEncryption::refreshBundle(const Jid &owner, uint32_t deviceId, bool buildSession)
{
    return refreshBundle(owner, deviceId, OmemoProtocol::Omemo2, buildSession);
}

EncryptionJob *OmemoEncryption::publishOwnBundle(OmemoProtocol protocol)
{
    auto job = new EncryptionJob(this);
    if (!d->supportedProtocols.testFlag(protocol)) {
        job->fail(EncryptionJob::Error::Unsupported,
                  QStringLiteral("The %1 OMEMO wire profile is unavailable locally").arg(protocolName(protocol)));
        return job;
    }
    QString    error;
    const auto document = d->makeBundleDocument(protocol, &error);
    if (document.documentElement().isNull() || !d->data.ownDevice) {
        job->fail(EncryptionJob::Error::NoSession, error);
        return job;
    }
    PubSubOptions options;
    if (protocol == OmemoProtocol::Omemo2) {
        options.insert(QStringLiteral("pubsub#access_model"), { QStringLiteral("open") });
        options.insert(QStringLiteral("pubsub#max_items"), { QStringLiteral("max") });
    }
    const QString    itemId = protocol == OmemoProtocol::Omemo2 ? QString::number(d->data.ownDevice->id) : QString();
    const PubSubItem item(itemId, document.documentElement());
    d->publishPepItem(protocolBundleNode(protocol, d->data.ownDevice->id), item, options,
                      [job, protocol](bool ok, const QString &publishError) {
                          if (ok)
                              job->complete(QByteArray());
                          else
                              job->fail(
                                  EncryptionJob::Error::NetworkError,
                                  publishError.isEmpty()
                                      ? QStringLiteral("Could not publish %1 OMEMO bundle").arg(protocolName(protocol))
                                      : publishError);
                      });
    return job;
}

EncryptionJob *OmemoEncryption::publishOwnDevice(OmemoProtocol protocol)
{
    auto job = new EncryptionJob(this);
    if (!d->supportedProtocols.testFlag(protocol)) {
        job->fail(EncryptionJob::Error::Unsupported,
                  QStringLiteral("The %1 OMEMO wire profile is unavailable locally").arg(protocolName(protocol)));
        return job;
    }
    if (!d->data.ownDevice) {
        job->fail(EncryptionJob::Error::NoSession, QStringLiteral("OMEMO local device is unavailable"));
        return job;
    }
    const auto    document = d->makeDeviceListDocument(protocol);
    PubSubOptions options;
    if (protocol == OmemoProtocol::Omemo2)
        options.insert(QStringLiteral("pubsub#access_model"), { QStringLiteral("open") });
    const QString    itemId = protocol == OmemoProtocol::Omemo2 ? QStringLiteral("current") : QString();
    const PubSubItem item(itemId, document.documentElement());
    d->publishPepItem(
        protocolDevicesNode(protocol), item, options, [this, job, protocol](bool ok, const QString &publishError) {
            if (ok) {
                d->fetchedDeviceLists.insert(Private::fetchedListKey(d->client->jid().bare(), protocol));
                job->complete(QByteArray());
            } else {
                job->fail(EncryptionJob::Error::NetworkError,
                          publishError.isEmpty()
                              ? QStringLiteral("Could not publish %1 OMEMO device list").arg(protocolName(protocol))
                              : publishError);
            }
        });
    return job;
}

EncryptionJob *OmemoEncryption::publishOwnBundle()
{
    auto job    = new EncryptionJob(this);
    auto modern = publishOwnBundle(OmemoProtocol::Omemo2);
    connect(modern, &EncryptionJob::finished, this, [this, job, modern]() {
        if (!modern->success()) {
            job->fail(modern->error(), modern->errorString());
            modern->deleteLater();
            return;
        }
        modern->deleteLater();
        if (!d->supportedProtocols.testFlag(OmemoProtocol::Legacy)) {
            job->complete(QByteArray());
            return;
        }
        auto legacy = publishOwnBundle(OmemoProtocol::Legacy);
        connect(legacy, &EncryptionJob::finished, this, [job, legacy]() {
            if (legacy->success())
                job->complete(QByteArray());
            else
                job->fail(legacy->error(), legacy->errorString());
            legacy->deleteLater();
        });
    });
    return job;
}

EncryptionJob *OmemoEncryption::publishOwnDevice()
{
    auto job    = new EncryptionJob(this);
    auto modern = publishOwnDevice(OmemoProtocol::Omemo2);
    connect(modern, &EncryptionJob::finished, this, [this, job, modern]() {
        if (!modern->success()) {
            job->fail(modern->error(), modern->errorString());
            modern->deleteLater();
            return;
        }
        modern->deleteLater();
        if (!d->supportedProtocols.testFlag(OmemoProtocol::Legacy)) {
            job->complete(QByteArray());
            return;
        }
        auto legacy = publishOwnDevice(OmemoProtocol::Legacy);
        connect(legacy, &EncryptionJob::finished, this, [job, legacy]() {
            if (legacy->success())
                job->complete(QByteArray());
            else
                job->fail(legacy->error(), legacy->errorString());
            legacy->deleteLater();
        });
    });
    return job;
}

EncryptionJob *OmemoEncryption::sanitizeOwnPep()
{
    auto *job = new EncryptionJob(this);
    if (!d->data.ownDevice || !isReady()) {
        job->fail(EncryptionJob::Error::NoSession, QStringLiteral("OMEMO has not been set up locally"));
        return job;
    }

    const auto ownBare = d->client->jid().bare();
    d->fetchDeviceList(ownBare, OmemoProtocol::Omemo2, [this, job, ownBare](bool listOk, const QString &listError) {
        if (!listOk) {
            job->fail(EncryptionJob::Error::NetworkError, listError);
            return;
        }

        auto       targets   = d->activeDevicesFor(ownBare, OmemoProtocol::Omemo2);
        const auto ownTarget = qMakePair(ownBare, d->data.ownDevice->id);
        if (!targets.contains(ownTarget))
            targets.append(ownTarget);
        auto                                                next = std::make_shared<std::function<void(qsizetype)>>();
        const std::weak_ptr<std::function<void(qsizetype)>> weakNext = next;
        *next = [this, job, ownBare, targets, weakNext](qsizetype index) {
            const auto next = weakNext.lock();
            if (!next) {
                job->fail(EncryptionJob::Error::Cancelled, QStringLiteral("OMEMO PEP sanitization was cancelled"));
                return;
            }
            if (index == targets.size()) {
                // Also repair a device list from which the current device was lost.
                auto publish = publishOwnDevice(OmemoProtocol::Omemo2);
                connect(publish, &EncryptionJob::finished, this, [job, publish]() {
                    if (publish->success())
                        job->complete(QByteArray());
                    else
                        job->fail(publish->error(), publish->errorString());
                    publish->deleteLater();
                });
                return;
            }

            const auto target = targets.at(index);
            d->fetchBundle(
                ownBare, target.second, OmemoProtocol::Omemo2, false,
                [this, job, ownBare, target, index, next](bool ok, EncryptionJob::Error error, const QString &message) {
                    if (ok) {
                        (*next)(index + 1);
                        return;
                    }
                    if (error != EncryptionJob::Error::ProtocolError) {
                        job->fail(error, message);
                        return;
                    }

                    if (target.second == d->data.ownDevice->id) {
                        auto repair = publishOwnBundle(OmemoProtocol::Omemo2);
                        connect(repair, &EncryptionJob::finished, this, [job, repair, index, next]() {
                            if (repair->success())
                                (*next)(index + 1);
                            else
                                job->fail(repair->error(), repair->errorString());
                            repair->deleteLater();
                        });
                        return;
                    }

                    // Device bundles are items in a shared OMEMO 2 node.  Retracting a broken
                    // item prevents stale public key material from lingering; a missing item is
                    // already in the desired state, so ItemNotFound is harmless.
                    auto retract = d->client->pubSubManager()->retract(
                        Jid(ownBare), protocolBundleNode(OmemoProtocol::Omemo2, target.second),
                        QString::number(target.second));
                    connect(retract, &Task::finished, this, [this, job, retract, target, index, next]() {
                        if (!retract->success()
                            && retract->error().condition != Stanza::Error::ErrorCond::ItemNotFound) {
                            const auto error = retract->statusString();
                            retract->deleteLater();
                            job->fail(EncryptionJob::Error::NetworkError,
                                      error.isEmpty() ? QStringLiteral("Could not retract broken OMEMO bundle")
                                                      : error);
                            return;
                        }
                        retract->deleteLater();
                        auto retire = retireOwnDevice(target.second);
                        connect(retire, &EncryptionJob::finished, this, [job, retire, index, next]() {
                            if (retire->success())
                                (*next)(index + 1);
                            else
                                job->fail(retire->error(), retire->errorString());
                            retire->deleteLater();
                        });
                    });
                });
        };
        (*next)(0);
    });
    return job;
}

EncryptionJob *OmemoEncryption::retireOwnDevice(uint32_t deviceId)
{
    auto *job = new EncryptionJob(this);
    if (!d->data.ownDevice || deviceId == 0) {
        job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("Invalid OMEMO device id"));
        return job;
    }
    if (deviceId == d->data.ownDevice->id) {
        job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("The current OMEMO device cannot be retired"));
        return job;
    }

    const auto ownBare = d->client->jid().bare();
    const auto known   = d->data.devices.value(ownBare);
    const auto device  = known.constFind(deviceId);
    if (device == known.cend()) {
        job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("OMEMO device is not known for this account"));
        return job;
    }

    QList<OmemoProtocol> protocols;
    for (const auto protocol : { OmemoProtocol::Omemo2, OmemoProtocol::Legacy }) {
        if (Private::protocolActive(*device, protocol))
            protocols.append(protocol);
    }
    if (protocols.isEmpty()) {
        job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("OMEMO device is already retired"));
        return job;
    }

    auto publishNext = std::make_shared<std::function<void(qsizetype)>>();
    const std::weak_ptr<std::function<void(qsizetype)>> weakPublishNext = publishNext;
    *publishNext = [this, job, deviceId, protocols, weakPublishNext](qsizetype index) {
        const auto publishNext = weakPublishNext.lock();
        if (!publishNext) {
            job->fail(EncryptionJob::Error::Cancelled, QStringLiteral("OMEMO device retirement was cancelled"));
            return;
        }
        if (index == protocols.size()) {
            job->complete(QByteArray());
            return;
        }

        const auto    protocol = protocols.at(index);
        const auto    document = d->makeDeviceListDocument(protocol, deviceId);
        PubSubOptions options;
        if (protocol == OmemoProtocol::Omemo2)
            options.insert(QStringLiteral("pubsub#access_model"), { QStringLiteral("open") });
        const QString itemId = protocol == OmemoProtocol::Omemo2 ? QStringLiteral("current") : QString();
        d->publishPepItem(
            protocolDevicesNode(protocol), PubSubItem(itemId, document.documentElement()), options,
            [this, job, deviceId, protocol, index, publishNext](bool ok, const QString &publishError) {
                if (!ok) {
                    job->fail(EncryptionJob::Error::NetworkError,
                              publishError.isEmpty()
                                  ? QStringLiteral("Could not update %1 OMEMO device list").arg(protocolName(protocol))
                                  : publishError);
                    return;
                }
                QString error;
                if (!d->markOwnDeviceRetired(deviceId, { protocol }, &error)) {
                    job->fail(EncryptionJob::Error::StorageError, error);
                    return;
                }
                (*publishNext)(index + 1);
            });
    };
    (*publishNext)(0);
    return job;
}

EncryptionJob *OmemoEncryption::prepareDecryptionRecovery(const EncryptionMetadata &metadata)
{
    auto                    job = new EncryptionJob(this);
    QPointer<EncryptionJob> guarded(job);
    d->resolveRecoveryTarget(
        metadata, false,
        [guarded](bool ok, EncryptionJob::Error error, const QString &message, const EncryptionMetadata &prepared) {
            if (!guarded)
                return;
            if (ok)
                guarded->complete(QByteArray(), prepared);
            else
                guarded->fail(error, message, prepared);
        });
    return job;
}

EncryptionJob *OmemoEncryption::recoverDecryption(const EncryptionMetadata &metadata)
{
    auto                    job = new EncryptionJob(this);
    QPointer<EncryptionJob> guarded(job);
    d->resolveRecoveryTarget(
        metadata, true,
        [this, guarded](bool ok, EncryptionJob::Error error, const QString &message,
                        const EncryptionMetadata &prepared) {
            if (!guarded)
                return;
            if (!ok) {
                guarded->fail(error, message, prepared);
                return;
            }

            const auto protocol = protocolFromName(prepared.details.value(QLatin1String(OmemoProtocolOption)));
            if (!protocol) {
                guarded->fail(EncryptionJob::Error::Unsupported,
                              QStringLiteral("OMEMO recovery context has no supported wire profile"), prepared);
                return;
            }
            auto exchange = sendProtocolMessage(prepared.sender, prepared.senderDeviceId, *protocol, true);
            if (exchange->success())
                guarded->complete(exchange->stanza(), exchange->metadata());
            else
                guarded->fail(exchange->error(), exchange->errorString(), prepared);
            exchange->deleteLater();
        });
    return job;
}

EncryptionJob *OmemoEncryption::sendProtocolMessage(const Jid &recipient, uint32_t deviceId, OmemoProtocol protocol,
                                                    bool requireKeyExchange)
{
    auto       job  = new EncryptionJob(this);
    const auto bare = recipient.bare();
    if (!d->data.ownDevice || !recipient.isValid() || bare.isEmpty() || deviceId == 0) {
        job->fail(EncryptionJob::Error::InvalidInput, QStringLiteral("Invalid OMEMO protocol-message recipient"));
        return job;
    }
    if (!d->supportedProtocols.testFlag(protocol)) {
        job->fail(EncryptionJob::Error::Unsupported,
                  QStringLiteral("The requested OMEMO wire profile is unavailable locally"));
        return job;
    }
    if (!d->hasSession(bare, deviceId, protocol)) {
        job->fail(EncryptionJob::Error::NoSession, QStringLiteral("No existing OMEMO session for protocol message"));
        return job;
    }

    Private::EncryptedKey encryptedKey;
    QString               error;
    const QByteArray      keyMaterial = protocol == OmemoProtocol::Omemo2 ? QByteArray(32, '\0') : randomBytes(16);
    if (keyMaterial.isEmpty()) {
        job->fail(EncryptionJob::Error::CryptoError, QStringLiteral("Could not generate OMEMO protocol material"));
        return job;
    }
    const int code = d->encryptKey(bare, deviceId, protocol, keyMaterial, &encryptedKey, &error);
    if (code != SG_SUCCESS) {
        job->fail(signalErrorToJob(code), error);
        return job;
    }
    if (requireKeyExchange && !encryptedKey.keyExchange) {
        job->fail(EncryptionJob::Error::ProtocolError,
                  QStringLiteral("Rebuilt OMEMO session did not produce a pre-key message"));
        return job;
    }

    QDomDocument document;
    auto         message = document.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("message"));
    message.setAttribute(QStringLiteral("to"), recipient.resource().isEmpty() ? bare : recipient.full());
    if (d->client->jid().isValid())
        message.setAttribute(QStringLiteral("from"), d->client->jid().full());
    document.appendChild(message);

    const auto ns        = protocolNamespace(protocol);
    auto       encrypted = document.createElementNS(ns, QStringLiteral("encrypted"));
    auto       header    = document.createElementNS(ns, QStringLiteral("header"));
    header.setAttribute(QStringLiteral("sid"), QString::number(d->data.ownDevice->id));
    if (protocol == OmemoProtocol::Legacy) {
        const auto iv = randomBytes(12);
        if (iv.size() != 12) {
            job->fail(EncryptionJob::Error::CryptoError, QStringLiteral("Could not generate legacy OMEMO IV"));
            return job;
        }
        appendBase64(document, header, QStringLiteral("iv"), iv, ns);
        auto key = document.createElementNS(ns, QStringLiteral("key"));
        key.setAttribute(QStringLiteral("rid"), QString::number(deviceId));
        if (encryptedKey.keyExchange)
            key.setAttribute(QStringLiteral("prekey"), QStringLiteral("true"));
        key.appendChild(document.createTextNode(QString::fromLatin1(encryptedKey.data.toBase64())));
        header.appendChild(key);
    } else {
        auto keys = document.createElementNS(ns, QStringLiteral("keys"));
        keys.setAttribute(QStringLiteral("jid"), bare);
        auto key = document.createElementNS(ns, QStringLiteral("key"));
        key.setAttribute(QStringLiteral("rid"), QString::number(deviceId));
        if (encryptedKey.keyExchange)
            key.setAttribute(QStringLiteral("kex"), QStringLiteral("true"));
        key.appendChild(document.createTextNode(QString::fromLatin1(encryptedKey.data.toBase64())));
        keys.appendChild(key);
        header.appendChild(keys);
    }
    encrypted.appendChild(header);
    message.appendChild(encrypted);
    message.appendChild(document.createElementNS(QLatin1String(HintsNs), QStringLiteral("store")));
    auto eme = document.createElementNS(QLatin1String(EmeNs), QStringLiteral("encryption"));
    eme.setAttribute(QStringLiteral("namespace"), ns);
    eme.setAttribute(QStringLiteral("name"), QStringLiteral("OMEMO"));
    message.appendChild(eme);

    d->client->send(message);
    EncryptionMetadata metadata;
    metadata.methodId     = methodId();
    metadata.protocolOnly = true;
    metadata.details.insert(QStringLiteral("empty"), true);
    metadata.details.insert(QStringLiteral("keyExchange"), encryptedKey.keyExchange);
    metadata.details.insert(QLatin1String(OmemoProtocolOption), protocolName(protocol));
    job->complete(message, metadata);
    return job;
}

EncryptionJob *OmemoEncryption::sendEmptyMessage(const Jid &recipient, uint32_t deviceId)
{
    return sendProtocolMessage(recipient, deviceId, OmemoProtocol::Omemo2, false);
}

bool OmemoEncryption::resetAllLocally()
{
    if (!d->storage->resetAll())
        return false;
    d->data = d->storage->allData();
    d->fetchedDeviceLists.clear();
    d->updateReady();
    return true;
}

} // namespace XMPP
