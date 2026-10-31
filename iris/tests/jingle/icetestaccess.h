// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "../../src/xmpp/xmpp-im/jingle-ice-pad_p.h"

namespace XMPP { namespace Jingle { namespace ICE {

    struct TransportTestAccess {
        static bool      iceCanSendMedia(const Transport &transport) { return transport.iceCanSendMedia(); }
        static qsizetype liveAssociationCount(const Pad &pad) { return pad.d->liveAssociationCount(); }
    };

}}}
