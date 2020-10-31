#pragma once

#include "SctpParameters.hpp"

namespace RTC {

class DataConsumer {
    SctpParameters sctpParameters;

public:
    const SctpParameters &GetSctpStreamParameters() const { return sctpParameters; }
};

}
