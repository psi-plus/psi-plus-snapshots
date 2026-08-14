/*
 * xmpp_omemostorage.h - persistent state interface for XEP-0384 OMEMO
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef XMPP_OMEMOSTORAGE_H
#define XMPP_OMEMOSTORAGE_H

#include <QByteArray>
#include <QDateTime>
#include <QFlags>
#include <QHash>
#include <QMap>
#include <QString>

#include <cstdint>
#include <optional>

namespace XMPP {

/**
 * OMEMO wire protocol generations supported by Iris.
 *
 * Legacy is the pre-OMEMO-2 protocol used by Psi's historical OMEMO plugin
 * (eu.siacs.conversations.axolotl, XEP-0384 0.3.x / Signal protocol v2/v3).
 * Omemo2 is urn:xmpp:omemo:2 (XEP-0384 0.4+ / libomemo-c protocol v4).
 */
enum class OmemoProtocol : quint8 { Legacy = 0x01, Omemo2 = 0x02 };
Q_DECLARE_FLAGS(OmemoProtocols, OmemoProtocol)
Q_DECLARE_OPERATORS_FOR_FLAGS(OmemoProtocols)

/**
 * Persistent state used by the OMEMO Double Ratchet engine.
 *
 * The interface is synchronous on purpose: libomemo-c storage callbacks are
 * synchronous as well. Implementations should normally keep an in-memory
 * authoritative copy and persist mutations atomically, as AnyKeep already does
 * for its QXmpp-backed OMEMO storage.
 */
class OmemoStorage {
public:
    struct OwnDevice {
        uint32_t   id = 0;
        QString    label;
        QByteArray privateIdentityKey;
        QByteArray publicIdentityKey;
        uint32_t   latestSignedPreKeyId = 1;
        uint32_t   latestPreKeyId       = 1;
    };

    /**
     * Protocol-specific state for one remote OMEMO device.
     *
     * A dual-stack device may use the same OMEMO device id for legacy and
     * OMEMO 2 while maintaining independent Double Ratchet sessions. Keeping
     * these blobs separate is therefore required for correct interoperation.
     */
    struct DeviceProtocolState {
        QByteArray keyId;
        QByteArray session;
        QByteArray lastReceivedRatchetKey;
        int        unrespondedSentStanzasCount     = 0;
        int        unrespondedReceivedStanzasCount = 0;
        QDateTime  removalFromDeviceListDate;
    };

    struct Device {
        QString                                  label;
        QByteArray                               labelSignature;
        bool                                     labelVerified = false;
        QMap<OmemoProtocol, DeviceProtocolState> protocols;
    };

    struct SignedPreKeyPair {
        QDateTime  creationDate;
        QByteArray data;
    };

    struct OmemoData {
        std::optional<OwnDevice>                ownDevice;
        QHash<uint32_t, SignedPreKeyPair>       signedPreKeyPairs;
        QHash<uint32_t, QByteArray>             preKeyPairs;
        QHash<QString, QHash<uint32_t, Device>> devices;
    };

    virtual ~OmemoStorage() = default;

    virtual OmemoData allData() const                                      = 0;
    virtual bool      setOwnDevice(const std::optional<OwnDevice> &device) = 0;

    virtual bool addSignedPreKeyPair(uint32_t keyId, const SignedPreKeyPair &keyPair) = 0;
    virtual bool removeSignedPreKeyPair(uint32_t keyId)                               = 0;

    virtual bool addPreKeyPairs(const QHash<uint32_t, QByteArray> &keyPairs) = 0;
    virtual bool removePreKeyPair(uint32_t keyId)                            = 0;

    virtual bool addDevice(const QString &jid, uint32_t deviceId, const Device &device) = 0;
    virtual bool removeDevice(const QString &jid, uint32_t deviceId)                    = 0;
    virtual bool removeDevices(const QString &jid)                                      = 0;
    virtual bool resetAll()                                                             = 0;
};

/** Volatile storage implementation suitable for tests and ephemeral clients. */
class MemoryOmemoStorage final : public OmemoStorage {
public:
    OmemoData allData() const override;
    bool      setOwnDevice(const std::optional<OwnDevice> &device) override;
    bool      addSignedPreKeyPair(uint32_t keyId, const SignedPreKeyPair &keyPair) override;
    bool      removeSignedPreKeyPair(uint32_t keyId) override;
    bool      addPreKeyPairs(const QHash<uint32_t, QByteArray> &keyPairs) override;
    bool      removePreKeyPair(uint32_t keyId) override;
    bool      addDevice(const QString &jid, uint32_t deviceId, const Device &device) override;
    bool      removeDevice(const QString &jid, uint32_t deviceId) override;
    bool      removeDevices(const QString &jid) override;
    bool      resetAll() override;

private:
    OmemoData data_;
};

} // namespace XMPP

#endif // XMPP_OMEMOSTORAGE_H
