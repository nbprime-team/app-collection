#ifndef NMARKDOWN_PLATFORM_DESKTOP_CLOCK_DESKTOP_H
#define NMARKDOWN_PLATFORM_DESKTOP_CLOCK_DESKTOP_H

#include <chrono>

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

class ClockDesktop final : public Clock {
public:
    ClockDesktop();

    std::uint64_t milliseconds() const override;
    void sleep_ms(std::uint32_t duration) override;

private:
    std::chrono::steady_clock::time_point started_at_;
};

}  // namespace nmarkdown

#endif

