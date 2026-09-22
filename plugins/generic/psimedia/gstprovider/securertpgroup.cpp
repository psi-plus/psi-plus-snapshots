/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "securertpgroup.h"

#include <QDebug>
#include <QPointer>
#include <QThread>

namespace PsiMedia {

SecureRtpGroup::SecureRtpGroup(QObject *parent) : QObject(parent)
{
    bridge_.setNetworkPacketHandler([this](const PRtpPacket &packet) { protectOutgoing(packet); });
    bridge_.setRuntimeErrorHandler([this]() {
        const auto handler = runtimeErrorHandler_;
        if (!handler)
            return;
        QPointer<SecureRtpGroup> guard(this);
        handler(SecureRtpSessionContext::Error::LibraryFailure);
        if (!guard)
            return;
    });
}

SecureRtpGroup::~SecureRtpGroup()
{
    ownerThread("destruction");
    bridge_.stop();
    bridge_.setNetworkPacketHandler({});
    bridge_.setRuntimeErrorHandler({});
    protectedPacketHandler_ = {};
    runtimeErrorHandler_    = {};
    crypto_.reset();
}

bool SecureRtpGroup::ownerThread(const char *operation) const
{
    if (QThread::currentThread() == thread())
        return true;
    qWarning() << "SecureRtpGroup" << operation << "must run on its owner thread";
    return false;
}

bool SecureRtpGroup::configureEndpoints(const QList<RtpGroupBridge::Endpoint> &endpoints)
{
    return ownerThread("configureEndpoints") && bridge_.configure(endpoints);
}

bool SecureRtpGroup::removeEndpoint(const QByteArray &endpointId)
{
    return ownerThread("removeEndpoint") && bridge_.removeEndpoint(endpointId);
}

void SecureRtpGroup::clearEndpoints()
{
    if (!ownerThread("clearEndpoints"))
        return;
    bridge_.clear();
}

bool SecureRtpGroup::activate(const QByteArray &associationId, quint64 epoch, const QString &profile,
                              const QByteArray &localMasterKey, const QByteArray &localMasterSalt,
                              const QByteArray &remoteMasterKey, const QByteArray &remoteMasterSalt)
{
    if (!ownerThread("activate"))
        return false;
    const bool configured = crypto_.configure(associationId, epoch, profile, localMasterKey, localMasterSalt,
                                              remoteMasterKey, remoteMasterSalt);
    if (configured)
        mediaError_ = SecureRtpSessionContext::Error::None;
    return configured;
}

void SecureRtpGroup::invalidate(const QByteArray &associationId, quint64 epoch)
{
    if (!ownerThread("invalidate"))
        return;
    crypto_.invalidate(associationId, epoch);
    if (!crypto_.isReady())
        mediaError_ = SecureRtpSessionContext::Error::None;
}

void SecureRtpGroup::setProtectedPacketHandler(ProtectedPacketHandler handler)
{
    if (!ownerThread("setProtectedPacketHandler"))
        return;
    protectedPacketHandler_ = std::move(handler);
}

void SecureRtpGroup::setEndpointMediaPacketHandler(const QByteArray &endpointId, MediaPacketHandler handler)
{
    if (!ownerThread("setEndpointMediaPacketHandler"))
        return;
    bridge_.setEndpointMediaPacketHandler(endpointId, std::move(handler));
}

void SecureRtpGroup::setRuntimeErrorHandler(RuntimeErrorHandler handler)
{
    if (!ownerThread("setRuntimeErrorHandler"))
        return;
    runtimeErrorHandler_ = std::move(handler);
}

bool SecureRtpGroup::start() { return ownerThread("start") && crypto_.isReady() && bridge_.start(); }

void SecureRtpGroup::stop()
{
    if (!ownerThread("stop"))
        return;
    // Media pause/stop does not invalidate the DTLS/SRTP association. Replay,
    // ROC and SRTCP index state survive until explicit security invalidation.
    bridge_.stop();
}

GstFlowReturn SecureRtpGroup::sendRtp(const QByteArray &endpointId, GstBuffer *buffer, GstClockTime presentationAge)
{
    if (!ownerThread("sendRtp") || !crypto_.isReady())
        return GST_FLOW_FLUSHING;
    return bridge_.sendRtp(endpointId, buffer, presentationAge);
}

GstFlowReturn SecureRtpGroup::sendRtp(const QByteArray &endpointId, const PRtpPacket &packet)
{
    if (!ownerThread("sendRtp") || !crypto_.isReady())
        return GST_FLOW_FLUSHING;
    return bridge_.sendRtp(endpointId, packet);
}

bool SecureRtpGroup::fatalCryptoError(SecureRtpSessionContext::Error error)
{
    using Error = SecureRtpSessionContext::Error;
    return error == Error::KeyExpired || error == Error::IndexLimit || error == Error::LibraryFailure;
}

void SecureRtpGroup::reportCryptoFailure(SecureRtpSessionContext::Error error)
{
    if (!fatalCryptoError(error))
        return;
    const auto handler = runtimeErrorHandler_;
    if (!handler)
        return;
    QPointer<SecureRtpGroup> guard(this);
    handler(error);
    if (!guard)
        return;
}

void SecureRtpGroup::protectOutgoing(const PRtpPacket &packet)
{
    if (!crypto_.isReady())
        return;

    PSecureRtpPacket plain;
    plain.associationId = crypto_.associationId();
    plain.epoch         = crypto_.epoch();
    plain.rawValue      = packet.rawValue;
    plain.type          = packet.type;

    PSecureRtpPacket protectedPacket;
    if (!crypto_.protect(plain, &protectedPacket)) {
        reportCryptoFailure(crypto_.lastError());
        return;
    }
    mediaError_ = SecureRtpSessionContext::Error::None;

    const auto handler = protectedPacketHandler_;
    if (!handler)
        return;

    // External network glue may synchronously tear down the secure media group.
    QPointer<SecureRtpGroup> guard(this);
    handler(protectedPacket);
    if (!guard)
        return;
}

bool SecureRtpGroup::receiveProtectedPacket(const PSecureRtpPacket &packet)
{
    if (!ownerThread("receiveProtectedPacket") || !crypto_.isReady())
        return false;

    PSecureRtpPacket plain;
    if (!crypto_.unprotect(packet, &plain)) {
        mediaError_ = SecureRtpSessionContext::Error::None;
        reportCryptoFailure(crypto_.lastError());
        return false;
    }

    PRtpPacket mediaPacket;
    mediaPacket.rawValue = std::move(plain.rawValue);
    mediaPacket.type     = plain.type;
    if (bridge_.receivePacket(mediaPacket) != GST_FLOW_OK) {
        mediaError_ = SecureRtpSessionContext::Error::InvalidPacket;
        return false;
    }
    mediaError_ = SecureRtpSessionContext::Error::None;
    return true;
}

} // namespace PsiMedia
