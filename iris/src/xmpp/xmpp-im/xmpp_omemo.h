/*
 * xmpp_omemo.h - XEP-0384 OMEMO encryption method
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef XMPP_OMEMO_H
#define XMPP_OMEMO_H

#include "xmpp_encryption.h"
#include "xmpp_omemostorage.h"

#include <QList>

#include <memory>
#include <optional>

namespace XMPP {

class Client;

struct OmemoDeviceInfo {
    Jid                  owner;
    uint32_t             id = 0;
    QString              label;
    QByteArray           identityKey;
    EncryptionTrustLevel trust = EncryptionTrustLevel::Undecided;
    OmemoProtocols       protocols;
    bool                 active     = false;
    bool                 hasSession = false;
};

/**
 * XEP-0384 OMEMO implementation for Iris.
 *
 * The method owns one Signal/Double-Ratchet engine and exposes multiple wire
 * profiles. OMEMO 2 (urn:xmpp:omemo:2) is the preferred profile; the legacy
 * eu.siacs.conversations.axolotl profile is retained for interoperability with
 * older Psi and other XEP-0384 0.3.x clients. Protocol selection for a concrete
 * online resource uses Iris' existing XEP-0030/entity-caps cache and therefore
 * operates on a full JID.
 *
 * OMEMO is an optional EncryptionMethod because its Double Ratchet backend,
 * libomemo-c, is GPLv3. Iris' generic encryption, SCE and PubSub APIs do not
 * depend on it. All cryptographic primitives requested by libomemo-c and all
 * OMEMO payload cryptography are provided exclusively through QCA.
 */
class OmemoEncryption final : public EncryptionMethod {
    Q_OBJECT
public:
    explicit OmemoEncryption(Client *client, OmemoStorage *storage = nullptr,
                             EncryptionTrustStorage *trustStorage = nullptr, QObject *parent = nullptr);
    ~OmemoEncryption() override;

    static QString methodId();

    static QString namespaceUri();
    static QString devicesNode();
    static QString bundlesNode();

    static QString legacyNamespaceUri();
    static QString legacyDevicesNode();
    static QString legacyBundleNode(uint32_t deviceId);

    QString           id() const override;
    QString           name() const override;
    Capabilities      capabilities() const override;
    EncryptedSession *startSession(Capabilities caps, const EncryptionContext &context) override;
    Features          features() const override;
    bool              canDecrypt(const QDomElement &stanza) const override;

    /** Wire profiles usable with the currently available QCA provider. */
    OmemoProtocols supportedProtocols() const;
    /** Protocols advertised by a concrete online resource's cached disco info and usable locally. */
    OmemoProtocols protocolsFor(const Jid &fullJid) const;
    /** Prefer OMEMO 2 when a resource advertises both profiles. */
    std::optional<OmemoProtocol> preferredProtocolFor(const Jid &fullJid) const;

    bool       isReady() const;
    uint32_t   ownDeviceId() const;
    QByteArray ownIdentityKey() const;
    QString    ownDeviceLabel() const;

    QList<OmemoDeviceInfo> devices(const Jid &owner = {}) const;

    EncryptionTrustLevels acceptedSessionBuildingTrustLevels() const;
    void                  setAcceptedSessionBuildingTrustLevels(EncryptionTrustLevels levels);
    EncryptionTrustLevel  newIdentityTrustLevel() const;
    void                  setNewIdentityTrustLevel(EncryptionTrustLevel level);
    EncryptionTrustLevel  trustLevel(const Jid &owner, const QByteArray &identityKey) const;
    bool                  setTrustLevel(const Jid &owner, const QByteArray &identityKey, EncryptionTrustLevel level);

    int  minimumEnvelopeSize() const;
    void setMinimumEnvelopeSize(int bytes);

    /** Generate local identity/prekeys if needed and publish both wire profiles. */
    EncryptionJob *setUp(const QString &deviceLabel = {});

    /** Refresh a PEP device list for the selected wire profile. */
    EncryptionJob *refreshDevices(const Jid &owner, OmemoProtocol protocol, bool fetchBundles = false);
    /** OMEMO 2 compatibility overload. */
    EncryptionJob *refreshDevices(const Jid &owner, bool fetchBundles = false);

    /** Fetch one bundle and optionally build/update its protocol-specific session. */
    EncryptionJob *refreshBundle(const Jid &owner, uint32_t deviceId, OmemoProtocol protocol, bool buildSession = true);
    /** OMEMO 2 compatibility overload. */
    EncryptionJob *refreshBundle(const Jid &owner, uint32_t deviceId, bool buildSession = true);

    EncryptionJob *publishOwnBundle(OmemoProtocol protocol);
    EncryptionJob *publishOwnDevice(OmemoProtocol protocol);
    /** Publish both OMEMO 2 and legacy representations. */
    EncryptionJob *publishOwnBundle();
    EncryptionJob *publishOwnDevice();

    /**
     * Remove a non-current device from this account's published OMEMO device
     * lists. Its bundle is intentionally left on the server: without a
     * device-list entry it is no longer selected as an encryption recipient.
     */
    EncryptionJob *retireOwnDevice(uint32_t deviceId);

    /**
     * Send an empty OMEMO 2 protocol message to one existing Double Ratchet
     * session. Empty messages are an OMEMO 2 protocol-management primitive and
     * are not generated for the legacy profile.
     */
    EncryptionJob *sendEmptyMessage(const Jid &recipient, uint32_t deviceId);

    /** Clear all local OMEMO state. Server-side PEP data is intentionally untouched. */
    bool resetAllLocally();

signals:
    void readyChanged(bool ready);
    void deviceChanged(const XMPP::Jid &owner, uint32_t deviceId);
    void deviceRemoved(const XMPP::Jid &owner, uint32_t deviceId);
    void trustChanged(const XMPP::Jid &owner, const QByteArray &identityKey, XMPP::EncryptionTrustLevel level);
    void warning(const QString &message);

private:
    friend class OmemoEncryptedSession;
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

Q_DECLARE_METATYPE(XMPP::OmemoDeviceInfo)
Q_DECLARE_METATYPE(XMPP::OmemoProtocol)
Q_DECLARE_METATYPE(XMPP::OmemoProtocols)

#endif // XMPP_OMEMO_H
