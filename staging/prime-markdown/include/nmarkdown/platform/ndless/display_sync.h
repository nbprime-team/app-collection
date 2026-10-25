#ifndef NMARKDOWN_PLATFORM_NDLESS_DISPLAY_SYNC_H
#define NMARKDOWN_PLATFORM_NDLESS_DISPLAY_SYNC_H

#include <cstdint>

namespace nmarkdown {
namespace display_sync {

constexpr int kLandscapeWidth = 320;
constexpr int kLandscapeHeight = 240;
constexpr int kNativeWidth = 240;
constexpr int kNativeHeight = 320;

// HW-W/CX II scanout is physically 240x320. Preparing that layout before the
// vertical boundary keeps the synchronized LCD update to one contiguous copy.
inline void stage_landscape_rgb565_for_native(
    const std::uint16_t* landscape, std::uint16_t* native) {
    for (int native_y = 0; native_y < kNativeHeight; ++native_y) {
        std::uint16_t* const output_row =
            native + native_y * kNativeWidth;
        const std::uint16_t* input = landscape + native_y;
        for (int native_x = 0; native_x < kNativeWidth; ++native_x) {
            output_row[native_x] = *input;
            input += kLandscapeWidth;
        }
    }
}

enum class VerticalCompareAction : std::uint8_t {
    Wait,
    AcknowledgeCurrent,
    Present,
};

// A latched vertical-compare bit that was already asserted when presentation
// began belongs to the old frame. Require an idle sample before accepting the
// next assertion, while acknowledging a still-latched old event as needed.
class VerticalCompareEdge final {
public:
    explicit VerticalCompareEdge(bool idle_seen) : idle_seen_(idle_seen) {}

    VerticalCompareAction observe(bool asserted) {
        if (!asserted) {
            idle_seen_ = true;
            return VerticalCompareAction::Wait;
        }
        return idle_seen_ ? VerticalCompareAction::Present
                          : VerticalCompareAction::AcknowledgeCurrent;
    }

private:
    bool idle_seen_ = false;
};

}  // namespace display_sync
}  // namespace nmarkdown

#endif
