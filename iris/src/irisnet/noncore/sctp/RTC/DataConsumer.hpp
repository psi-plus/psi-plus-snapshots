#pragma once

#include "SctpParameters.hpp"

namespace RTC {

class DataConsumer {

public:
    SctpParameters        sctpParameters;
    const SctpParameters &GetSctpStreamParameters() const { return sctpParameters; }
};

}
