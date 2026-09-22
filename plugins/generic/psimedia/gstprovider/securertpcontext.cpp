/*
 * Copyright (C) 2026 Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "securertpcontext.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef PSIMEDIA_HAVE_SRTP
#include <srtp2/srtp.h>
#endif

namespace PsiMedia {
namespace {

    using Error = SecureRtpSessionContext::Error;

    class SecureBuffer {
    public:
        SecureBuffer() = default;
        ~SecureBuffer() { clear(); }

        SecureBuffer(const SecureBuffer &)            = delete;
        SecureBuffer &operator=(const SecureBuffer &) = delete;

        SecureBuffer(SecureBuffer &&other) noexcept : bytes_(std::move(other.bytes_)) { other.bytes_.clear(); }
        SecureBuffer &operator=(SecureBuffer &&other) noexcept
        {
            if (this != &other) {
                clear();
                bytes_ = std::move(other.bytes_);
                other.bytes_.clear();
            }
            return *this;
        }

        void assign(const QByteArray &key, const QByteArray &salt)
        {
            clear();
            bytes_.resize(size_t(key.size() + salt.size()));
            if (!key.isEmpty())
                std::memcpy(bytes_.data(), key.constData(), size_t(key.size()));
            if (!salt.isEmpty())
                std::memcpy(bytes_.data() + key.size(), salt.constData(), size_t(salt.size()));
        }

        bool equals(const QByteArray &key, const QByteArray &salt) const
        {
            if (bytes_.size() != size_t(key.size() + salt.size()))
                return false;
            if (!key.isEmpty() && std::memcmp(bytes_.data(), key.constData(), size_t(key.size())) != 0)
                return false;
            return salt.isEmpty()
                || std::memcmp(bytes_.data() + key.size(), salt.constData(), size_t(salt.size())) == 0;
        }

        unsigned char *data() { return bytes_.empty() ? nullptr : bytes_.data(); }
        bool           empty() const { return bytes_.empty(); }

        void clear()
        {
            volatile unsigned char *p = bytes_.empty() ? nullptr : bytes_.data();
            for (size_t i = 0; i < bytes_.size(); ++i)
                p[i] = 0;
            bytes_.clear();
            bytes_.shrink_to_fit();
        }

    private:
        std::vector<unsigned char> bytes_;
    };

#ifdef PSIMEDIA_HAVE_SRTP
    struct Profile {
        const char    *name;
        srtp_profile_t id;
        int            keySize;
        int            saltSize;
    };

    const Profile Profiles[] = {
        { "SRTP_AES128_CM_HMAC_SHA1_80", srtp_profile_aes128_cm_sha1_80, 16, 14 },
        { "SRTP_AES128_CM_HMAC_SHA1_32", srtp_profile_aes128_cm_sha1_32, 16, 14 },
        { "SRTP_AEAD_AES_128_GCM", srtp_profile_aead_aes_128_gcm, 16, 12 },
        { "SRTP_AEAD_AES_256_GCM", srtp_profile_aead_aes_256_gcm, 32, 12 },
    };

    const Profile *findProfile(const QString &name)
    {
        for (const auto &profile : Profiles) {
            if (name == QLatin1String(profile.name))
                return &profile;
        }
        return nullptr;
    }

    bool initializeLibrary()
    {
        static std::once_flag once;
        static bool           ready = false;
        std::call_once(once, [] { ready = srtp_init() == srtp_err_status_ok; });
        // libSRTP initialization is process-global. Do not call srtp_shutdown()
        // from a plugin: another loaded provider/library may share the same DSO.
        return ready;
    }

    srtp_t createContext(const Profile &profile, SecureBuffer &material, bool sending)
    {
        srtp_policy_t policy {};
        if (srtp_crypto_policy_set_from_profile_for_rtp(&policy.rtp, profile.id) != srtp_err_status_ok
            || srtp_crypto_policy_set_from_profile_for_rtcp(&policy.rtcp, profile.id) != srtp_err_status_ok)
            return nullptr;

        policy.key             = material.data();
        policy.ssrc.type       = sending ? ssrc_any_outbound : ssrc_any_inbound;
        policy.window_size     = 128;
        policy.allow_repeat_tx = 0;

        srtp_t context = nullptr;
        if (srtp_create(&context, &policy) != srtp_err_status_ok) {
            if (context)
                srtp_dealloc(context);
            return nullptr;
        }
        return context;
    }

    Error mapStatus(srtp_err_status_t status)
    {
        switch (status) {
        case srtp_err_status_auth_fail:
        case srtp_err_status_nonce_bad:
            return Error::Authentication;
        case srtp_err_status_replay_fail:
        case srtp_err_status_replay_old:
        case srtp_err_status_pkt_idx_old:
            return Error::Replay;
        case srtp_err_status_key_expired:
            return Error::KeyExpired;
        case srtp_err_status_pkt_idx_adv:
            return Error::IndexLimit;
        case srtp_err_status_bad_param:
        case srtp_err_status_bad_mki:
        case srtp_err_status_parse_err:
            return Error::InvalidPacket;
        default:
            return Error::LibraryFailure;
        }
    }
#endif

} // namespace

class SrtpAssociation::Private {
public:
    mutable QMutex mutex;
    Error          error = Error::NotReady;
    QByteArray     associationId;
    quint64        epoch = 0;
    QString        profile;
    SecureBuffer   local;
    SecureBuffer   remote;
    QSet<quint32>  sent;
    QSet<quint32>  received;
#ifdef PSIMEDIA_HAVE_SRTP
    srtp_t sender   = nullptr;
    srtp_t receiver = nullptr;
#endif

    void resetUnlocked()
    {
#ifdef PSIMEDIA_HAVE_SRTP
        if (sender)
            srtp_dealloc(sender);
        if (receiver)
            srtp_dealloc(receiver);
        sender = receiver = nullptr;
#endif
        local.clear();
        remote.clear();
        associationId.clear();
        epoch = 0;
        profile.clear();
        sent.clear();
        received.clear();
        error = Error::NotReady;
    }

    bool readyUnlocked() const
    {
#ifdef PSIMEDIA_HAVE_SRTP
        return sender && receiver;
#else
        return false;
#endif
    }
};

SrtpAssociation::SrtpAssociation() : d(std::make_unique<Private>()) { }

SrtpAssociation::~SrtpAssociation() { reset(); }

QStringList SrtpAssociation::supportedProfiles()
{
    QStringList result;
#ifdef PSIMEDIA_HAVE_SRTP
    if (!initializeLibrary())
        return result;

    for (const auto &profile : Profiles) {
        QByteArray   zeroKey(profile.keySize, char(0));
        QByteArray   zeroSalt(profile.saltSize, char(0));
        SecureBuffer material;
        material.assign(zeroKey, zeroSalt);
        auto context = createContext(profile, material, true);
        if (context) {
            result.append(QLatin1String(profile.name));
            srtp_dealloc(context);
        }
    }
#endif
    return result;
}

bool SrtpAssociation::configure(const QByteArray &associationId, quint64 epoch, const QString &profileName,
                                const QByteArray &localMasterKey, const QByteArray &localMasterSalt,
                                const QByteArray &remoteMasterKey, const QByteArray &remoteMasterSalt)
{
    QMutexLocker locker(&d->mutex);
#ifdef PSIMEDIA_HAVE_SRTP
    if (!initializeLibrary()) {
        d->resetUnlocked();
        d->error = Error::LibraryFailure;
        return false;
    }

    const auto *profile = findProfile(profileName);
    if (!profile) {
        d->resetUnlocked();
        d->error = Error::UnsupportedProfile;
        return false;
    }

    if (associationId.isEmpty() || epoch == 0 || localMasterKey.size() != profile->keySize
        || remoteMasterKey.size() != profile->keySize || localMasterSalt.size() != profile->saltSize
        || remoteMasterSalt.size() != profile->saltSize) {
        d->resetUnlocked();
        d->error = Error::InvalidKey;
        return false;
    }

    const bool sameAssociation = d->readyUnlocked() && d->associationId == associationId;
    if (sameAssociation && epoch < d->epoch) {
        d->error = Error::StaleEpoch;
        return false;
    }

    const bool sameMaterial = sameAssociation && d->profile == profileName
        && d->local.equals(localMasterKey, localMasterSalt) && d->remote.equals(remoteMasterKey, remoteMasterSalt);
    if (sameMaterial) {
        d->epoch = epoch;
        d->error = Error::None;
        return true;
    }

    // A key/profile change under the same opaque association must advance the
    // security epoch. Otherwise queued packets carrying the same metadata could
    // be interpreted under two different key sets.
    if (sameAssociation && epoch == d->epoch) {
        d->error = Error::StaleEpoch;
        return false;
    }

    SecureBuffer local;
    SecureBuffer remote;
    local.assign(localMasterKey, localMasterSalt);
    remote.assign(remoteMasterKey, remoteMasterSalt);

    d->resetUnlocked();
    d->sender   = createContext(*profile, local, true);
    d->receiver = createContext(*profile, remote, false);
    if (!d->readyUnlocked()) {
        d->resetUnlocked();
        d->error = Error::LibraryFailure;
        return false;
    }

    d->associationId = associationId;
    d->epoch         = epoch;
    d->profile       = profileName;
    d->local         = std::move(local);
    d->remote        = std::move(remote);
    d->error         = Error::None;
    return true;
#else
    Q_UNUSED(associationId)
    Q_UNUSED(epoch)
    Q_UNUSED(profileName)
    Q_UNUSED(localMasterKey)
    Q_UNUSED(localMasterSalt)
    Q_UNUSED(remoteMasterKey)
    Q_UNUSED(remoteMasterSalt)
    d->resetUnlocked();
    d->error = Error::UnsupportedProfile;
    return false;
#endif
}

void SrtpAssociation::invalidate(const QByteArray &associationId, quint64 epoch)
{
    QMutexLocker locker(&d->mutex);
    if (!d->readyUnlocked() || d->associationId != associationId || d->epoch != epoch)
        return;
    d->resetUnlocked();
}

void SrtpAssociation::reset()
{
    QMutexLocker locker(&d->mutex);
    d->resetUnlocked();
}

bool SrtpAssociation::isReady() const
{
    QMutexLocker locker(&d->mutex);
    return d->readyUnlocked();
}

QByteArray SrtpAssociation::associationId() const
{
    QMutexLocker locker(&d->mutex);
    return d->associationId;
}

quint64 SrtpAssociation::epoch() const
{
    QMutexLocker locker(&d->mutex);
    return d->epoch;
}

SecureRtpSessionContext::Error SrtpAssociation::lastError() const
{
    QMutexLocker locker(&d->mutex);
    return d->error;
}

bool SrtpAssociation::process(const PSecureRtpPacket &input, PSecureRtpPacket *output, bool sending)
{
    QMutexLocker locker(&d->mutex);
    if (!output) {
        d->error = Error::InvalidPacket;
        return false;
    }
    if (!d->readyUnlocked()) {
        d->error = Error::NotReady;
        return false;
    }
    if (input.associationId != d->associationId || input.epoch != d->epoch) {
        d->error = Error::StaleEpoch;
        return false;
    }

#ifdef PSIMEDIA_HAVE_SRTP
    const bool    rtp     = input.type == PRtpPacket::Type::Rtp;
    const bool    rtcp    = input.type == PRtpPacket::Type::Rtcp;
    const int     minimum = rtp ? 12 : 8;
    constexpr int Trailer = SRTP_MAX_TRAILER_LEN + 4;

    QByteArray bytes = input.rawValue;
    if ((!rtp && !rtcp) || bytes.size() < minimum || bytes.size() > 65535 - (sending ? Trailer : 0)
        || (quint8(bytes[0]) >> 6) != 2) {
        d->error = Error::InvalidPacket;
        return false;
    }

    const quint32 ssrc    = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()) + (rtp ? 8 : 4));
    auto         &streams = sending ? d->sent : d->received;
    if (!streams.contains(ssrc) && streams.size() >= 64) {
        d->error = Error::StreamLimit;
        return false;
    }

    int length = bytes.size();
    if (sending)
        bytes.resize(length + Trailer);

    auto       context = sending ? d->sender : d->receiver;
    const auto status  = sending
         ? (rtp ? srtp_protect(context, bytes.data(), &length) : srtp_protect_rtcp(context, bytes.data(), &length))
         : (rtp ? srtp_unprotect(context, bytes.data(), &length) : srtp_unprotect_rtcp(context, bytes.data(), &length));

    if (status != srtp_err_status_ok) {
        if (!streams.contains(ssrc))
            srtp_remove_stream(context, qToBigEndian(ssrc));
        d->error = mapStatus(status);
        return false;
    }

    streams.insert(ssrc);
    bytes.resize(length);
    output->associationId = d->associationId;
    output->epoch         = d->epoch;
    output->rawValue      = std::move(bytes);
    output->type          = input.type;
    d->error              = Error::None;
    return true;
#else
    Q_UNUSED(input)
    Q_UNUSED(output)
    Q_UNUSED(sending)
    d->error = Error::UnsupportedProfile;
    return false;
#endif
}

bool SrtpAssociation::protect(const PSecureRtpPacket &plain, PSecureRtpPacket *protectedPacket)
{
    return process(plain, protectedPacket, true);
}

bool SrtpAssociation::unprotect(const PSecureRtpPacket &protectedPacket, PSecureRtpPacket *plain)
{
    return process(protectedPacket, plain, false);
}

} // namespace PsiMedia
