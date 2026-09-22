// SPDX-License-Identifier: LGPL-2.1-or-later

#include "../gstprovider/rtpworker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <functional>

namespace {

struct Result {
    bool started = false;
    bool updated = false;
    bool stopped = false;
    bool failed  = false;
};

bool spinUntil(GMainContext *context, const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        g_main_context_iteration(context, false);
        QThread::msleep(1);
    }
    return done();
}

PsiMedia::PPayloadInfo opusPayload()
{
    PsiMedia::PPayloadInfo payload;
    payload.id        = 111;
    payload.name      = QStringLiteral("opus");
    payload.clockrate = 48000;
    payload.channels  = 2;
    return payload;
}

PsiMedia::PPayloadInfo vp8Payload()
{
    PsiMedia::PPayloadInfo payload;
    payload.id        = 96;
    payload.name      = QStringLiteral("VP8");
    payload.clockrate = 90000;
    return payload;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    gst_init(nullptr, nullptr);
    auto *context = g_main_context_default();

    PsiMedia::RtpWorker worker(context, nullptr);
    Result              result;
    worker.app        = &result;
    worker.cb_started = [](void *p) { static_cast<Result *>(p)->started = true; };
    worker.cb_updated = [](void *p) { static_cast<Result *>(p)->updated = true; };
    worker.cb_stopped = [](void *p) { static_cast<Result *>(p)->stopped = true; };
    worker.cb_error   = [](void *p) { static_cast<Result *>(p)->failed = true; };

    // Reproduce the Conversations ordering that exposed the bug: video is
    // negotiated first and starts the receive graph before audio arrives.
    PsiMedia::PVideoParams video;
    video.codec                   = QStringLiteral("vp8");
    worker.localVideoParams       = { video };
    worker.remoteVideoPayloadInfo = { vp8Payload() };
    worker.start();

    if (!spinUntil(context, [&] { return result.started || result.failed; }) || result.failed)
        qFatal("Could not establish video-first receive graph");
    if (worker.remoteVideoPayloadInfo.isEmpty())
        qFatal("Video negotiation was not committed");

    // Audio arrives second. It must be grafted onto the active recvbin rather
    // than silently discarded merely because video already owns the graph.
    PsiMedia::PAudioParams audio;
    audio.codec                   = QStringLiteral("opus");
    audio.sampleRate              = 48000;
    audio.sampleSize              = 16;
    audio.channels                = 2;
    worker.localAudioParams       = { audio };
    worker.remoteAudioPayloadInfo = { opusPayload() };
    result.updated                = false;
    worker.update();

    if (!spinUntil(context, [&] { return result.updated || result.failed; }) || result.failed)
        qFatal("Could not add audio to video-first receive graph");
    if (worker.remoteAudioPayloadInfo.isEmpty())
        qFatal("Audio negotiation was lost after video-first receive setup");

    worker.stop();
    if (!spinUntil(context, [&] { return result.stopped; }))
        qFatal("Receive-order worker did not stop cleanly");

    qInfo("Video-first audio receive transition regression passed");
    return 0;
}
