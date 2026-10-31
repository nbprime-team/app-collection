#include "input_ndless.h"

#include <algorithm>
#include <cstdlib>

#include <libndls.h>

namespace nmarkdown {
namespace {

constexpr std::uint64_t kInitialRepeatDelayMs = 360;
constexpr std::uint64_t kRepeatIntervalMs = 70;
constexpr std::uint64_t kTapMaximumDurationMs = 500;

// Firebird models the CX/CX II's rectangular 100 x 70 touch surface with
// native ranges 2328 x 1691, giving both axes nearly the same coordinate
// density. Comparing raw deltas therefore preserves physical gesture angles;
// normalizing each delta by its full range would stretch the pad into a square
// and bias direction detection toward vertical movement.
constexpr int kTouchTapSlopDivisor = 80;       // 1.25% of the short span.
constexpr int kTouchAxisLockDivisor = 25;      // 4% of the short span.
constexpr int kTouchMotionDeadzoneDivisor = 80;
constexpr int kTouchSwipeExtentDivisor = 5;    // 20% per axis.
constexpr int kTouchAxisDominanceNumerator = 4;
constexpr int kTouchAxisDominanceDenominator = 3;

struct TouchMetrics {
    int width = 0;
    int height = 0;
    int tap_slop = 2;
    int axis_lock = 4;
    int motion_deadzone = 2;
    int swipe_extent_x = 24;
    int swipe_extent_y = 24;
};

int proportional_threshold(int extent, int minimum, int divisor) {
    return extent > 0 ? std::max(minimum, extent / divisor) : minimum;
}

TouchMetrics touch_metrics(const touchpad_info_t* info) {
    TouchMetrics metrics;
    if (info != nullptr) {
        metrics.width = static_cast<int>(info->width);
        metrics.height = static_cast<int>(info->height);
    }
    const int coordinate_span = metrics.width > 0 && metrics.height > 0
                                    ? std::min(metrics.width, metrics.height)
                                    : std::max(metrics.width, metrics.height);
    metrics.tap_slop = proportional_threshold(
        coordinate_span, metrics.tap_slop, kTouchTapSlopDivisor);
    metrics.axis_lock = proportional_threshold(
        coordinate_span, metrics.axis_lock, kTouchAxisLockDivisor);
    metrics.motion_deadzone = proportional_threshold(
        coordinate_span, metrics.motion_deadzone,
        kTouchMotionDeadzoneDivisor);
    if (metrics.width > 0) {
        metrics.swipe_extent_x = proportional_threshold(
            metrics.width, 18, kTouchSwipeExtentDivisor);
    }
    if (metrics.height > 0) {
        metrics.swipe_extent_y = proportional_threshold(
            metrics.height, 18, kTouchSwipeExtentDivisor);
    }
    return metrics;
}

bool axis_dominates(int primary_displacement, int secondary_displacement) {
    const std::int64_t primary = std::abs(primary_displacement);
    const std::int64_t secondary = std::abs(secondary_displacement);
    return primary * kTouchAxisDominanceDenominator >=
           secondary * kTouchAxisDominanceNumerator;
}

}  // namespace

InputNdless::InputNdless(Clock& clock) : clock_(clock) {}

InputNdless::PhysicalKey InputNdless::pressed_key(std::uint32_t& character,
                                                  TouchSample& touch) const {
    character = 0;
    // Scan every matrix key before touching the synchronous touchpad I2C API.
    // Apart from keeping Esc/Ctrl+Esc responsive, this avoids turning a
    // touchpad failure into a total loss of keyboard input.
    if (isKeyPressed(KEY_NSPIRE_ESC)) {
        return PhysicalKey::Escape;
    }
#if defined(NMARKDOWN_FIREBIRD_KEYMAP_FIXTURE)
    // PocketJS cannot hold Ctrl while pressing F. The dedicated keymap build
    // uses Doc only to open Search so physical numeric aliases can be checked
    // through the production InputNdless and Viewer path.
    if (isKeyPressed(KEY_NSPIRE_DOC)) {
        return PhysicalKey::Search;
    }
#elif defined(NMARKDOWN_FIREBIRD_THEME_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_STATE_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
    // PocketJS cannot hold Ctrl while pressing T. Keep this Doc-key alias in
    // these dedicated regression fixtures. Production maps Doc to sections.
    if (isKeyPressed(KEY_NSPIRE_DOC)) {
        return PhysicalKey::Settings;
    }
#else
    // The TI Doc key opens the current document's section list.
    if (isKeyPressed(KEY_NSPIRE_DOC)) {
        return PhysicalKey::Menu;
    }
#endif
    // The TI Menu key opens Reader Settings. Ctrl+T remains an equivalent
    // shortcut, and Ctrl+O remains reserved for the document browser.
    if (isKeyPressed(KEY_NSPIRE_MENU)) {
        return PhysicalKey::Settings;
    }
    if (isKeyPressed(KEY_NSPIRE_ENTER)) {
        return PhysicalKey::Enter;
    }
    if (isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_F)) {
        return PhysicalKey::Search;
    }
    if (isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_T)) {
        return PhysicalKey::Settings;
    }
    if (isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_D)) {
        return PhysicalKey::Diagnostics;
    }
    if (isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_O)) {
        return PhysicalKey::OpenDocument;
    }
    if (isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_N)) {
        return PhysicalKey::SearchNext;
    }
    if (isKeyPressed(KEY_NSPIRE_CTRL) && isKeyPressed(KEY_NSPIRE_B)) {
        return PhysicalKey::Bookmark;
    }
    // The Catalog key carries the book glyph, so it opens the bookmark
    // management list. Ctrl+B remains the toggle shortcut.
    if (isKeyPressed(KEY_NSPIRE_CAT)) {
        return PhysicalKey::Catalog;
    }
    if (isKeyPressed(KEY_NSPIRE_SCRATCHPAD)) {
        return PhysicalKey::Scratchpad;
    }
    if (isKeyPressed(KEY_NSPIRE_DEL)) {
        return PhysicalKey::Delete;
    }
    // Ctrl+Plus/Minus is owned by the handheld OS and adjusts the LCD
    // backlight.  Matrix reads are non-consuming, so leave the chord out of
    // nMarkdown's semantic stream; the system brightness task can observe it
    // while plain Plus/Minus remain reader font-size shortcuts.
    if (isKeyPressed(KEY_NSPIRE_CTRL) &&
        (isKeyPressed(KEY_NSPIRE_PLUS) ||
         isKeyPressed(KEY_NSPIRE_MINUS))) {
        return PhysicalKey::None;
    }
    if (isKeyPressed(KEY_NSPIRE_PLUS)) {
        return PhysicalKey::Plus;
    }
    if (isKeyPressed(KEY_NSPIRE_MINUS)) {
        return PhysicalKey::Minus;
    }
    // Calculator-friendly reading aliases. Keep these ahead of the generic
    // digit-to-text scan. Their semantic event retains the originating digit
    // so the Search text field can accept it without weakening the aliases in
    // the document reader or other overlays.
    if (isKeyPressed(KEY_NSPIRE_TAB)) {
        return PhysicalKey::Tab;
    }
    if (isKeyPressed(KEY_NSPIRE_1)) {
        return PhysicalKey::Number1;
    }
    if (isKeyPressed(KEY_NSPIRE_7)) {
        return PhysicalKey::Number7;
    }
    if (isKeyPressed(KEY_NSPIRE_2)) {
        return PhysicalKey::Number2;
    }
    if (isKeyPressed(KEY_NSPIRE_4)) {
        return PhysicalKey::Number4;
    }
    if (isKeyPressed(KEY_NSPIRE_6)) {
        return PhysicalKey::Number6;
    }
    if (isKeyPressed(KEY_NSPIRE_8)) {
        return PhysicalKey::Number8;
    }
    if (!is_touchpad) {
        if (isKeyPressed(KEY_NSPIRE_UP)) {
            return PhysicalKey::Up;
        }
        if (isKeyPressed(KEY_NSPIRE_DOWN)) {
            return PhysicalKey::Down;
        }
        if (isKeyPressed(KEY_NSPIRE_LEFT)) {
            return PhysicalKey::Left;
        }
        if (isKeyPressed(KEY_NSPIRE_RIGHT)) {
            return PhysicalKey::Right;
        }
    }
    static const t_key letter_keys[] = {
        KEY_NSPIRE_A, KEY_NSPIRE_B, KEY_NSPIRE_C, KEY_NSPIRE_D,
        KEY_NSPIRE_E, KEY_NSPIRE_F, KEY_NSPIRE_G, KEY_NSPIRE_H,
        KEY_NSPIRE_I, KEY_NSPIRE_J, KEY_NSPIRE_K, KEY_NSPIRE_L,
        KEY_NSPIRE_M, KEY_NSPIRE_N, KEY_NSPIRE_O, KEY_NSPIRE_P,
        KEY_NSPIRE_Q, KEY_NSPIRE_R, KEY_NSPIRE_S, KEY_NSPIRE_T,
        KEY_NSPIRE_U, KEY_NSPIRE_V, KEY_NSPIRE_W, KEY_NSPIRE_X,
        KEY_NSPIRE_Y, KEY_NSPIRE_Z,
    };
    for (std::size_t index = 0; index < sizeof(letter_keys) / sizeof(letter_keys[0]);
         ++index) {
        if (isKeyPressed(letter_keys[index])) {
            const bool uppercase = isKeyPressed(KEY_NSPIRE_SHIFT);
            character = static_cast<std::uint32_t>((uppercase ? 'A' : 'a') + index);
            return PhysicalKey::Character;
        }
    }
    static const t_key digit_keys[] = {
        KEY_NSPIRE_0, KEY_NSPIRE_1, KEY_NSPIRE_2, KEY_NSPIRE_3, KEY_NSPIRE_4,
        KEY_NSPIRE_5, KEY_NSPIRE_6, KEY_NSPIRE_7, KEY_NSPIRE_8, KEY_NSPIRE_9,
    };
    for (std::size_t index = 0; index < sizeof(digit_keys) / sizeof(digit_keys[0]);
         ++index) {
        if (isKeyPressed(digit_keys[index])) {
            character = static_cast<std::uint32_t>('0' + index);
            return PhysicalKey::Character;
        }
    }
    if (isKeyPressed(KEY_NSPIRE_SPACE)) {
        character = ' ';
        return PhysicalKey::Character;
    }
    if (isKeyPressed(KEY_NSPIRE_PERIOD)) {
        character = '.';
        return PhysicalKey::Character;
    }

    // KEY_NSPIRE_CLICK and the four KEY_NSPIRE_* arrow constants each invoke
    // touchpad_scan() internally on CX/CX II. Calling all five independently,
    // then scanning once more for gestures, previously caused as many as six
    // blocking I2C transactions per idle poll. Take one report and reuse it.
    if (sample_touchpad(touch)) {
        switch (touch.key) {
        case TouchKey::Click: return PhysicalKey::TouchClick;
        case TouchKey::Up: return PhysicalKey::Up;
        case TouchKey::Down: return PhysicalKey::Down;
        case TouchKey::Left: return PhysicalKey::Left;
        case TouchKey::Right: return PhysicalKey::Right;
        case TouchKey::None: break;
        }
    }
    return PhysicalKey::None;
}

