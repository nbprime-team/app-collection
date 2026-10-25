#include "input_desktop.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nmarkdown {

ScriptedInput::ScriptedInput(std::vector<InputEvent> events)
    : events_(std::move(events)) {}

bool ScriptedInput::poll(InputEvent& event) {
    if (next_event_ >= events_.size()) {
        return false;
    }
    event = events_[next_event_++];
    return true;
}

bool parse_input_event(const std::string& name, InputEvent& event) {
    std::string normalized = name;
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });

    event = {};
    if (normalized == "up") {
        event.type = InputEventType::ScrollLineUp;
    } else if (normalized == "down") {
        event.type = InputEventType::ScrollLineDown;
    } else if (normalized == "page-up") {
        event.type = InputEventType::PageUp;
    } else if (normalized == "page-down") {
        event.type = InputEventType::PageDown;
    } else if (normalized == "swipe-left") {
        event.type = InputEventType::SwipeLeft;
    } else if (normalized == "swipe-right") {
        event.type = InputEventType::SwipeRight;
    } else if (normalized == "swipe-up") {
        event.type = InputEventType::SwipeUp;
    } else if (normalized == "swipe-down") {
        event.type = InputEventType::SwipeDown;
    } else if (normalized == "left") {
        event.type = InputEventType::PanLeft;
    } else if (normalized == "right") {
        event.type = InputEventType::PanRight;
    } else if (normalized == "enter") {
        event.type = InputEventType::Activate;
    } else if (normalized == "back" || normalized == "escape") {
        event.type = InputEventType::Back;
    } else if (normalized == "quit" || normalized == "ctrl-escape" ||
               normalized == "ctrl-esc") {
        event.type = InputEventType::Quit;
    } else if (normalized == "menu") {
        event.type = InputEventType::OpenMenu;
    } else if (normalized == "search") {
        event.type = InputEventType::OpenSearch;
    } else if (normalized == "settings") {
        event.type = InputEventType::OpenSettings;
    } else if (normalized == "diagnostics" || normalized == "benchmark") {
        event.type = InputEventType::OpenDiagnostics;
    } else if (normalized == "open" || normalized == "open-document") {
        event.type = InputEventType::OpenDocument;
    } else if (normalized == "next") {
        event.type = InputEventType::SearchNext;
    } else if (normalized == "bookmark") {
        event.type = InputEventType::ToggleBookmark;
    } else if (normalized == "backspace" || normalized == "delete") {
        event.type = InputEventType::Backspace;
    } else if (normalized.rfind("char:", 0) == 0 && normalized.size() == 6) {
        event.type = InputEventType::TextInput;
        event.amount = static_cast<unsigned char>(name[5]);
    } else if (normalized == "font+") {
        event.type = InputEventType::IncreaseFont;
    } else if (normalized == "font-") {
        event.type = InputEventType::DecreaseFont;
    } else if (normalized.rfind("scroll:", 0) == 0) {
        const char* amount = normalized.c_str() + 7;
        char* end = nullptr;
        const long parsed = std::strtol(amount, &end, 10);
        if (end == amount || *end != '\0') {
            return false;
        }
        event.type = InputEventType::PointerScroll;
        event.amount = static_cast<int>(parsed);
    } else if (normalized.rfind("pan:", 0) == 0) {
        const char* amount = normalized.c_str() + 4;
        char* end = nullptr;
        const long parsed = std::strtol(amount, &end, 10);
        if (end == amount || *end != '\0') {
            return false;
        }
        event.type = InputEventType::PointerPan;
        event.amount = static_cast<int>(parsed);
    } else {
        return false;
    }

    return true;
}

}  // namespace nmarkdown
