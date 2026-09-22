// SPDX-License-Identifier: LGPL-2.1-or-later

#include "jingle-rtp-srtp.h"

#include <iris/dtls.h>

#include <utility>

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

SecureRtpAssociation::SecureRtpAssociation(XMPP::Dtls *dtls, QByteArray associationId, QObject *parent) :
    QObject(parent), dtls_(dtls), associationId_(std::move(associationId))
{
    qRegisterMetaType<PacketKind>();
    if (!dtls_ || associationId_.isEmpty())
        return;

    Q_ASSERT(dtls_->thread() == thread());
    connect(dtls_, &XMPP::Dtls::connected, this, &SecureRtpAssociation::activate);
    connect(dtls_, &XMPP::Dtls::needRestart, this, &SecureRtpAssociation::invalidate);
    connect(dtls_, &XMPP::Dtls::errorOccurred, this, &SecureRtpAssociation::invalidate);
    connect(dtls_, &XMPP::Dtls::closed, this, &SecureRtpAssociation::invalidate);
    connect(dtls_, &QObject::destroyed, this, &SecureRtpAssociation::invalidate);
    activate(); // Supports attaching after the verified handshake completed.
}

SecureRtpAssociation::~SecureRtpAssociation()
{
    close();
}

bool SecureRtpAssociation::isReady() const
{
    return active_ && dtls_ && material_.isValid()
        && dtls_->selectedSRTPProfile() == material_.profile;
}

void SecureRtpAssociation::close()
{
    if (dtls_)
        disconnect(dtls_, nullptr, this, nullptr);
    dtls_.clear();
    invalidate();
}

void SecureRtpAssociation::activate()
{
#if QCA_MAJOR_VERSION >= 3
    if (!dtls_ || active_)
        return;

    const auto material = dtls_->srtpKeyingMaterial();
    if (material.isNull() || material.profile().isEmpty())
        return;

    SecureRtpKeyingMaterial next;
    next.profile          = material.profile();
    next.localMasterKey   = material.localMasterKey();
    next.localMasterSalt  = material.localMasterSalt();
    next.remoteMasterKey  = material.remoteMasterKey();
    next.remoteMasterSalt = material.remoteMasterSalt();
    if (!next.isValid())
        return;

    material_ = std::move(next);
    active_   = true;
    ++epoch_;
    if (!epoch_)
        ++epoch_;
    emit ready(epoch_);
#endif
}

void SecureRtpAssociation::invalidate()
{
    if (!active_ && !material_.isValid())
        return;
    const quint64 invalidatedEpoch = epoch_;
    active_ = false;
    material_.clear();
    ++epoch_;
    if (!epoch_)
        ++epoch_;
    emit invalidated(invalidatedEpoch);
}

bool SecureRtpAssociation::receiveMuxed(QByteArray bytes)
{
    const auto kind = classifyDatagram(bytes);
    if (kind == DatagramKind::Dtls) {
        qInfo("jingle-srtp[%s] DTLS incoming dtls=%p bytes=%d ready=%d", associationId_.toHex().left(12).constData(),
              dtls_.data(), int(bytes.size()), int(isReady()));
        if (dtls_)
            dtls_->writeIncomingDatagram(bytes);
        return true;
    }

    if (!isReady() || (kind != DatagramKind::Rtp && kind != DatagramKind::Rtcp))
        return false;

    // RFC 5761 reserves RTP payload types 64..95 when RTP/RTCP mux is used.
    if (kind == DatagramKind::Rtp) {
        const auto payload = quint8(bytes[1]) & 0x7f;
        if (payload >= 64 && payload <= 95)
            return false;
    }

    emit protectedPacketReceived(bytes, kind == DatagramKind::Rtp ? PacketKind::Rtp : PacketKind::Rtcp, epoch_);
    return true;
}

bool SecureRtpAssociation::validateProtectedMuxed(const QByteArray &bytes, PacketKind packet, quint64 epoch) const
{
    if (!isReady() || epoch != epoch_)
        return false;

    const auto kind = classifyDatagram(bytes);
    if (packet == PacketKind::Rtp) {
        if (kind != DatagramKind::Rtp)
            return false;
        const auto payload = quint8(bytes[1]) & 0x7f;
        return payload < 64 || payload > 95;
    }
    return packet == PacketKind::Rtcp && kind == DatagramKind::Rtcp;
}

} // namespace XMPP::Jingle::RTP
