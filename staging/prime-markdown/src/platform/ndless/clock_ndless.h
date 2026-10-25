#ifndef NMARKDOWN_PLATFORM_NDLESS_CLOCK_NDLESS_H
#define NMARKDOWN_PLATFORM_NDLESS_CLOCK_NDLESS_H

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

class ClockNdless final : public Clock {
public:
    std::uint64_t milliseconds() const override;
    void sleep_ms(std::uint32_t duration) override;
    bool has_hardware_timer() const { return false; }

private:
    std::uint64_t logical_milliseconds_ = 0;
};

}  // namespace nmarkdown

#endif
