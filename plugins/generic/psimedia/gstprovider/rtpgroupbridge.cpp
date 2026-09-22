/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rtpgroupbridge.h"

#include <QDebug>
#include <QPointer>
#include <QThread>

#include <limits>
#include <optional>

namespace PsiMedia {

namespace {
    quint16 readNetwork16(const QByteArray &data, int offset)
    {
        const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
        return quint16((quint16(p[0]) << 8) | quint16(p[1]));
    }

    void appendNetwork16(QByteArray &data, quint16 value)
    {
        data.append(char(value >> 8));
        data.append(char(value));
    }

    std::optional<QByteArray> stampMidExtension(const QByteArray &packet, quint16 extensionId, const QByteArray &mid)
    {
        if (packet.size() < 12 || !extensionId || extensionId > 255 || mid.isEmpty() || mid.size() > 255)
            return {};

        const auto *bytes = reinterpret_cast<const uchar *>(packet.constData());
        if ((bytes[0] >> 6) != 2)
            return {};

        const int csrcBytes = int(bytes[0] & 0x0f) * 4;
        if (csrcBytes > packet.size() - 12)
            return {};
        const int baseHeader = 12 + csrcBytes;

        auto appendElement = [&](QByteArray &extensions, bool oneByte) {
            if (oneByte) {
                if (extensionId > 14 || mid.size() > 16)
                    return false;
                extensions.append(char((extensionId << 4) | quint16(mid.size() - 1)));
                extensions.append(mid);
            } else {
                extensions.append(char(extensionId));
                extensions.append(char(mid.size()));
                extensions.append(mid);
            }
            return true;
        };

        auto containsElement = [&](const QByteArray &extensions, bool oneByte) -> std::optional<bool> {
            int cursor = 0;
            while (cursor < extensions.size()) {
                const quint8 id = quint8(extensions.at(cursor++));
                if (!id)
                    continue;

                if (oneByte) {
                    const quint8 elementId = id >> 4;
                    if (elementId == 15)
                        return {};
                    const int length = (id & 0x0f) + 1;
                    if (length > extensions.size() - cursor)
                        return {};
                    if (elementId == extensionId)
                        return extensions.mid(cursor, length) == mid;
                    cursor += length;
                } else {
                    const quint8 elementId = id;
                    if (cursor >= extensions.size())
                        return {};
                    const int length = quint8(extensions.at(cursor++));
                    if (length > extensions.size() - cursor)
                        return {};
                    if (elementId == extensionId)
                        return extensions.mid(cursor, length) == mid;
                    cursor += length;
                }
            }
            return false;
        };

        QByteArray extensions;
        quint16    profile       = 0;
        int        payloadOffset = baseHeader;

        if (bytes[0] & 0x10) {
            if (packet.size() - baseHeader < 4)
                return {};
            profile                  = readNetwork16(packet, baseHeader);
            const int extensionBytes = int(readNetwork16(packet, baseHeader + 2)) * 4;
            if (extensionBytes > packet.size() - baseHeader - 4)
                return {};
            extensions    = packet.mid(baseHeader + 4, extensionBytes);
            payloadOffset = baseHeader + 4 + extensionBytes;

            const bool oneByte = profile == 0xbede;
            const bool twoByte = (profile & 0xfff0) == 0x1000;
            if (!oneByte && !twoByte)
                return {};
            if ((oneByte && (extensionId > 14 || mid.size() > 16)) || (!oneByte && extensionId > 255))
                return {};

            const auto existing = containsElement(extensions, oneByte);
            if (!existing)
                return {};
            if (*existing)
                return packet;
            if (!appendElement(extensions, oneByte))
                return {};
        } else {
            const bool oneByte = extensionId <= 14 && mid.size() <= 16;
            profile            = oneByte ? quint16(0xbede) : quint16(0x1000);
            if (!appendElement(extensions, oneByte))
                return {};
        }

        while (extensions.size() & 3)
            extensions.append(char(0));
        if (extensions.size() / 4 > std::numeric_limits<quint16>::max())
            return {};

        QByteArray result = packet.left(baseHeader);
        result[0]         = char(quint8(result.at(0)) | 0x10);
        appendNetwork16(result, profile);
        appendNetwork16(result, quint16(extensions.size() / 4));
        result.append(extensions);
        result.append(packet.mid(payloadOffset));
        return result;
    }

    GstBuffer *bufferWithBytes(GstBuffer *source, const QByteArray &bytes)
    {
        if (!source || bytes.isEmpty())
            return nullptr;
        GstBuffer *result = gst_buffer_new_allocate(nullptr, gsize(bytes.size()), nullptr);
        if (!result)
            return nullptr;
        if (gst_buffer_fill(result, 0, bytes.constData(), gsize(bytes.size())) != gsize(bytes.size())) {
            gst_buffer_unref(result);
            return nullptr;
        }
        const auto flags = static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_TIMESTAMPS);
        gst_buffer_copy_into(result, source, flags, 0, -1);
        return result;
    }
} // namespace

