// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-srtp.h"
#include <QSet>
#include <QtEndian>
#include <cstring>
#include <iris/dtls.h>
#include <mutex>
#ifdef IRIS_HAVE_SRTP
#include <srtp2/srtp.h>
#endif

namespace XMPP::Jingle::RTP {
DatagramKind classifyDatagram(const QByteArray &data)
{
    if (data.isEmpty())
        return DatagramKind::Unknown;
    const auto first = quint8(data[0]);
    if (first <= 3)
        return DatagramKind::Stun;
    if (first >= 16 && first <= 19)
        return DatagramKind::Zrtp;
    if (first >= 20 && first <= 63)
        return DatagramKind::Dtls;
    if (first >= 64 && first <= 79)
        return DatagramKind::Turn;
    if (first >= 128 && first <= 191 && data.size() >= 2) {
        const auto second = quint8(data[1]);
        return second >= 192 && second <= 223 ? DatagramKind::Rtcp : DatagramKind::Rtp;
    }
    return DatagramKind::Unknown;
}
class SrtpContext::Private {
public:
    Error            error = Error::NotReady;
    QString          profile;
    QCA::SecureArray local, remote;
    QSet<quint32>    sent, received;
#ifdef IRIS_HAVE_SRTP
    srtp_t sender = nullptr, receiver = nullptr;
#endif
};

#ifdef IRIS_HAVE_SRTP
namespace {
    struct Profile {
        const char    *name;
        srtp_profile_t id;
        int            key, salt;
    };
    const Profile profiles[] = {
        { "SRTP_AES128_CM_HMAC_SHA1_80", srtp_profile_aes128_cm_sha1_80, 16, 14 },
        { "SRTP_AES128_CM_HMAC_SHA1_32", srtp_profile_aes128_cm_sha1_32, 16, 14 },
        { "SRTP_AEAD_AES_128_GCM", srtp_profile_aead_aes_128_gcm, 16, 12 },
        { "SRTP_AEAD_AES_256_GCM", srtp_profile_aead_aes_256_gcm, 32, 12 },
    };
    bool initializeLibrary()
    {
        static std::once_flag once;
        static bool           ready = false;
        std::call_once(once, [] { ready = srtp_init() == srtp_err_status_ok; });
        // Never shut down process-global libSRTP while other users may still need it.
        return ready;
    }
    srtp_t create(const Profile &profile, QCA::SecureArray &key, bool sending)
    {
        srtp_policy_t policy {};
        if (srtp_crypto_policy_set_from_profile_for_rtp(&policy.rtp, profile.id) != srtp_err_status_ok
            || srtp_crypto_policy_set_from_profile_for_rtcp(&policy.rtcp, profile.id) != srtp_err_status_ok)
            return nullptr;
        policy.key             = reinterpret_cast<unsigned char *>(key.data());
        policy.ssrc.type       = sending ? ssrc_any_outbound : ssrc_any_inbound;
        policy.window_size     = 128;
        policy.allow_repeat_tx = 0;
        srtp_t result          = nullptr;
        if (srtp_create(&result, &policy) != srtp_err_status_ok) {
            if (result)
                srtp_dealloc(result);
            return nullptr;
        }
        return result;
    }
    QCA::SecureArray join(const QCA::SecureArray &key, const QCA::SecureArray &salt)
    {
        QCA::SecureArray result(key.size() + salt.size());
        std::memcpy(result.data(), key.constData(), size_t(key.size()));
        std::memcpy(result.data() + key.size(), salt.constData(), size_t(salt.size()));
        return result;
    }
}
#endif

SrtpContext::SrtpContext() : d(std::make_unique<Private>()) { }
SrtpContext::~SrtpContext() { reset(); }
void SrtpContext::reset()
{
#ifdef IRIS_HAVE_SRTP
    if (d->sender)
        srtp_dealloc(d->sender);
    if (d->receiver)
        srtp_dealloc(d->receiver);
    d->sender = d->receiver = nullptr;
#endif
    d->local  = QCA::SecureArray();
    d->remote = QCA::SecureArray();
    d->profile.clear();
    d->sent.clear();
    d->received.clear();
    d->error = Error::NotReady;
}
bool SrtpContext::isReady() const
{
#ifdef IRIS_HAVE_SRTP
    return d->sender && d->receiver;
#else
    return false;
#endif
}
SrtpContext::Error SrtpContext::lastError() const { return d->error; }
QStringList        SrtpContext::supportedProfiles()
{
    QStringList result;
#ifdef IRIS_HAVE_SRTP
    if (!initializeLibrary())
        return result;
    for (const auto &profile : profiles) {
        QCA::SecureArray probe(profile.key + profile.salt);
        std::memset(probe.data(), 0, size_t(probe.size()));
        // Probe the actual crypto backend, not just libSRTP policy constants.
        auto context = create(profile, probe, true);
        if (context) {
            result.append(QLatin1String(profile.name));
            srtp_dealloc(context);
        }
    }
#endif
    return result;
}

QStringList supportedSecureRtpProfiles()
{
    if (!Dtls::isSupported())
        return {};
    auto       result       = SrtpContext::supportedProfiles();
    const auto dtlsProfiles = Dtls::supportedSRTPProfiles();
    for (auto it = result.begin(); it != result.end();) {
        if (!dtlsProfiles.contains(*it))
            it = result.erase(it);
        else
            ++it;
    }
    return result;
}
bool SrtpContext::configure(const QString &name, const QCA::SecureArray &localKey, const QCA::SecureArray &localSalt,
                            const QCA::SecureArray &remoteKey, const QCA::SecureArray &remoteSalt)
{
#ifdef IRIS_HAVE_SRTP
    const Profile *profile = nullptr;
    for (const auto &candidate : profiles)
        if (name == QLatin1String(candidate.name))
            profile = &candidate;
    if (!profile) {
        reset();
        d->error = Error::UnsupportedProfile;
        return false;
    }
    if (localKey.size() != profile->key || remoteKey.size() != profile->key || localSalt.size() != profile->salt
        || remoteSalt.size() != profile->salt) {
        reset();
        d->error = Error::InvalidKey;
        return false;
    }
    auto local = join(localKey, localSalt), remote = join(remoteKey, remoteSalt);
    if (isReady() && d->profile == name && d->local == local && d->remote == remote) {
        d->error = Error::None;
        return true;
    }
    reset();
    if (!initializeLibrary()) {
        d->error = Error::LibraryFailure;
        return false;
    }
    d->sender   = create(*profile, local, true);
    d->receiver = create(*profile, remote, false);
    if (!isReady()) {
        reset();
        d->error = Error::LibraryFailure;
        return false;
    }
    d->profile = name;
    d->local   = std::move(local);
    d->remote  = std::move(remote);
    d->error   = Error::None;
    return true;
#else
    Q_UNUSED(name);
    Q_UNUSED(localKey);
    Q_UNUSED(localSalt);
    Q_UNUSED(remoteKey);
    Q_UNUSED(remoteSalt);
    reset();
    d->error = Error::UnsupportedProfile;
    return false;
#endif
}
std::optional<QByteArray> SrtpContext::process(QByteArray bytes, Packet packet, bool sending)
{
    if (!isReady()) {
        d->error = Error::NotReady;
        return {};
    }
#ifdef IRIS_HAVE_SRTP
    const int     minimum = packet == Packet::Rtp ? 12 : 8;
    constexpr int trailer = SRTP_MAX_TRAILER_LEN + 4;
    if ((packet != Packet::Rtp && packet != Packet::Rtcp) || bytes.size() < minimum
        || bytes.size() > 65535 - (sending ? trailer : 0) || (quint8(bytes[0]) >> 6) != 2) {
        d->error = Error::InvalidPacket;
        return {};
    }
    const quint32 ssrc
        = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData()) + (packet == Packet::Rtp ? 8 : 4));
    auto &streams = sending ? d->sent : d->received;
    // Bound auto-created libSRTP stream state, including authenticated peers.
    if (!streams.contains(ssrc) && streams.size() >= 64) {
        d->error = Error::StreamLimit;
        return {};
    }
    int length = int(bytes.size());
    if (sending)
        bytes.resize(length + trailer);
    auto context = sending ? d->sender : d->receiver;
    auto status  = sending ? (packet == Packet::Rtp ? srtp_protect(context, bytes.data(), &length)
                                                    : srtp_protect_rtcp(context, bytes.data(), &length))
                           : (packet == Packet::Rtp ? srtp_unprotect(context, bytes.data(), &length)
                                                    : srtp_unprotect_rtcp(context, bytes.data(), &length));
    if (status != srtp_err_status_ok) {
        // Do not retain wildcard stream allocations for unauthenticated or
        // malformed first packets (libSRTP versions differ in allocation timing).
        if (!streams.contains(ssrc))
            srtp_remove_stream(context, qToBigEndian(ssrc));
        if (status == srtp_err_status_auth_fail)
            d->error = Error::Authentication;
        else if (status == srtp_err_status_replay_fail || status == srtp_err_status_replay_old)
            d->error = Error::Replay;
        else
            d->error = Error::LibraryFailure;
        return {};
    }
    streams.insert(ssrc);
    bytes.resize(length);
    d->error = Error::None;
    return bytes;
