#pragma once

#include <QtGlobal>

namespace RTC {

struct SctpParameters {
    quint16 streamId          = 0;
    bool    ordered           = false;
    quint32 maxPacketLifeTime = 0;
    quint32 maxRetransmits    = 0;
};

}