InputEvent InputNdless::semantic_event(PhysicalKey key,
                                       std::uint32_t character) const {
    const bool page_modifier = isKeyPressed(KEY_NSPIRE_SHIFT) ||
                               isKeyPressed(KEY_NSPIRE_CTRL);
    switch (key) {
    case PhysicalKey::Escape:
        return {isKeyPressed(KEY_NSPIRE_CTRL) ? InputEventType::Quit
                                              : InputEventType::Back,
                0};
    case PhysicalKey::Scratchpad:
        return {InputEventType::OpenDocument, 0};
    case PhysicalKey::Menu:
        return {InputEventType::OpenMenu, 0};
    case PhysicalKey::Enter:
        return {InputEventType::Activate, 0};
    case PhysicalKey::TouchClick:
        return {InputEventType::Activate, 0,
                InputEventOrigin::TouchpadActivation};
    case PhysicalKey::Search:
        return {InputEventType::OpenSearch, 0};
    case PhysicalKey::Settings:
        return {InputEventType::OpenSettings, 0};
    case PhysicalKey::Diagnostics:
        return {InputEventType::OpenDiagnostics, 0};
    case PhysicalKey::OpenDocument:
        return {InputEventType::OpenDocument, 0};
    case PhysicalKey::SearchNext:
        return {InputEventType::SearchNext, 0};
    case PhysicalKey::Bookmark:
        return {InputEventType::ToggleBookmark, 0};
    case PhysicalKey::Catalog:
        return {InputEventType::OpenBookmarks, 0};
    case PhysicalKey::Character:
        return {InputEventType::TextInput, static_cast<int>(character)};
    case PhysicalKey::Delete:
        return {InputEventType::Backspace, 0};
    case PhysicalKey::Plus:
        return {InputEventType::IncreaseFont, 0};
    case PhysicalKey::Minus:
        return {InputEventType::DecreaseFont, 0};
    case PhysicalKey::Tab:
        return {InputEventType::PageDown, 0};
    case PhysicalKey::Number1:
        return {InputEventType::PageDown, '1',
                InputEventOrigin::NumericNavigationAlias};
    case PhysicalKey::Number7:
        return {InputEventType::PageUp, '7',
                InputEventOrigin::NumericNavigationAlias};
    case PhysicalKey::Number2:
        return {page_modifier ? InputEventType::PageDown
                              : InputEventType::ScrollLineDown,
                '2', InputEventOrigin::NumericNavigationAlias};
    case PhysicalKey::Number4:
        return {InputEventType::PanLeft, '4',
                InputEventOrigin::NumericNavigationAlias};
    case PhysicalKey::Number6:
        return {InputEventType::PanRight, '6',
                InputEventOrigin::NumericNavigationAlias};
    case PhysicalKey::Number8:
        return {page_modifier ? InputEventType::PageUp
                              : InputEventType::ScrollLineUp,
                '8', InputEventOrigin::NumericNavigationAlias};
    case PhysicalKey::Up:
        return {page_modifier ? InputEventType::PageUp : InputEventType::ScrollLineUp, 0};
    case PhysicalKey::Down:
        return {page_modifier ? InputEventType::PageDown : InputEventType::ScrollLineDown, 0};
    case PhysicalKey::Left:
        return {InputEventType::PanLeft, 0};
    case PhysicalKey::Right:
        return {InputEventType::PanRight, 0};
    case PhysicalKey::None:
        return {};
    }
    return {};
}

