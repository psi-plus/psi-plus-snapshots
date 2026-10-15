// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2009-2024 Psi IM team
#pragma once

#include <functional>
#include <glib.h>

namespace PsiMedia {
class GlibTimer {
    GMainContext         *ctx;
    guint                 timer = 0;
    guint                 interval;
    std::function<bool()> callback; // return true to continue, false to stop
    bool                  stopRequested    = false;
    bool                  restartRequested = false;
    bool                  inHandler        = false;

public:
    GlibTimer(GMainContext *ctx, guint interval, std::function<bool()> callback) :
        ctx(ctx), interval(interval), callback(callback)
    {
    }

    ~GlibTimer() { stop(); }

    inline bool isActive() const { return !!timer; }

    void start()
    {
        stopRequested = false;
        if (timer) {
            stop();
            if (inHandler) {
                restartRequested = true;
                return;
            }
        }
        // qDebug("starting timer");
        struct TimerCallback {
            static gboolean call(gpointer data)
            {
                auto priv       = reinterpret_cast<GlibTimer *>(data);
                priv->inHandler = true;
                auto ret        = priv->callback();
                if (!ret) {
                    priv->timer = 0;
                }
                priv->inHandler        = false;
                auto restartRequested  = priv->restartRequested;
                auto stopRequested     = priv->stopRequested;
                priv->restartRequested = false;
                priv->stopRequested    = false;
                return restartRequested ? TRUE : stopRequested ? FALSE : ret;
            }
        };
        timer = g_timeout_add(interval, &TimerCallback::call, this);
    }

    void stop()
    {
        // qDebug("stopping timer");
        restartRequested = false;
        if (!timer) {
            return;
        }
        if (inHandler) {
            stopRequested = true;
            return;
        }
        auto source = g_main_context_find_source_by_id(ctx, timer);
        if (source) {
            // The lookup returns a borrowed pointer owned by the context.
            // We did not acquire a reference to release after destroying it.
            g_source_destroy(source);
        }
        timer = 0;
    }
};
}
