// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <iris/jingle-rtp.h>

namespace XMPP { namespace Jingle { namespace RTP {

    struct ApplicationTestAccess {
        static bool allowsRtp(const Application &application, bool sending) { return application.allowsRtp(sending); }
    };

}}}
