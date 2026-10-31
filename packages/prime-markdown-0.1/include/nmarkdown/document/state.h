#ifndef NMARKDOWN_DOCUMENT_STATE_H
#define NMARKDOWN_DOCUMENT_STATE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "nmarkdown/document/document_ir.h"
#include "nmarkdown/text/render_style.h"

namespace nmarkdown {

enum class ReadingMode : std::uint8_t {
    VerticalScroll = 0,
    HorizontalScroll = 1,
    // Source-compatible names for v3/v4 callers and state fixtures. The
    // serialized values are unchanged; only their user-facing meaning has
    // been clarified from pseudo-pagination to touchpad layout.
    Scroll = VerticalScroll,
    PageSwipe = HorizontalScroll,
};

struct SavedPosition {
    std::uint64_t document_identity = 0;
    std::uint32_t source_offset = 0;
    NodeId nearest_block = kInvalidNode;
    std::uint16_t relative_position_0_65535 = 0;
};

struct ReaderState {
    SavedPosition position;
    std::vector<std::uint32_t> bookmarks;
    std::uint32_t last_selected_heading = 0;
    std::uint8_t font_size = 15;
    // Negative selects automatic content-aware spacing; 0-10 is a manual
    // gap in pixels. The serialized byte keeps 0 = automatic for older
    // files and stores the manual zero gap as 255.
    int line_gap = -1;
    std::uint8_t side_margin = 5;
    bool dark_theme = false;
    bool high_contrast = false;
    bool code_wrap = true;
    std::uint8_t table_mode = 0;
    ReadingMode reading_mode = ReadingMode::VerticalScroll;
    // Continuous scrolling follows direct manipulation: Natural makes an
    // up/left drag advance, while Reversed makes down/right advance.
    bool natural_scrolling = true;
    // Discrete swipes follow reading order: Natural makes a right/down swipe
    // advance, while Reversed makes left/up advance.
    bool natural_swiping = true;
    RenderSharpness render_sharpness = kDefaultRenderSharpness;
};

std::uint64_t document_identity(const std::uint8_t* data, std::size_t size);
std::uint64_t document_identity(const std::deque<std::string>& segments,
                                std::size_t total_size);
bool encode_reader_state(const ReaderState& state,
                         std::vector<std::uint8_t>& bytes,
                         std::string& error);
bool decode_reader_state(const std::uint8_t* bytes,
                         std::size_t size,
                         ReaderState& state,
                         std::string& error);

}  // namespace nmarkdown

#endif
