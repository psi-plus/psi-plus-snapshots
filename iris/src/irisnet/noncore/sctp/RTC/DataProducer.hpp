#pragma once

#include "SctpParameters.hpp"

namespace RTC {

class DataProducer {
    SctpParameters sctpParameters;

public:
    const SctpParameters &GetSctpStreamParameters() const { return sctpParameters; }
};

}
