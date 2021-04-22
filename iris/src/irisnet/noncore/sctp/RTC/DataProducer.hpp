#pragma once

#include "SctpParameters.hpp"

namespace RTC {

class DataProducer {
public:
    SctpParameters sctpParameters;

    const SctpParameters &GetSctpStreamParameters() const { return sctpParameters; }
};

}