bool InputNdless::repeatable(PhysicalKey key) {
    return key == PhysicalKey::Tab || key == PhysicalKey::Number1 ||
           key == PhysicalKey::Number2 || key == PhysicalKey::Number4 ||
           key == PhysicalKey::Number6 || key == PhysicalKey::Number7 ||
           key == PhysicalKey::Number8 || key == PhysicalKey::Up ||
           key == PhysicalKey::Down ||
           key == PhysicalKey::Left || key == PhysicalKey::Right ||
           key == PhysicalKey::Delete;
}

bool InputNdless::poll_key(InputEvent& event, TouchSample& touch) {
    std::uint32_t character = 0;
    const PhysicalKey key = pressed_key(character, touch);
    if (key == PhysicalKey::TouchClick) {
        // Releasing center-button pressure can leave the same finger in
        // contact for another poll. Do not reinterpret that tail as a second
        // tap-to-click.
        touch_tap_suppressed_until_release_ = true;
    }
    const std::uint64_t now = clock_.milliseconds();
    if (key == PhysicalKey::None) {
        active_key_ = PhysicalKey::None;
        active_event_type_ = InputEventType::None;
        active_amount_ = 0;
        return false;
    }

    const InputEvent mapped = semantic_event(key, character);
    if (key != active_key_ || mapped.type != active_event_type_ ||
        mapped.amount != active_amount_) {
        active_key_ = key;
        active_event_type_ = mapped.type;
        active_amount_ = mapped.amount;
        pressed_at_ms_ = now;
        repeated_at_ms_ = now;
        event = mapped;
        return true;
    }

    if (!repeatable(key) || now - pressed_at_ms_ < kInitialRepeatDelayMs ||
        now - repeated_at_ms_ < kRepeatIntervalMs) {
        return false;
    }

    repeated_at_ms_ = now;
    event = mapped;
    return true;
}

