/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "gstrtpsessioncontext.h"

#include <QCoreApplication>
#include <QMutexLocker>

#include <chrono>

using namespace std::chrono_literals;

namespace PsiMedia {

struct GstRtpSessionContextTestAccess {
    static void enableAudioProducer(GstRtpSessionContext &session, const QByteArray &associationId,
                                    const QByteArray &endpointId, quint64 epoch)
    {
        QMutexLocker locker(&session.secureOutgoingMutex_);
        session.audioSecureProducer_.associationId = associationId;
        session.audioSecureProducer_.endpointId    = endpointId;
        session.audioSecureProducer_.epoch         = epoch;
        session.audioSecureProducer_.enabled       = true;
    }

    static void enqueue(GstRtpSessionContext &session, GstBuffer *buffer)
    {
        RtpWorker::EncodedRtpPacket packet;
        packet.buffer          = buffer;
        packet.presentationAge = 5 * GST_MSECOND;
        session.enqueueSecureOutgoing(true, packet);
    }

    static int queueSize(GstRtpSessionContext &session)
    {
        QMutexLocker locker(&session.secureOutgoingMutex_);
        return session.secureOutgoingQueue_.size();
    }

    static quint64 queuedBytes(GstRtpSessionContext &session)
    {
        QMutexLocker locker(&session.secureOutgoingMutex_);
        return session.secureOutgoingBytes_;
    }

    static quint64 generation(GstRtpSessionContext &session)
    {
        QMutexLocker locker(&session.secureOutgoingMutex_);
        return session.secureRouteGeneration_;
    }

    static void expireFront(GstRtpSessionContext &session)
    {
        QMutexLocker locker(&session.secureOutgoingMutex_);
        if (!session.secureOutgoingQueue_.isEmpty())
            session.secureOutgoingQueue_.head().enqueuedAt = std::chrono::steady_clock::now() - 2s;
        session.purgeExpiredSecureOutgoingLocked();
    }

    static void changeRouteGeneration(GstRtpSessionContext &session) { session.refreshSecureProducerRoutes(); }
};

} // namespace PsiMedia

namespace {

void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    PsiMedia::GstRtpSessionContext session(nullptr, nullptr, nullptr, true);
    PsiMedia::GstRtpSessionContextTestAccess::enableAudioProducer(session, QByteArrayLiteral("audio-association"),
                                                                  QByteArrayLiteral("audio-endpoint"), 9);

    constexpr int PacketBytes = 4096;
    GstBuffer    *source      = gst_buffer_new_allocate(nullptr, PacketBytes, nullptr);
    check(source != nullptr, "failed to allocate queue test buffer");

    // Do not process the Qt event loop: the one scheduled drain stays pending,
    // allowing deterministic observation of the producer-side bounds.
    for (int i = 0; i < 1000; ++i)
        PsiMedia::GstRtpSessionContextTestAccess::enqueue(session, source);

    const int expectedByByteLimit = (512 * 1024) / PacketBytes;
    check(PsiMedia::GstRtpSessionContextTestAccess::queueSize(session) == expectedByByteLimit,
          "secure producer queue did not enforce byte bound");
    check(PsiMedia::GstRtpSessionContextTestAccess::queuedBytes(session) == quint64(512 * 1024),
          "secure producer queue byte accounting mismatch");

    PsiMedia::GstRtpSessionContextTestAccess::expireFront(session);
    check(PsiMedia::GstRtpSessionContextTestAccess::queueSize(session) == expectedByByteLimit - 1,
          "expired secure producer packet was not evicted");
    check(PsiMedia::GstRtpSessionContextTestAccess::queuedBytes(session)
              == quint64((expectedByByteLimit - 1) * PacketBytes),
          "secure producer queue age-eviction accounting mismatch");

    const quint64 oldGeneration = PsiMedia::GstRtpSessionContextTestAccess::generation(session);
    PsiMedia::GstRtpSessionContextTestAccess::changeRouteGeneration(session);
    check(PsiMedia::GstRtpSessionContextTestAccess::generation(session) != oldGeneration,
          "secure producer route generation did not advance");
    check(PsiMedia::GstRtpSessionContextTestAccess::queueSize(session) == 0
              && PsiMedia::GstRtpSessionContextTestAccess::queuedBytes(session) == 0,
          "route generation change did not fence and clear queued RTP");

    gst_buffer_unref(source);
    qInfo("secure RTP producer queue bounds regression passed");
    return 0;
}