#else
    Q_UNUSED(bytes);
    Q_UNUSED(packet);
    Q_UNUSED(sending);
    return {};
#endif
}
std::optional<QByteArray> SrtpContext::protect(QByteArray bytes, Packet packet)
{
    return process(std::move(bytes), packet, true);
}
std::optional<QByteArray> SrtpContext::unprotect(QByteArray bytes, Packet packet)
{
    return process(std::move(bytes), packet, false);
}

SrtpSession::SrtpSession(XMPP::Dtls *dtls, QObject *parent) : QObject(parent), dtls_(dtls)
{
    qRegisterMetaType<SrtpContext::Packet>();
    if (!dtls)
        return;
    Q_ASSERT(dtls->thread() == thread());
    connect(dtls, &XMPP::Dtls::connected, this, &SrtpSession::activate);
    connect(dtls, &XMPP::Dtls::needRestart, this, &SrtpSession::invalidate);
    connect(dtls, &XMPP::Dtls::errorOccurred, this, &SrtpSession::invalidate);
    connect(dtls, &XMPP::Dtls::closed, this, &SrtpSession::invalidate);
    connect(dtls, &QObject::destroyed, this, &SrtpSession::invalidate);
    activate(); // Also support attaching after fingerprint verification.
}
bool SrtpSession::isReady() const
{
    // Check the DTLS gate too: an earlier invalidation signal subscriber can
    // reenter us before our own invalidation slot has run.
    return dtls_ && context_.isReady() && !dtls_->selectedSRTPProfile().isEmpty();
}
void SrtpSession::close()
{
    if (!dtls_ && !context_.isReady())
        return;
    if (dtls_)
        disconnect(dtls_, nullptr, this, nullptr);
    dtls_.clear();
    invalidate();
}
void SrtpSession::activate()
{
#if QCA_MAJOR_VERSION >= 3
    if (!dtls_ || context_.isReady())
        return;
    const auto material = dtls_->srtpKeyingMaterial();
    if (material.isNull())
        return;
    if (!context_.configure(material.profile(), material.localMasterKey(), material.localMasterSalt(),
                            material.remoteMasterKey(), material.remoteMasterSalt()))
        return;
    ++epoch_;
    emit ready();
#endif
}
void SrtpSession::invalidate()
{
    context_.reset();
    ++epoch_;
    emit invalidated();
}
std::optional<QByteArray> SrtpSession::protect(QByteArray bytes, SrtpContext::Packet packet, quint64 epoch)
{
    if (epoch != epoch_ || !isReady())
        return {};
    return context_.protect(std::move(bytes), packet);
}
std::optional<QByteArray> SrtpSession::unprotect(QByteArray bytes, SrtpContext::Packet packet, quint64 epoch)
{
    if (epoch != epoch_ || !isReady())
        return {};
    return context_.unprotect(std::move(bytes), packet);
}
std::optional<SrtpSession::ReceivedPacket> SrtpSession::receiveMuxed(QByteArray bytes)
{
    const auto kind = classifyDatagram(bytes);
    if (kind == DatagramKind::Dtls) {
        if (dtls_)
            dtls_->writeIncomingDatagram(bytes);
        return {}; // DTLS callbacks may have destroyed this binding.
    }
    if (kind != DatagramKind::Rtp && kind != DatagramKind::Rtcp)
        return {};
    if (kind == DatagramKind::Rtp) {
        const auto payload = quint8(bytes[1]) & 0x7f;
        if (payload >= 64 && payload <= 95)
            return {};
    }
    const auto packet = kind == DatagramKind::Rtp ? SrtpContext::Packet::Rtp : SrtpContext::Packet::Rtcp;
    auto       plain  = unprotect(std::move(bytes), packet, epoch_);
    if (!plain)
        return {};
    return ReceivedPacket { std::move(*plain), packet, epoch_ };
}
std::optional<QByteArray> SrtpSession::protectMuxed(QByteArray bytes, SrtpContext::Packet packet, quint64 epoch)
{
    const auto kind = classifyDatagram(bytes);
    if (packet == SrtpContext::Packet::Rtp) {
        if (kind != DatagramKind::Rtp)
            return {};
        const auto payload = quint8(bytes[1]) & 0x7f;
        if (payload >= 64 && payload <= 95)
            return {}; // RFC 5761: conflicts with RTCP when marker is set.
    } else if (packet != SrtpContext::Packet::Rtcp || kind != DatagramKind::Rtcp) {
        return {};
    }
    return protect(std::move(bytes), packet, epoch);
}
void SrtpSession::dispatchMuxed(QByteArray bytes)
{
    auto packet = receiveMuxed(std::move(bytes));
    if (packet)
        emit packetReceived(packet->data, packet->kind, packet->epoch);
}
}