RtpGroupBridge::RtpGroupBridge(QObject *parent) : QObject(parent), session_(QStringLiteral("group"))
{
    session_.setNetworkPacketHandler([this](const PRtpPacket &packet) {
        const auto handler = networkPacketHandler_;
        if (!handler)
            return;
        QPointer<RtpGroupBridge> guard(this);
        handler(packet);
        if (!guard)
            return;
    });
    session_.setMediaPacketHandler([this](GstBuffer *buffer) { deliverIncomingRtp(buffer); });
    session_.setRuntimeErrorHandler([this]() { emitRuntimeError(); });
}

RtpGroupBridge::~RtpGroupBridge()
{
    ownerThread("destruction");
    session_.stop();
    session_.setNetworkPacketHandler({});
    session_.setMediaPacketHandler({});
    session_.setRuntimeErrorHandler({});
}

bool RtpGroupBridge::ownerThread(const char *operation) const
{
    if (QThread::currentThread() == thread())
        return true;
    qWarning() << "RtpGroupBridge" << operation << "must run on its owner thread";
    return false;
}

bool RtpGroupBridge::configure(const QList<Endpoint> &endpoints)
{
    if (!ownerThread("configure") || endpoints.isEmpty())
        return false;

    QList<RtpSessionBridge::PayloadGroup> payloadGroups;
    QList<RtpBundleRouter::Route>         routes;
    QHash<QByteArray, EndpointState>      nextEndpoints;

    for (const auto &endpoint : endpoints) {
        if (endpoint.id.isEmpty() || endpoint.media.isEmpty() || endpoint.route.endpointId != endpoint.id
            || nextEndpoints.contains(endpoint.id))
            return false;

        RtpSessionBridge::PayloadGroup payloadGroup;
        payloadGroup.endpointId = endpoint.id;
        payloadGroup.media      = endpoint.media;
        payloadGroup.local      = endpoint.localPayloads;
        payloadGroup.remote     = endpoint.remotePayloads;
        payloadGroups.append(std::move(payloadGroup));
        routes.append(endpoint.route);

        EndpointState state;
        state.config = endpoint;
        for (const auto &payload : endpoint.localPayloads) {
            if (payload.id < 0 || payload.id > 127)
                return false;
            state.outgoingPayloadTypes.insert(quint8(payload.id));
        }
        nextEndpoints.insert(endpoint.id, std::move(state));
    }

    // Copying retains runtime outgoing-SSRC ownership for surviving endpoint IDs.
    // A rejected candidate cannot mutate the live route table.
    auto candidateRouter = router_;
    if (!candidateRouter.configure(routes))
        return false;
    if (!session_.setPayloadGroups(payloadGroups))
        return false;

    router_    = std::move(candidateRouter);
    endpoints_ = std::move(nextEndpoints);

    // Route removal itself prevents delivery, but discard stale user callbacks
    // too so an endpoint ID can later be reused only with an explicit handler.
    for (auto it = mediaHandlers_.begin(); it != mediaHandlers_.end();) {
        if (!endpoints_.contains(it.key()))
            it = mediaHandlers_.erase(it);
        else
            ++it;
    }
    return true;
}

bool RtpGroupBridge::removeEndpoint(const QByteArray &endpointId)
{
    if (!ownerThread("removeEndpoint") || !endpoints_.contains(endpointId))
        return false;
    if (endpoints_.size() == 1)
        return false; // group lifetime is owned above us; stop()/teardown handles the final member.

    QList<Endpoint> remaining;
    remaining.reserve(endpoints_.size() - 1);
    for (auto it = endpoints_.cbegin(); it != endpoints_.cend(); ++it) {
        if (it.key() != endpointId)
            remaining.append(it.value().config);
    }
    return configure(remaining);
}

void RtpGroupBridge::clear()
{
    if (!ownerThread("clear"))
        return;

    session_.stop();
    session_.setPayloadGroups({});
    router_.reset();
    endpoints_.clear();
    mediaHandlers_.clear();
}

void RtpGroupBridge::setNetworkPacketHandler(NetworkPacketHandler handler)
{
    if (!ownerThread("setNetworkPacketHandler"))
        return;
    networkPacketHandler_ = std::move(handler);
}

void RtpGroupBridge::setEndpointMediaPacketHandler(const QByteArray &endpointId, MediaPacketHandler handler)
{
    if (!ownerThread("setEndpointMediaPacketHandler"))
        return;
    if (handler)
        mediaHandlers_.insert(endpointId, std::move(handler));
    else
        mediaHandlers_.remove(endpointId);
}

void RtpGroupBridge::setRuntimeErrorHandler(RuntimeErrorHandler handler)
{
    if (!ownerThread("setRuntimeErrorHandler"))
        return;
    runtimeErrorHandler_ = std::move(handler);
}

bool RtpGroupBridge::start() { return ownerThread("start") && !endpoints_.isEmpty() && session_.start(); }

void RtpGroupBridge::stop()
{
    if (!ownerThread("stop"))
        return;
    session_.stop();
}

