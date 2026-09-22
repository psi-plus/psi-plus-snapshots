/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rwcontrol.h"

#include <QCoreApplication>

#include <gst/gst.h>

namespace PsiMedia {

class RwControlRemoteLifecycleTest {
public:
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

    qInfo("RwControl pending-dispatch teardown regression passed");
    return 0;
}