bool InputNdless::sample_touchpad(TouchSample& touch) const {
    if (touch.scanned) return touch.valid;
    touch.scanned = true;
    if (!is_touchpad) return false;

    touchpad_report_t report{};
    if (touchpad_scan(&report) != 0) return false;

    touch.valid = true;
    touch.contact = report.contact;
    touch.x = report.x;
    touch.y = report.y;
    if (report.pressed) {
        switch (report.arrow) {
        case TPAD_ARROW_CLICK: touch.key = TouchKey::Click; break;
        case TPAD_ARROW_UP: touch.key = TouchKey::Up; break;
        case TPAD_ARROW_DOWN: touch.key = TouchKey::Down; break;
        case TPAD_ARROW_LEFT: touch.key = TouchKey::Left; break;
        case TPAD_ARROW_RIGHT: touch.key = TouchKey::Right; break;
        default: break;
        }
    }
    return true;
}

bool InputNdless::poll_touchpad(InputEvent& event, TouchSample& touch) {
    if (!sample_touchpad(touch)) {
        touch_active_ = false;
        touch_tap_candidate_ = false;
        touch_tap_suppressed_until_release_ = false;
        touch_axis_ = TouchAxis::None;
        touch_axis_corrected_ = false;
        touch_discrete_emitted_ = false;
        pending_touch_event_ = InputEventType::None;
        pending_touch_amount_ = 0;
        return false;
    }

    if (pending_touch_event_ != InputEventType::None) {
        event = {pending_touch_event_, pending_touch_amount_};
        pending_touch_event_ = InputEventType::None;
        pending_touch_amount_ = 0;
        if (!touch.contact) {
            touch_active_ = false;
            touch_tap_candidate_ = false;
            touch_tap_suppressed_until_release_ = false;
            touch_axis_ = TouchAxis::None;
            touch_axis_corrected_ = false;
            touch_discrete_emitted_ = false;
        }
        return true;
    }

    if (!touch.contact) {
        const bool emit_tap =
            touch_active_ && touch_tap_candidate_ &&
            touch_axis_ == TouchAxis::None &&
            clock_.milliseconds() - touch_started_at_ms_ <=
                kTapMaximumDurationMs;
        touch_active_ = false;
        touch_tap_candidate_ = false;
        touch_tap_suppressed_until_release_ = false;
        touch_axis_ = TouchAxis::None;
        touch_axis_corrected_ = false;
        touch_discrete_emitted_ = false;
        if (emit_tap) {
            event = {InputEventType::Activate, 0,
                     InputEventOrigin::TouchpadTap};
            return true;
        }
        return false;
    }

    if (!touch_active_) {
        touch_active_ = true;
        touch_tap_candidate_ = !touch_tap_suppressed_until_release_;
        touch_axis_ = TouchAxis::None;
        touch_axis_corrected_ = false;
        touch_discrete_emitted_ = false;
        pending_touch_event_ = InputEventType::None;
        pending_touch_amount_ = 0;
        touch_origin_x_ = touch.x;
        touch_origin_y_ = touch.y;
        touch_x_ = touch.x;
        touch_y_ = touch.y;
        touch_started_at_ms_ = clock_.milliseconds();
        return false;
    }

    // A drag produces a continuous axis delta and, at most once per contact,
    // a named swipe. Each touchpad mode selects the behavior it owns without
    // making the hardware sampler aware of Viewer state: Vertical Scroll uses
    // vertical deltas/horizontal swipes, while Horizontal Scroll uses vertical
    // swipes/horizontal deltas.

    const touchpad_info_t* info = touchpad_getinfo();
    const TouchMetrics metrics = touch_metrics(info);
    const int horizontal_from_origin = touch.x - touch_origin_x_;
    // Ndless exposes the touchpad's native coordinate system: X increases to
    // the right, but Y increases toward the physical top of the pad.  Convert
    // only Y to the screen-style convention used by InputEvent (down is
    // positive).  Firebird models the same hardware convention by inverting
    // its GUI-space Y in touchpad_set_state(), so this normalization is shared
    // by the emulator and a real CX/CX II.
    const int vertical_from_origin = touch_origin_y_ - touch.y;
    // Tap slop is deliberately smaller than the drag-start deadzone. Motion
    // in between emits nothing, but it also cannot become an accidental tap
    // when the finger is released.
    if (std::abs(horizontal_from_origin) >= metrics.tap_slop ||
        std::abs(vertical_from_origin) >= metrics.tap_slop) {
        touch_tap_candidate_ = false;
    }

    if (touch_axis_ == TouchAxis::None) {
        // Lock only after intentional travel and require 4:3 dominance in
        // physical-scale native units. Ambiguous diagonal motion stays in the
        // deadzone until one axis becomes clear.
        if (std::abs(horizontal_from_origin) >= metrics.axis_lock &&
            axis_dominates(horizontal_from_origin, vertical_from_origin)) {
            touch_axis_ = TouchAxis::Horizontal;
            touch_tap_candidate_ = false;
        } else if (std::abs(vertical_from_origin) >= metrics.axis_lock &&
                   axis_dominates(vertical_from_origin,
                                  horizontal_from_origin)) {
            touch_axis_ = TouchAxis::Vertical;
            touch_tap_candidate_ = false;
        } else {
            return false;
        }
    }

    // The first few samples often contain landing wobble, particularly when a
    // thumb is beginning a left/right drag. Before a named swipe commits the
    // contact, allow one correction if the other axis later becomes clearly
    // dominant. Without this, a short vertical wobble permanently swallowed
    // an otherwise unambiguous horizontal swipe or continuous scroll.
    if (!touch_axis_corrected_ && !touch_discrete_emitted_) {
        if (touch_axis_ == TouchAxis::Vertical &&
            std::abs(horizontal_from_origin) >= metrics.axis_lock &&
            axis_dominates(horizontal_from_origin, vertical_from_origin)) {
            touch_axis_ = TouchAxis::Horizontal;
            touch_axis_corrected_ = true;
        } else if (touch_axis_ == TouchAxis::Horizontal &&
                   std::abs(vertical_from_origin) >= metrics.axis_lock &&
                   axis_dominates(vertical_from_origin,
                                  horizontal_from_origin)) {
            touch_axis_ = TouchAxis::Vertical;
            touch_axis_corrected_ = true;
        }
    }

    const bool horizontal_axis = touch_axis_ == TouchAxis::Horizontal;
    const int displacement = horizontal_axis ? horizontal_from_origin
                                             : vertical_from_origin;
    const int swipe_extent = horizontal_axis ? metrics.swipe_extent_x
                                             : metrics.swipe_extent_y;
    InputEventType discrete_event = InputEventType::None;
    if (!touch_discrete_emitted_ && std::abs(displacement) >= swipe_extent) {
        discrete_event = horizontal_axis
                             ? (displacement < 0 ? InputEventType::SwipeLeft
                                                 : InputEventType::SwipeRight)
                             : (displacement < 0 ? InputEventType::SwipeUp
                                                 : InputEventType::SwipeDown);
        touch_discrete_emitted_ = true;
    }

    const int delta = horizontal_axis ? touch.x - touch_x_ : touch_y_ - touch.y;
    // Keep sub-threshold samples accumulated against the last emitted point,
    // filtering jitter without throwing away deliberate slow movement.
    const int threshold = metrics.motion_deadzone;
    if (std::abs(delta) < threshold) {
        if (discrete_event != InputEventType::None) {
            event = {discrete_event, 0};
            return true;
        }
        return false;
    }

    if (horizontal_axis) touch_x_ = touch.x;
    else touch_y_ = touch.y;
    const int axis_extent = horizontal_axis ? metrics.width : metrics.height;
    const int scaled = axis_extent == 0 ? delta : delta * 240 / axis_extent;
    const InputEventType continuous_event = horizontal_axis
                                                ? InputEventType::PointerPan
                                                : InputEventType::PointerScroll;
    const int continuous_amount = scaled == 0 ? (delta > 0 ? 1 : -1) : scaled;
    if (discrete_event != InputEventType::None) {
        pending_touch_event_ = continuous_event;
        pending_touch_amount_ = continuous_amount;
        event = {discrete_event, 0};
    } else {
        event = {continuous_event, continuous_amount};
    }
    return true;
}

bool InputNdless::poll(InputEvent& event) {
    TouchSample touch;
    if (poll_key(event, touch)) {
        return true;
    }
    return active_key_ == PhysicalKey::None && poll_touchpad(event, touch);
}

bool InputNdless::interaction_active() const {
    return active_key_ != PhysicalKey::None || touch_active_;
}

}  // namespace nmarkdown
