// SPDX-License-Identifier: LGPL-2.1-or-later
#include "glibtimer_p.h"

int main()
{
    auto *context = g_main_context_default();
    int   calls   = 0;
    {
        PsiMedia::GlibTimer timer(context, 60000, [&] {
            ++calls;
            return false;
        });
        timer.start();
        g_assert_true(timer.isActive());
        timer.stop(); // The context owns the reference returned by find_source_by_id.
        g_assert_false(timer.isActive());
        timer.stop(); // Idempotent.
        timer.start();
        timer.start(); // Replacement stops the old source.
    } // Destruction must stop an active source without unref'ing a borrowed pointer.
    g_assert_cmpint(calls, ==, 0);
    {
        PsiMedia::GlibTimer timer(context, 0, [&] {
            ++calls;
            return false;
        });
        timer.start();
        g_main_context_iteration(context, false);
        g_assert_false(timer.isActive());
    } // A naturally completed timer has no remaining source to stop.
    g_assert_cmpint(calls, ==, 1);
}
