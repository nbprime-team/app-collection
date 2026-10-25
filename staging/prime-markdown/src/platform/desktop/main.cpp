#include <cstdio>
#include <string>
#include <vector>

#include "clock_desktop.h"
#include "display_desktop.h"
#include "input_desktop.h"
#include "nmarkdown/app/application.h"
#include "nmarkdown/document/utf8.h"

namespace {

void print_usage(const char* program) {
    std::printf(
        "Usage: %s [--document FILE] [--body-font FILE] [--italic-font FILE]\n"
        "          [--mono-font FILE] [--cjk-font FILE] [--browse] [--root DIR]\n"
        "          [--output FILE.ppm]\n"
        "          [--events LIST] [--no-state]\n"
        "\n"
        "LIST is comma-separated: up, down, page-up, page-down, swipe-left,\n"
        "swipe-right, swipe-up, swipe-down, left, right,\n"
        "menu, search, settings, diagnostics, open, next, bookmark, enter, back, font+/font-,\n"
        "text:UTF8, char:X, backspace, scroll:N, or pan:N. A safe exit sequence is automatic.\n",
        program);
}

bool append_events(const std::string& script,
                   std::vector<nmarkdown::InputEvent>& events) {
    std::size_t begin = 0;
    while (begin <= script.size()) {
        const std::size_t comma = script.find(',', begin);
        const std::size_t end = comma == std::string::npos ? script.size() : comma;
        const std::string token = script.substr(begin, end - begin);
        if (token.rfind("text:", 0) == 0 && token.size() > 5) {
            const std::string value = token.substr(5);
            std::size_t offset = 0;
            while (offset < value.size()) {
                const nmarkdown::DecodedCodepoint decoded = nmarkdown::utf8_next(
                    reinterpret_cast<const std::uint8_t*>(value.data()), value.size(),
                    static_cast<std::uint32_t>(offset));
                events.push_back({nmarkdown::InputEventType::TextInput,
                                  static_cast<int>(decoded.value)});
                offset += decoded.byte_length == 0 ? 1 : decoded.byte_length;
            }
            if (comma == std::string::npos) break;
            begin = comma + 1;
            continue;
        }
        nmarkdown::InputEvent event;
        if (token.empty() || !nmarkdown::parse_input_event(token, event)) {
            std::fprintf(stderr, "Unknown input event: %s\n", token.c_str());
            return false;
        }
        events.push_back(event);
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string output_path = "nmarkdown-phase0.ppm";
    const char* document_path = nullptr;
    std::vector<nmarkdown::InputEvent> events;
    bool persist_state = true;
    bool browse_on_start = false;
    std::string document_root = ".";
    std::string initial_body_font_path;
    std::string initial_body_italic_font_path;
    std::string initial_monospace_font_path;
    std::string initial_cjk_font_path;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (argument == "--document" && index + 1 < argc) {
            document_path = argv[++index];
        } else if (argument == "--body-font" && index + 1 < argc) {
            initial_body_font_path = argv[++index];
        } else if (argument == "--italic-font" && index + 1 < argc) {
            initial_body_italic_font_path = argv[++index];
        } else if (argument == "--mono-font" && index + 1 < argc) {
            initial_monospace_font_path = argv[++index];
        } else if (argument == "--cjk-font" && index + 1 < argc) {
            initial_cjk_font_path = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            output_path = argv[++index];
        } else if (argument == "--events" && index + 1 < argc) {
            if (!append_events(argv[++index], events)) {
                return 2;
            }
        } else if (argument == "--no-state") {
            persist_state = false;
        } else if (argument == "--browse") {
            browse_on_start = true;
        } else if (argument == "--root" && index + 1 < argc) {
            document_root = argv[++index];
        } else if (!argument.empty() && argument[0] != '-' && document_path == nullptr) {
            document_path = argv[index];
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argument.c_str());
            print_usage(argv[0]);
            return 2;
        }
    }

    // The first Back may close an overlay. The second then exits; if no
    // overlay is open, the loop exits on the first and never consumes the next.
    events.push_back({nmarkdown::InputEventType::Back, 0});
    events.push_back({nmarkdown::InputEventType::Back, 0});

    nmarkdown::DisplayDesktop display(output_path);
    nmarkdown::ScriptedInput input(std::move(events));
    nmarkdown::StdioFileSystem files;
    nmarkdown::ClockDesktop clock;

    nmarkdown::ReaderOptions options;
    options.maximum_source_bytes = 8U * 1024U * 1024U;
    options.persist_state = persist_state;
    options.open_browser_on_empty_path = browse_on_start;
    options.document_root = document_root;
    options.initial_body_font_path = initial_body_font_path;
    options.initial_body_italic_font_path = initial_body_italic_font_path;
    options.initial_monospace_font_path = initial_monospace_font_path;
    options.initial_cjk_font_path = initial_cjk_font_path;
    const int result = nmarkdown::run_reader(display,
                                             input,
                                             files,
                                             clock,
                                             document_path,
                                             options);
    if (!display.write_succeeded()) {
        std::fprintf(stderr, "Could not write RGB565 preview to %s\n", output_path.c_str());
        return 3;
    }

    std::printf("Wrote %d frame(s) to %s\n",
                display.present_count(),
                output_path.c_str());
    return result;
}
