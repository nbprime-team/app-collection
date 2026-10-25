#include "clock_desktop.h"

#include <thread>

namespace nmarkdown {

ClockDesktop::ClockDesktop() : started_at_(std::chrono::steady_clock::now()) {}

std::uint64_t ClockDesktop::milliseconds() const {
    const auto elapsed = std::chrono::steady_clock::now() - started_at_;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void ClockDesktop::sleep_ms(std::uint32_t duration) {
    std::this_thread::sleep_for(std::chrono::milliseconds(duration));
}

}  // namespace nmarkdown

