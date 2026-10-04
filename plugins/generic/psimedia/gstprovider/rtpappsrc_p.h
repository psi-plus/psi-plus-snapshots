// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef PSIMEDIA_RTPAPPSRC_P_H
#define PSIMEDIA_RTPAPPSRC_P_H

#include <QDebug>
#include <gst/app/gstappsrc.h>
#include <mutex>

namespace PsiMedia { namespace RtpInput {
    // RTP is live datagram traffic. A stalled codec/rtpbin must lose packets
    // rather than accumulate memory or block the Qt/GLib owner loop. max-bytes
    // alone only emits enough-data when appsrc's block property is false.
    constexpr guint64 MaxQueuedBytes = 512 * 1024;
    struct State {
        std::mutex mutex;
        guint64    dropped = 0;
    };
    inline GQuark stateKey() { return g_quark_from_static_string("psimedia-bounded-rtp-input"); }
    inline void   configure(GstAppSrc *source)
    {
        gst_app_src_set_max_bytes(source, MaxQueuedBytes);
        g_object_set(source, "block", FALSE, nullptr);
        g_object_set_qdata_full(G_OBJECT(source), stateKey(), new State,
                                [](gpointer value) { delete static_cast<State *>(value); });
    }
    // Takes ownership of buffer on every path, just like gst_app_src_push_buffer.
    // Callers hold a source reference; configure() runs before publishing it.
    inline GstFlowReturn push(GstAppSrc *source, GstBuffer *buffer)
    {
        auto state = static_cast<State *>(g_object_get_qdata(G_OBJECT(source), stateKey()));
        if (!state) {
            gst_buffer_unref(buffer);
            return GST_FLOW_ERROR;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto                  size   = gst_buffer_get_size(buffer);
        const auto                  queued = gst_app_src_get_current_level_bytes(source);
        if (!size || size > MaxQueuedBytes || queued > MaxQueuedBytes - size) {
            gst_buffer_unref(buffer);
            ++state->dropped;
            // Rate-limit overload diagnostics to powers of two.
            if ((state->dropped & (state->dropped - 1)) == 0)
                qWarning("psimedia appsrc=%s RTP input overload queued-bytes=%llu dropped=%llu",
                         GST_OBJECT_NAME(source), static_cast<unsigned long long>(queued),
                         static_cast<unsigned long long>(state->dropped));
            return GST_FLOW_OK;
        }
        return gst_app_src_push_buffer(source, buffer);
    }
} // namespace RtpInput
} // namespace PsiMedia
#endif
