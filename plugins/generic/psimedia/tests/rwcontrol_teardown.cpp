/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "gstthread.h"
#include "rwcontrol.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <thread>

#include <gst/gst.h>

namespace PsiMedia {

class RwControlRemoteLifecycleTest {
public:
    static bool queuedImagesAreReleased()
    {
        GstMainLoop loop(QString {});
        QSemaphore  ready;
        QObject::connect(&loop, &GstMainLoop::started, &loop, [&ready] { ready.release(); }, Qt::DirectConnection);
        std::thread worker([&loop] {
            auto context = g_main_context_new();
            g_main_context_push_thread_default(context);
            loop.start();
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
        });
        if (!ready.tryAcquire(1, 5000))
            qFatal("Media loop failed to start");
        bool passed = false;
        {
            RwControlLocal local(&loop, nullptr);
            struct ImageStorage {
                int  *released;
                uchar pixels[8 * 8 * 4] {};
            };
            int released = 0;
            int preview = 0, output = 0;
            QObject::connect(&local, &RwControlLocal::previewFrame, &local, [&](const QImage &) { ++preview; });
            QObject::connect(&local, &RwControlLocal::outputFrame, &local, [&](const QImage &) { ++output; });
            // Keep the Qt event loop stalled while producers submit frames.
            // Observe actual image backing-store release, not the queue's
            // implementation-specific capacity or the process allocator's RSS.
            constexpr int frames = 2000;
            for (int i = 0; i < frames; ++i) {
                auto storage         = new ImageStorage;
                storage->released    = &released;
                auto message         = new RwControlFrameMessage;
                message->frame.type  = i % 2 ? RwControlFrame::Preview : RwControlFrame::Output;
                message->frame.image = QImage(
                    storage->pixels, 8, 8, QImage::Format_RGB32,
                    [](void *data) {
                        auto storage = static_cast<ImageStorage *>(data);
                        ++*storage->released;
                        delete storage;
                    },
                    storage);
                local.postMessage(message);
            }
            const bool releasedWhileStalled = released > 0;
            local.processMessages();
            passed = releasedWhileStalled && released == frames && preview == 1 && output == 1;
            if (!passed)
                qWarning("Frame backing stores released=%d/%d preview=%d output=%d", released, frames, preview, output);

            // Commands discarded after stop must also release their payloads.
            struct Command final : RwControlTransmitMessage {
                explicit Command(int &released) : released(released) { }
                ~Command() override { ++released; }
                int &released;
            };
            int  discarded = 0;
            auto context   = g_main_context_new();
            {
                RwControlRemote remote(context, nullptr, &local);
                remote.postMessage(new RwControlStopMessage);
                for (int i = 0; i < 100; ++i)
                    remote.postMessage(new Command(discarded));
                g_main_context_iteration(context, FALSE);
            }
            g_main_context_unref(context);
            passed = passed && discarded == 100;
        }
        loop.stop();
        worker.join();
        return passed;
    }

    static bool destroyWithPendingDispatch()
    {
        GMainContext *context = g_main_context_new();
        if (!context)
            return false;

        {
            // Do not iterate the context before destruction: postMessage()
            // must leave a dispatch source queued with this remote as callback
            // data. The destructor is responsible for removing that source.
            auto *remote = new RwControlRemote(context, nullptr, nullptr);

            auto *transmit              = new RwControlTransmitMessage;
            transmit->transmit.useAudio = true;
            remote->postMessage(transmit);

            auto *devices               = new RwControlUpdateDevicesMessage;
            devices->devices.audioOutId = QStringLiteral("queued-before-destroy");
            remote->postMessage(devices);

            delete remote;
        }

        // Force the same main context to dispatch after the remote has gone.
        // If its queued source retained a stale callback/data pair, ASan catches
        // the use-after-free here. A marker guarantees that the context really
        // performed a post-destruction dispatch turn.
        bool     markerDispatched = false;
        GSource *marker           = g_idle_source_new();
        g_source_set_callback(
            marker,
            [](gpointer data) -> gboolean {
                *static_cast<bool *>(data) = true;
                return G_SOURCE_REMOVE;
            },
            &markerDispatched, nullptr);
        g_source_attach(marker, context);
        g_source_unref(marker);

        for (int i = 0; i < 16 && !markerDispatched; ++i)
            g_main_context_iteration(context, FALSE);

        // Drain anything else that was made ready in the same turn. A destroyed
        // RwControl source must never execute during this drain.
        while (g_main_context_pending(context))
            g_main_context_iteration(context, FALSE);

        g_main_context_unref(context);
        return markerDispatched;
    }
};

} // namespace PsiMedia

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    if (!PsiMedia::RwControlRemoteLifecycleTest::destroyWithPendingDispatch())
        return 1;

    if (!PsiMedia::RwControlRemoteLifecycleTest::queuedImagesAreReleased())
        return 1;

    qInfo("RwControl pending-dispatch teardown and queue ownership regressions passed");
    return 0;
}