QByteArray RtpGroupBridge::bytesFromBuffer(GstBuffer *buffer)
{
    if (!buffer)
        return {};
    const auto size = gst_buffer_get_size(buffer);
    if (!size || size > size_t(std::numeric_limits<int>::max()))
        return {};
    QByteArray bytes;
    bytes.resize(int(size));
    if (gst_buffer_extract(buffer, 0, bytes.data(), size) != size)
        return {};
    return bytes;
}

bool RtpGroupBridge::rtpIdentity(const QByteArray &packet, quint8 *payloadType, quint32 *ssrc)
{
    if (packet.size() < 12)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(packet.constData());
    if ((p[0] >> 6) != 2)
        return false;
    if (payloadType)
        *payloadType = p[1] & 0x7f;
    if (ssrc)
        *ssrc = (quint32(p[8]) << 24) | (quint32(p[9]) << 16) | (quint32(p[10]) << 8) | quint32(p[11]);
    return true;
}

bool RtpGroupBridge::registerOutgoing(const QByteArray &endpointId, const QByteArray &packet)
{
    const auto endpoint = endpoints_.constFind(endpointId);
    if (endpoint == endpoints_.cend())
        return false;

    quint8  payloadType = 0;
    quint32 ssrc        = 0;
    if (!rtpIdentity(packet, &payloadType, &ssrc) || !ssrc || !endpoint->outgoingPayloadTypes.contains(payloadType))
        return false;
    return router_.registerOutgoingSsrc(endpointId, ssrc);
}

GstFlowReturn RtpGroupBridge::sendRtp(const QByteArray &endpointId, GstBuffer *buffer, GstClockTime presentationAge)
{
    if (!buffer)
        return GST_FLOW_ERROR;
    const auto endpoint = endpoints_.constFind(endpointId);
    if (endpoint == endpoints_.cend())
        return GST_FLOW_ERROR;

    auto bytes = bytesFromBuffer(buffer);
    if (bytes.isEmpty() || !registerOutgoing(endpointId, bytes))
        return GST_FLOW_ERROR;

    GstBuffer  *stampedBuffer = nullptr;
    const auto &route         = endpoint->config.route;
    if (!route.mid.isEmpty() && route.midExtensionId) {
        auto stamped = stampMidExtension(bytes, route.midExtensionId, route.mid);
        if (!stamped)
            return GST_FLOW_ERROR;
        bytes         = std::move(*stamped);
        stampedBuffer = bufferWithBytes(buffer, bytes);
        if (!stampedBuffer)
            return GST_FLOW_ERROR;
        buffer = stampedBuffer;
    }

    const auto result = session_.sendRtp(buffer, presentationAge);
    if (stampedBuffer)
        gst_buffer_unref(stampedBuffer);
    return result;
}

GstFlowReturn RtpGroupBridge::sendRtp(const QByteArray &endpointId, const PRtpPacket &packet)
{
    if (packet.type != PRtpPacket::Type::Rtp)
        return GST_FLOW_ERROR;
    const auto endpoint = endpoints_.constFind(endpointId);
    if (endpoint == endpoints_.cend() || !registerOutgoing(endpointId, packet.rawValue))
        return GST_FLOW_ERROR;

    PRtpPacket  outgoing = packet;
    const auto &route    = endpoint->config.route;
    if (!route.mid.isEmpty() && route.midExtensionId) {
        auto stamped = stampMidExtension(packet.rawValue, route.midExtensionId, route.mid);
        if (!stamped)
            return GST_FLOW_ERROR;
        outgoing.rawValue = std::move(*stamped);
    }
    return session_.sendRtp(outgoing);
}

GstFlowReturn RtpGroupBridge::receivePacket(const PRtpPacket &packet)
{
    if (packet.rawValue.isEmpty())
        return GST_FLOW_ERROR;
    if (packet.type == PRtpPacket::Type::Rtp) {
        // Authentication must already have succeeded at the secure boundary.
        // Route before rtpsession so an ambiguous/unknown stream cannot mutate
        // group RTP state or teach a route.
        if (!router_.routeIncomingRtp(packet.rawValue))
            return GST_FLOW_ERROR;
    } else if (packet.type == PRtpPacket::Type::Rtcp) {
        if (!router_.validateRtcp(packet.rawValue))
            return GST_FLOW_ERROR;
    } else {
        return GST_FLOW_ERROR;
    }
    return session_.receivePacket(packet);
}

void RtpGroupBridge::deliverIncomingRtp(GstBuffer *buffer)
{
    const auto bytes  = bytesFromBuffer(buffer);
    auto       routed = router_.routeIncomingRtp(bytes);
    if (!routed || !router_.isCurrent(*routed))
        return;

    const auto handler = mediaHandlers_.value(routed->endpointId);
    if (!handler)
        return;

    // External code may synchronously delete this group bridge. Do not access
    // any members after invoking it.
    QPointer<RtpGroupBridge> guard(this);
    handler(buffer);
    if (!guard)
        return;
}

void RtpGroupBridge::emitRuntimeError()
{
    const auto handler = runtimeErrorHandler_;
    if (!handler)
        return;
    QPointer<RtpGroupBridge> guard(this);
    handler();
    if (!guard)
        return;
}

} // namespace PsiMedia
