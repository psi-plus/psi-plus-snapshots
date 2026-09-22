// SPDX-License-Identifier: LGPL-2.1-or-later
// Compile the worker into this TU to observe its legacy file-local ownership
// flag without adding a public media API for transport/pipeline internals.
#include "../gstprovider/rtpworker.cpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    gst_init(nullptr, nullptr);
    auto *context = g_main_context_default();
    {
        PsiMedia::RtpWorker    owner(context, nullptr);
        PsiMedia::PAudioParams audio;
        audio.codec            = QStringLiteral("opus");
        audio.sampleRate       = 48000;
        audio.sampleSize       = 16;
        audio.channels         = 1;
        owner.localAudioParams = { audio };
        owner.ain              = QStringLiteral("audiotestsrc is-live=true wave=sine");
        struct Result {
            bool done   = false;
            bool failed = false;
        } result;
        owner.app        = &result;
        owner.cb_started = [](void *p) { static_cast<Result *>(p)->done = true; };
        owner.cb_error   = [](void *p) {
            auto &r = *static_cast<Result *>(p);
            r.done = r.failed = true;
        };
        owner.start();
        QElapsedTimer timer;
        timer.start();
        while (!result.done && timer.elapsed() < 15000) {
            g_main_context_iteration(context, false);
            QThread::msleep(1);
        }
        if (!result.done || result.failed || !PsiMedia::send_in_use)
            qFatal("Could not establish the synthetic sender");
        {
            PsiMedia::RtpWorker neverStarted(context, nullptr);
        }
        if (!PsiMedia::send_in_use)
            qFatal("Unrelated worker destruction released the active sender's ownership");
    }
    if (PsiMedia::send_in_use)
        qFatal("Sender destruction did not release ownership");
    qInfo("Sender ownership regression passed");
}
