#include "clock_ndless.h"

#include <algorithm>

#include <libndls.h>

namespace nmarkdown {
namespace {

// Ndless msleep() is not a scheduler sleep on CX/CX II. It reprograms the
// SP804 timer, masks every other IRQ, and waits in WFI until the timer status
// changes. There is no timeout, so one missed wakeup permanently strands this
// application's polled input loop (including Ctrl+Esc). Keep the idle delay
// bounded and local instead: this costs some idle power, but it cannot turn a
// transient timer/IRQ problem into an unrecoverable application freeze.
//
// The loop counts are approximate for the normal CX (132 MHz) and CX II
// (396 MHz) clocks. Exact timing is not part of Clock's contract here: the
// logical clock below remains deterministic and key repeat only needs the
// polling loop to be paced into a reasonable range.
constexpr std::uint32_t kMaximumBusyDelayMs = 20;
constexpr std::uint32_t kCxLoopsPerMillisecond = 44000;
constexpr std::uint32_t kCx2LoopsPerMillisecond = 132000;

void bounded_idle_delay(std::uint32_t duration) {
    const std::uint32_t bounded = std::min(duration, kMaximumBusyDelayMs);
    std::uint32_t remaining = bounded *
        (is_cx2 ? kCx2LoopsPerMillisecond : kCxLoopsPerMillisecond);
    while (remaining-- != 0) {
        __asm__ volatile("nop");
    }
}

}  // namespace

std::uint64_t ClockNdless::milliseconds() const {
    return logical_milliseconds_;
}

void ClockNdless::sleep_ms(std::uint32_t duration) {
    bounded_idle_delay(duration);
    logical_milliseconds_ += duration;
}

}  // namespace nmarkdown
