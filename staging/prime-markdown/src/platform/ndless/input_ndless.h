#ifndef NMARKDOWN_PLATFORM_NDLESS_INPUT_NDLESS_H
#define NMARKDOWN_PLATFORM_NDLESS_INPUT_NDLESS_H

#include <cstdint>

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

class InputNdless final : public Input {
public:
    explicit InputNdless(Clock& clock);
    bool poll(InputEvent& event) override;
    bool interaction_active() const override;

private:
    enum class PhysicalKey : std::uint8_t {
        None,
        Escape,
        Scratchpad,
        Menu,
        Enter,
        TouchClick,
        Search,
        Settings,
        Diagnostics,
        OpenDocument,
        SearchNext,
        Bookmark,
        Catalog,
        Character,
        Delete,
        Plus,
        Minus,
        Tab,
        Number1,
        Number2,
        Number4,
        Number6,
        Number7,
        Number8,
        Up,
        Down,
        Left,
        Right,
    };

    enum class TouchKey : std::uint8_t {
        None,
        Click,
        Up,
        Down,
        Left,
        Right,
    };

    enum class TouchAxis : std::uint8_t {
        None,
        Horizontal,
        Vertical,
    };

    struct TouchSample {
        bool scanned = false;
        bool valid = false;
        bool contact = false;
        TouchKey key = TouchKey::None;
        int x = 0;
        int y = 0;
    };

    PhysicalKey pressed_key(std::uint32_t& character,
                            TouchSample& touch) const;
    InputEvent semantic_event(PhysicalKey key, std::uint32_t character) const;
    bool poll_key(InputEvent& event, TouchSample& touch);
    bool poll_touchpad(InputEvent& event, TouchSample& touch);
    bool sample_touchpad(TouchSample& touch) const;
    static bool repeatable(PhysicalKey key);

    Clock& clock_;
    PhysicalKey active_key_ = PhysicalKey::None;
    InputEventType active_event_type_ = InputEventType::None;
    int active_amount_ = 0;
    std::uint64_t pressed_at_ms_ = 0;
    std::uint64_t repeated_at_ms_ = 0;
    bool touch_active_ = false;
    bool touch_tap_candidate_ = false;
    bool touch_tap_suppressed_until_release_ = false;
    TouchAxis touch_axis_ = TouchAxis::None;
    bool touch_axis_corrected_ = false;
    bool touch_discrete_emitted_ = false;
    InputEventType pending_touch_event_ = InputEventType::None;
    int pending_touch_amount_ = 0;
    int touch_origin_x_ = 0;
    int touch_origin_y_ = 0;
    int touch_x_ = 0;
    int touch_y_ = 0;
    std::uint64_t touch_started_at_ms_ = 0;
};

}  // namespace nmarkdown

#endif
