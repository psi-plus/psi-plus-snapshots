// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise production worker delivery without depending on pipeline ownership flags.
#include "../gstprovider/rtpworker.cpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    gst_init(nullptr, nullptr);
    auto *context = g_main_context_default();
    struct Result {
        bool             done   = false;
        bool             failed = false;
        std::atomic<int> packets { 0 };
    } result;
    {
        PsiMedia::RtpWorker    owner(context, nullptr);
        PsiMedia::PAudioParams audio;
        audio.codec            = QStringLiteral("opus");
        audio.sampleRate       = 48000;
        audio.sampleSize       = 16;
        audio.channels         = 1;
        owner.localAudioParams = { audio };
        owner.ain              = QStringLiteral("audiotestsrc is-live=true wave=sine");
        owner.app              = &result;
        owner.cb_started       = [](void *p) { static_cast<Result *>(p)->done = true; };
        owner.cb_error         = [](void *p) {
            auto &r = *static_cast<Result *>(p);
            r.done = r.failed = true;
        };
        owner.cb_rtpAudioOut
            = [](const PsiMedia::RtpWorker::EncodedRtpPacket &, void *p) { ++static_cast<Result *>(p)->packets; };
        owner.start();
        QElapsedTimer timer;
        timer.start();
        while (!result.done && timer.elapsed() < 15000) {
            g_main_context_iteration(context, false);
            QThread::msleep(1);
        }
        if (!result.done || result.failed)
            qFatal("Could not establish the synthetic sender");
        {
            PsiMedia::RtpWorker neverStarted(context, nullptr);
        }
        owner.transmitAudio();
        timer.restart();
        while (result.packets < 3 && timer.elapsed() < 5000) {
            g_main_context_iteration(context, false);
            QThread::msleep(1);
        }
        if (result.packets < 3)
            qFatal("Unrelated worker destruction stopped active sender delivery");
        owner.pauseAudio();
    }
    qInfo("Sender ownership regression passed");
}
