// SPDX-License-Identifier: LGPL-2.1-or-later

#include "../gstprovider/rtpworker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <atomic>
#include <functional>

namespace {

struct Result {
    PsiMedia::RtpWorker *worker = nullptr;
    std::atomic_bool     started { false };
    std::atomic_bool     updated { false };
    std::atomic_bool     stopped { false };
    std::atomic_bool     failed { false };
    std::atomic_bool     pauseFromCallback { false };
    std::atomic_bool     pausedFromCallback { false };
    std::atomic_bool     pauseVideoFromCallback { false };
    std::atomic_bool     pausedVideoFromCallback { false };
    std::atomic_int      audioPackets { 0 };
    std::atomic_int      videoPackets { 0 };
    std::atomic<quint32> firstAudioSsrc { 0 };
    std::atomic<quint32> lastAudioSsrc { 0 };
};

bool spinUntil(GMainContext *context, const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        g_main_context_iteration(context, false);
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return done();
}

quint32 rtpSsrc(GstBuffer *buffer)
{
    if (!buffer)
        return 0;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return 0;
    quint32 ssrc = 0;
    if (map.size >= 12) {
        ssrc = (quint32(map.data[8]) << 24) | (quint32(map.data[9]) << 16) | (quint32(map.data[10]) << 8)
            | quint32(map.data[11]);
    }
    gst_buffer_unmap(buffer, &map);
    return ssrc;
}

PsiMedia::PAudioParams opusParams()
{
    PsiMedia::PAudioParams audio;
    audio.codec      = QStringLiteral("opus");
    audio.sampleRate = 48000;
    audio.sampleSize = 16;
    audio.channels   = 1;
    return audio;
}

PsiMedia::PVideoParams vp8Params()
{
    PsiMedia::PVideoParams video;
    video.codec = QStringLiteral("vp8");
    video.size  = QSize(640, 480);
    video.fps   = 30;
    return video;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    gst_init(nullptr, nullptr);
    auto *context = g_main_context_default();

    PsiMedia::RtpWorker worker(context, nullptr);
    Result              result;
    result.worker         = &worker;
    worker.app            = &result;
    worker.cb_started     = [](void *p) { static_cast<Result *>(p)->started = true; };
    worker.cb_updated     = [](void *p) { static_cast<Result *>(p)->updated = true; };
    worker.cb_stopped     = [](void *p) { static_cast<Result *>(p)->stopped = true; };
    worker.cb_error       = [](void *p) { static_cast<Result *>(p)->failed = true; };
    worker.cb_rtpAudioOut = [](const PsiMedia::RtpWorker::EncodedRtpPacket &packet, void *p) {
        auto         &r        = *static_cast<Result *>(p);
        const quint32 ssrc     = rtpSsrc(packet.buffer);
        quint32       expected = 0;
        r.firstAudioSsrc.compare_exchange_strong(expected, ssrc);
        r.lastAudioSsrc.store(ssrc, std::memory_order_release);
        r.audioPackets.fetch_add(1, std::memory_order_release);

        // Regression: callbacks must run outside rtpaudioout_mutex. Before the
        // fix this re-entrant pauseAudio() deadlocked on the same mutex.
        if (r.pauseFromCallback.exchange(false, std::memory_order_acq_rel)) {
            r.worker->pauseAudio();
            r.pausedFromCallback.store(true, std::memory_order_release);
        }
    };
    worker.cb_rtpVideoOut = [](const PsiMedia::RtpWorker::EncodedRtpPacket &, void *p) {
        auto &r = *static_cast<Result *>(p);
        r.videoPackets.fetch_add(1, std::memory_order_release);
        if (r.pauseVideoFromCallback.exchange(false, std::memory_order_acq_rel)) {
            r.worker->pauseVideo();
            r.pausedVideoFromCallback.store(true, std::memory_order_release);
        }
    };

    const QString audioSource = QStringLiteral("audiotestsrc is-live=true wave=sine");
    const QString videoSource = QStringLiteral("videotestsrc is-live=true pattern=ball");

    worker.localAudioParams = { opusParams() };
    worker.setInputDevices(audioSource, QString(), QString(), QByteArray(), false);
    worker.start();
    if (!spinUntil(context,
                   [&] {
                       return result.started.load(std::memory_order_acquire)
                           || result.failed.load(std::memory_order_acquire);
                   })
        || result.failed.load(std::memory_order_acquire))
        qFatal("Could not establish audio-first sender");

    worker.transmitAudio();
    if (!spinUntil(context, [&] { return result.audioPackets.load(std::memory_order_acquire) >= 5; }))
        qFatal("Audio-first sender produced no RTP");
    const quint32 originalAudioSsrc = result.firstAudioSsrc.load(std::memory_order_acquire);
    if (!originalAudioSsrc)
        qFatal("Audio-first sender exposed no RTP SSRC");

    // Re-enter the transmit control from the streaming callback. This is the
    // exact lock order that used to self-deadlock.
    result.pauseFromCallback.store(true, std::memory_order_release);
    if (!spinUntil(context, [&] { return result.pausedFromCallback.load(std::memory_order_acquire); }, 5000))
        qFatal("RTP callback deadlocked while pausing audio");
    const int audioPacketsBeforeResume = result.audioPackets.load(std::memory_order_acquire);
    worker.transmitAudio();
    if (!spinUntil(
            context,
            [&] { return result.audioPackets.load(std::memory_order_acquire) >= audioPacketsBeforeResume + 5; }, 5000))
        qFatal("Audio sender did not resume after callback pause");

    // Preserve a transmit request made before the second media branch exists:
    // the hot-added branch must begin forwarding RTP without another toggle.
    worker.transmitVideo();

    // This used to call cleanupSend() merely because videoInput changed from
    // empty to non-empty, rebuilding audio and delaying video startup.
    worker.setInputDevices(audioSource, videoSource, QString(), QByteArray(), false);
    worker.localVideoParams = { vp8Params() };
    result.updated.store(false, std::memory_order_release);
    worker.update();
    if (!spinUntil(
            context,
            [&] {
                return result.updated.load(std::memory_order_acquire) || result.failed.load(std::memory_order_acquire);
            },
            15000)
        || result.failed.load(std::memory_order_acquire))
        qFatal("Could not hot-add video to active audio sender");

    const int audioPacketsAtUpdate = result.audioPackets.load(std::memory_order_acquire);
    if (!spinUntil(
            context,
            [&] {
                return result.videoPackets.load(std::memory_order_acquire) >= 5
                    && result.audioPackets.load(std::memory_order_acquire) >= audioPacketsAtUpdate + 5;
            },
            10000)) {
        qFatal("Hot-added video/audio sender did not continue producing RTP");
    }

    if (worker.localVideoPayloadInfo.isEmpty() || !worker.canTransmitVideo)
        qFatal("Hot-added video negotiation was not committed");
    if (result.lastAudioSsrc.load(std::memory_order_acquire) != originalAudioSsrc)
        qFatal("Adding video rebuilt the live audio sender");

    result.pauseVideoFromCallback.store(true, std::memory_order_release);
    if (!spinUntil(context, [&] { return result.pausedVideoFromCallback.load(std::memory_order_acquire); }, 5000))
        qFatal("RTP callback deadlocked while pausing video");
    const int videoPacketsBeforeResume = result.videoPackets.load(std::memory_order_acquire);
    worker.transmitVideo();
    if (!spinUntil(
            context,
            [&] { return result.videoPackets.load(std::memory_order_acquire) >= videoPacketsBeforeResume + 3; }, 5000))
        qFatal("Video sender did not resume after callback pause");

    worker.stop();
    if (!spinUntil(context, [&] { return result.stopped.load(std::memory_order_acquire); }))
        qFatal("Sender-order worker did not stop cleanly");

    // A hot-added live camera is allowed to take time before producing its
    // first frame/caps. appsrc with no producer models that startup window:
    // adding the video branch must complete without waiting for current caps
    // or tearing down the already-running audio sender.
    {
        PsiMedia::RtpWorker delayedWorker(context, nullptr);
        Result              delayed;
        delayed.worker               = &delayedWorker;
        delayedWorker.app            = &delayed;
        delayedWorker.cb_started     = [](void *p) { static_cast<Result *>(p)->started = true; };
        delayedWorker.cb_updated     = [](void *p) { static_cast<Result *>(p)->updated = true; };
        delayedWorker.cb_stopped     = [](void *p) { static_cast<Result *>(p)->stopped = true; };
        delayedWorker.cb_error       = [](void *p) { static_cast<Result *>(p)->failed = true; };
        delayedWorker.cb_rtpAudioOut = [](const PsiMedia::RtpWorker::EncodedRtpPacket &, void *p) {
            static_cast<Result *>(p)->audioPackets.fetch_add(1, std::memory_order_release);
        };

        delayedWorker.localAudioParams = { opusParams() };
        delayedWorker.setInputDevices(audioSource, QString(), QString(), QByteArray(), false);
        delayedWorker.start();
        if (!spinUntil(
                context,
                [&] {
                    return delayed.started.load(std::memory_order_acquire)
                        || delayed.failed.load(std::memory_order_acquire);
                },
                10000)
            || delayed.failed.load(std::memory_order_acquire)) {
            qFatal("Could not establish delayed-video audio sender");
        }

        delayedWorker.transmitAudio();
        if (!spinUntil(context, [&] { return delayed.audioPackets.load(std::memory_order_acquire) >= 5; }, 5000))
            qFatal("Delayed-video setup produced no audio RTP");

        delayedWorker.transmitVideo();
        delayedWorker.setInputDevices(audioSource, QStringLiteral("appsrc is-live=true format=time"), QString(),
                                      QByteArray(), false);
        delayedWorker.localVideoParams    = { vp8Params() };
        const int audioBeforeDelayedVideo = delayed.audioPackets.load(std::memory_order_acquire);
        delayed.updated.store(false, std::memory_order_release);
        delayedWorker.update();
        if (!spinUntil(
                context,
                [&] {
                    return delayed.updated.load(std::memory_order_acquire)
                        || delayed.failed.load(std::memory_order_acquire);
                },
                2000)
            || delayed.failed.load(std::memory_order_acquire)) {
            qFatal("Hot-added delayed video waited for first caps or failed");
        }
        if (delayedWorker.localVideoPayloadInfo.isEmpty() || !delayedWorker.canTransmitVideo)
            qFatal("Delayed video payload negotiation was not committed");
        if (!spinUntil(
                context,
                [&] { return delayed.audioPackets.load(std::memory_order_acquire) >= audioBeforeDelayedVideo + 5; },
                5000)) {
            qFatal("Delayed video hot-add interrupted the running audio sender");
        }

        delayedWorker.stop();
        if (!spinUntil(context, [&] { return delayed.stopped.load(std::memory_order_acquire); }))
            qFatal("Delayed-video worker did not stop cleanly");
    }

    qInfo("Audio-first video send transition regression passed");
    return 0;
}
