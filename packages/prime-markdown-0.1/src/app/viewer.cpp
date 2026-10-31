#include "nmarkdown/app/viewer.h"

#include <algorithm>
#include <cctype>
#if defined(NMARKDOWN_FIREBIRD_PAGE_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_SCROLL_SWIPE_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_PROGRESS_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_THEME_FIXTURE)
#include <cstdio>
#endif
#include <cstring>
#include <new>
#include <string>

#include "nmarkdown/document/unicode.h"
#include "nmarkdown/document/utf8.h"
#include "nmarkdown/platform/allocation_stats.h"
#include "nmarkdown/render/primitives.h"
#include "nmarkdown/text/compositor.h"

namespace nmarkdown {
namespace {

constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr int kHeaderHeight = 18;
constexpr int kBottomContentInset = 2;
constexpr int kProgressHeight = 2;
// The document owns the screen; the filename bar is transient chrome that
// appears on upward navigation and hides again while reading forward, so
// layout geometry never changes with its visibility. The permanent reading
// progress strip keeps a two-pixel margin to the first text line.
constexpr int kTopContentInset = kProgressHeight + 2;
constexpr int kViewportHeight =
    kScreenHeight - kTopContentInset - kBottomContentInset;
constexpr std::size_t kMaximumScreenStepHistory = 32;
constexpr int kChromeX = 8;
constexpr int kChromeBaseline = 15;
constexpr int kChromeRightPadding = 6;
constexpr int kLineScroll = 18;
constexpr int kPageX = 0;
constexpr int kPageWidth = kScreenWidth;
// Painting must synchronously measure only what can appear on this frame.
// Three following viewports are warmed one block at a time while input is
// idle; doing that work here was the dominant large-document page-turn cost.
constexpr unsigned kSynchronousLayoutOverscan = 0;
constexpr int kIdlePreloadViewports = 3;
constexpr int kMostlyVisiblePercent = 85;
constexpr std::size_t kSettingsRowCount = 13;

// Single-step list navigation wraps at both ends: stepping past the last row
// returns to the first and vice versa. Page-sized jumps keep clamping so a
// five-row leap can never teleport across the list boundary. Empty and
// single-row lists are no-ops.
std::size_t wrap_previous_row(std::size_t selected,
                              std::size_t row_count,
                              std::size_t minimum_row = 0) {
    if (row_count <= minimum_row) return selected;
    return selected > minimum_row ? selected - 1U : row_count - 1U;
}

std::size_t wrap_next_row(std::size_t selected,
                          std::size_t row_count,
                          std::size_t minimum_row = 0) {
    if (row_count <= minimum_row) return selected;
    return selected + 1U < row_count ? selected + 1U : minimum_row;
}

// Small chevrons at the panel's right edge showing that a scrollable list
// continues above or below the visible rows: light on the dark header
// strip, accent on the panel body near its bottom edge.
void draw_list_overflow_hints(const Surface565& surface,
                              const Rect& panel,
                              std::uint16_t color,
                              int first_row_top,
                              bool more_above,
                              bool more_below) {
    const int center_x = panel.x + panel.width - 12;
    if (more_above) {
        // The up hint sits on the first visible option row, not the title.
        for (int row = 0; row < 4; ++row) {
            fill_rect(surface,
                      {center_x - row, first_row_top + 6 + row,
                       row * 2 + 1, 1},
                      color, panel);
        }
    }
    if (more_below) {
        for (int row = 0; row < 4; ++row) {
            fill_rect(surface,
                      {center_x - (3 - row),
                       panel.y + panel.height - 12 + row,
                       (3 - row) * 2 + 1, 1},
                      color, panel);
        }
    }
}

// Greedy word wrap for short UI strings: at most maximum_lines shaped runs
// fitting maximum_width_px, breaking at spaces when possible and at UTF-8
// codepoints otherwise (so unspaced scripts still wrap), with the final line
// ellipsized when text remains.
std::vector<GlyphRun> wrap_text_runs(TextSystem& text,
                                     std::string_view body,
                                     Fx pixel_size,
                                     int maximum_width_px,
                                     std::size_t maximum_lines) {
    std::vector<GlyphRun> lines;
    std::string_view remaining = body;
    while (!remaining.empty() && lines.size() < maximum_lines) {
        const bool final_line = lines.size() + 1U == maximum_lines;
        std::size_t take = remaining.size();
        GlyphRun run;
        for (;;) {
            std::string candidate(remaining.substr(0, take));
            if (final_line && take < remaining.size()) candidate += u8"…";
            run = {};
            const bool shaped = text.shape(
                candidate.data(), candidate.size(), pixel_size, run);
            if (!shaped || fx_ceil(run.width) <= maximum_width_px ||
                take <= 1) {
                break;
            }
            if (!final_line && take >= 2) {
                const std::size_t space = remaining.rfind(' ', take - 2);
                if (space != std::string_view::npos && space > 0) {
                    take = space;
                    continue;
                }
            }
            --take;
            while (take > 1 &&
                   (static_cast<unsigned char>(remaining[take]) & 0xC0U) ==
                       0x80U) {
                --take;
            }
        }
        lines.push_back(std::move(run));
        if (final_line) break;
        std::size_t advance = take;
        while (advance < remaining.size() && remaining[advance] == ' ') {
            ++advance;
        }
        remaining.remove_prefix(advance);
    }
    return lines;
}
constexpr std::size_t kSettingsVisibleRows = 9;

// Modal typography is intentionally independent from the document font-size
// setting. These sizes are the smallest comfortable hierarchy at 320x240;
// every cached run is shaped and drawn at the same size.
constexpr int kMenuTitlePixelSize = 13;
constexpr int kMenuListPixelSize = 13;
constexpr int kMenuCompactPixelSize = 12;
constexpr int kMenuAuxiliaryPixelSize = 11;
constexpr int kMenuSearchInputPixelSize = 14;

constexpr int kMenuPanelHeaderHeight = 24;
constexpr std::size_t kMenuListVisibleRows = 6;
constexpr int kMenuListFirstRowY = 29;
constexpr int kMenuListRowStride = 26;
constexpr int kMenuListRowHeight = 23;
constexpr int kMenuListBaseline = 17;
constexpr int kSettingsFirstRowY = 25;
constexpr int kSettingsRowStride = 20;
constexpr int kSettingsRowHeight = 18;
constexpr int kSettingsBaseline = 14;
constexpr std::size_t kSearchVisibleRows = 4;
constexpr int kSearchFirstRowY = 79;
constexpr int kSearchRowStride = 27;
constexpr int kSearchRowHeight = 24;

static_assert(kMenuListFirstRowY +
                  static_cast<int>(kMenuListVisibleRows - 1) *
                      kMenuListRowStride +
                  kMenuListRowHeight <=
              192,
              "six large menu rows must fit the standard panel");
static_assert(kSettingsFirstRowY +
                  static_cast<int>(kSettingsVisibleRows - 1) *
                      kSettingsRowStride +
                  kSettingsRowHeight <=
              204,
              "nine settings rows must fit the 320x240 panel");
static_assert(kSearchFirstRowY +
                  static_cast<int>(kSearchVisibleRows - 1) *
                      kSearchRowStride +
                  kSearchRowHeight <=
              196,
              "four search results must fit the search panel");

bool is_numeric_navigation_alias(const InputEvent& event) {
    if (event.origin != InputEventOrigin::NumericNavigationAlias) return false;
    switch (event.amount) {
    case '1': return event.type == InputEventType::PageDown;
    case '2':
        return event.type == InputEventType::ScrollLineDown ||
               event.type == InputEventType::PageDown;
    case '4': return event.type == InputEventType::PanLeft;
    case '6': return event.type == InputEventType::PanRight;
    case '7': return event.type == InputEventType::PageUp;
    case '8':
        return event.type == InputEventType::ScrollLineUp ||
               event.type == InputEventType::PageUp;
    default: return false;
    }
}

struct ThemeColors {
    std::uint16_t canvas;
    std::uint16_t paper;
    std::uint16_t ink;
    std::uint16_t muted_ink;
    std::uint16_t header;
    std::uint16_t accent;
    std::uint16_t success;
    std::uint16_t failure;
    std::uint16_t code;
    std::uint16_t border;
    std::uint16_t overlay;
};

ThemeColors theme_colors(bool dark, bool high_contrast = false) {
    if (high_contrast && dark) {
        return {rgb565(0, 0, 0), rgb565(0, 0, 0), rgb565(255, 255, 255),
                rgb565(210, 210, 210), rgb565(0, 0, 0), rgb565(0, 238, 255),
                rgb565(80, 255, 120), rgb565(255, 90, 90), rgb565(12, 12, 12),
                rgb565(235, 235, 235), rgb565(0, 0, 0)};
    }
    if (high_contrast) {
        return {rgb565(255, 255, 255), rgb565(255, 255, 255), rgb565(0, 0, 0),
                rgb565(45, 45, 45), rgb565(0, 0, 0), rgb565(0, 74, 210),
                rgb565(0, 125, 45), rgb565(195, 0, 20), rgb565(245, 245, 245),
                rgb565(0, 0, 0), rgb565(255, 255, 255)};
    }
    if (dark) {
        return {
            rgb565(18, 22, 29),
            rgb565(29, 35, 45),
            rgb565(235, 239, 245),
            rgb565(166, 176, 190),
            rgb565(12, 27, 48),
            rgb565(76, 166, 231),
            rgb565(68, 190, 126),
            rgb565(230, 91, 99),
            rgb565(38, 46, 58),
            rgb565(75, 86, 102),
            rgb565(34, 41, 52),
        };
    }
    return {
        rgb565(232, 235, 241),
        rgb565(255, 255, 252),
        rgb565(45, 52, 64),
        rgb565(120, 130, 144),
        rgb565(25, 45, 74),
        rgb565(61, 144, 214),
        rgb565(59, 171, 111),
        rgb565(213, 76, 84),
        rgb565(239, 241, 244),
        rgb565(191, 197, 207),
        rgb565(248, 249, 251),
    };
}

// GitHub emits a native <input type="checkbox"> for task-list items. Its CSS
// only adjusts vertical alignment and margins; the browser supplies the native
// pixels. These are the supplied 2x reference controls reduced to their native
// 1x sizes (13x13 checked, 12x12 unchecked) and quantized once to RGB565.
// 0x0001 is an out-of-gamut sentinel used only for transparent checked pixels.
constexpr std::uint16_t kGitHubCheckboxTransparent = 0x0001U;
constexpr int kGitHubCheckedCheckboxSize = 13;
constexpr int kGitHubUncheckedCheckboxSize = 12;
constexpr std::uint16_t kGitHubCheckedCheckbox[] = {
    0x0001, 0xf7dd, 0xc6f6, 0xb6d4, 0xb6d4, 0xb6d4, 0xb6d4, 0xb6d4, 0xb6d4, 0xb6d4, 0xc6f6, 0xf7dd, 0x0001,
    0xf7dd, 0x85ec, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x85ec, 0xf7dd,
    0xbef5, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x75a9, 0x8e0d, 0x75ca, 0x75a9, 0xbef5,
    0xb6d3, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x9e4f, 0xffff, 0xb6d4, 0x6da9, 0xb6d3,
    0xb6b3, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x6da9, 0x75ca, 0xefbd, 0xffff, 0x860d, 0x6da9, 0xb6b3,
    0xb6b3, 0x6d88, 0x6d88, 0x6d88, 0x6d88, 0x6d88, 0x6d88, 0xbef5, 0xffff, 0xb6d4, 0x6d88, 0x6d88, 0xb6b3,
    0xaeb3, 0x6588, 0x964f, 0xffff, 0xb6d4, 0x6d88, 0xa671, 0xffff, 0xcf37, 0x6d88, 0x6588, 0x6588, 0xaeb3,
    0xaeb3, 0x6568, 0x6588, 0xd759, 0xffff, 0xcf38, 0xffff, 0xe79b, 0x6588, 0x6568, 0x6568, 0x6568, 0xaeb3,
    0xaeb3, 0x6567, 0x6567, 0x6d88, 0xe79c, 0xffff, 0xffff, 0x7deb, 0x6567, 0x6567, 0x6567, 0x6567, 0xaeb3,
    0xaeb3, 0x6567, 0x6567, 0x6567, 0x75aa, 0xdf7a, 0xa671, 0x6567, 0x6567, 0x6567, 0x6567, 0x6567, 0xaeb3,
    0xcf38, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0x5d67, 0xcf38,
    0x0001, 0xae92, 0x6568, 0x5d47, 0x5d47, 0x5d47, 0x5d47, 0x5d47, 0x5d47, 0x5d47, 0x6568, 0xae92, 0x0001,
    0x0001, 0x0001, 0xf7de, 0xefbd, 0xefbd, 0xefbd, 0xefbd, 0xefbd, 0xefbd, 0xefbd, 0xf7de, 0x0001, 0x0001,
};
constexpr std::uint8_t kGitHubUncheckedCoverage[] = {
      9, 45, 50, 48, 48, 48, 48, 48, 48, 50, 45,  9,
     42, 23, 16, 14, 13, 13, 13, 13, 14, 16, 23, 42,
     37,  7,  2,  1,  1,  1,  1,  1,  1,  2,  7, 37,
     29,  3,  0,  0,  0,  0,  0,  0,  0,  0,  3, 29,
     28,  3,  0,  0,  0,  0,  0,  0,  0,  0,  3, 28,
     27,  3,  0,  0,  0,  0,  0,  0,  0,  0,  3, 27,
     27,  3,  0,  0,  0,  0,  0,  0,  0,  0,  3, 27,
     27,  3,  0,  0,  0,  0,  0,  0,  0,  0,  3, 27,
     28,  3,  0,  0,  0,  0,  0,  0,  0,  0,  3, 28,
     29,  4,  1,  0,  0,  0,  0,  0,  0,  1,  4, 29,
     29,  9,  4,  3,  3,  3,  3,  3,  3,  4,  9, 29,
      6, 28, 29, 27, 27, 27, 27, 27, 27, 29, 28,  6,
};
static_assert(
    sizeof(kGitHubCheckedCheckbox) / sizeof(kGitHubCheckedCheckbox[0]) ==
        kGitHubCheckedCheckboxSize * kGitHubCheckedCheckboxSize);
static_assert(
    sizeof(kGitHubUncheckedCoverage) / sizeof(kGitHubUncheckedCoverage[0]) ==
        kGitHubUncheckedCheckboxSize * kGitHubUncheckedCheckboxSize);

void draw_github_task_checkbox(const Surface565& surface,
                               int x,
                               int y,
                               bool checked,
                               std::uint16_t unchecked_tint,
                               Rect clip) {
    const Rect bounds = intersect(clip, surface.bounds());
    const int size = checked ? kGitHubCheckedCheckboxSize
                             : kGitHubUncheckedCheckboxSize;
    // Place the visual control one pixel below the run's ascent box so it
    // follows the text baseline instead of sitting above adjacent glyphs.
    ++y;
    for (int row = 0; row < size; ++row) {
        const int destination_y = y + row;
        if (destination_y < bounds.y ||
            destination_y >= bounds.y + bounds.height) {
            continue;
        }
        for (int column = 0; column < size; ++column) {
            const int destination_x = x + column;
            if (destination_x < bounds.x ||
                destination_x >= bounds.x + bounds.width) {
                continue;
            }
            const std::size_t offset =
                static_cast<std::size_t>(row * size + column);
            if (checked) {
                // The source screenshot was composited over white. Its outer
                // downsampled pixels therefore contain a white matte, which
                // becomes a visible frame over menu selections and dark pages.
                // Keep GitHub's native gradient and tick pixels, but discard
                // only that precomposited fringe.
                const bool outside_green =
                    row == 0 || row == kGitHubCheckedCheckboxSize - 1 ||
                    column == 0 ||
                    column == kGitHubCheckedCheckboxSize - 1 ||
                    ((row == 1 ||
                      row == kGitHubCheckedCheckboxSize - 2) &&
                     (column == 1 ||
                      column == kGitHubCheckedCheckboxSize - 2));
                if (outside_green) continue;
                const std::uint16_t pixel = kGitHubCheckedCheckbox[offset];
                if (pixel != kGitHubCheckboxTransparent) {
                    surface.put_pixel(destination_x, destination_y, pixel);
                }
            } else {
                const std::uint8_t coverage = kGitHubUncheckedCoverage[offset];
                if (coverage != 0) {
                    surface.put_pixel(
                        destination_x, destination_y,
                        blend565(surface.pixel(destination_x, destination_y),
                                 unchecked_tint, coverage));
                }
            }
        }
    }
}

std::uint16_t task_checkbox_unchecked_tint(bool dark,
                                           bool high_contrast,
                                           bool selected) {
    if (selected || dark) return rgb565(255, 255, 255);
    return high_contrast ? rgb565(0, 0, 0) : rgb565(0, 0, 0);
}

constexpr const char* kSampleText[] = {
    u8"Unicode text on RGB565",
    u8"Latin: Café naïve — déjà vu",
    u8"Greek: Ελληνικά α β γ Ω",
    u8"Cyrillic: Привет, мир · Ж",
    u8"Math: ∫ ∑ √ ∞ ≤ ≥ → ↔",
    u8"Combining: Café · Å · ñ",
    u8"Missing CJK: 中 → �",
};

constexpr const char* kOverlayText[] = {
    "Reader controls",
    "Arrows / 2 4 6 8: navigate",
    "Tab / 1: Page Down - 7: Page Up",
    "Menu: settings - Scratchpad: files",
    "Doc / Esc: close - Ctrl+Esc: exit",
};

std::size_t text_size(const char* text) {
    return std::char_traits<char>::length(text);
}

int font_slot_index(FontRole role) {
    return external_font_role_index(role);
}

const char* font_role_name(std::size_t index) {
    static constexpr const char* kNames[kExternalFontRoleCount] = {
        "Body", "Body Italic", "Monospace", "CJK", "Body Bold",
        "Body Bold Italic", "Monospace Italic",
    };
    return index < kExternalFontRoleCount ? kNames[index] : "Font";
}

bool suggested_font_role(const FontFaceCatalogEntry& face,
                         std::size_t index) {
    switch (external_font_role(index)) {
    case FontRole::BodySans:
        return !face.italic && !face.bold && face.has_latin &&
               !face.fixed_pitch;
    case FontRole::BodySansItalic:
        return face.italic && !face.bold && face.has_latin &&
               !face.fixed_pitch;
    case FontRole::Monospace:
        return !face.italic && !face.bold && face.has_latin &&
               face.fixed_pitch;
    case FontRole::Cjk:
        return !face.italic && !face.bold && face.has_cjk;
    case FontRole::BodySansBold:
        return !face.italic && face.bold && face.has_latin &&
               !face.fixed_pitch;
    case FontRole::BodySansBoldItalic:
        return face.italic && face.bold && face.has_latin &&
               !face.fixed_pitch;
    case FontRole::MonospaceItalic:
        return face.italic && !face.bold && face.has_latin &&
               face.fixed_pitch;
    default:
        return false;
    }
}

std::string font_face_label(const FontFaceCatalogEntry& face) {
    std::string label = face.family;
    if (label.empty()) {
        label = face.path;
        const std::size_t slash = label.find_last_of("/\\");
        if (slash != std::string::npos) label.erase(0, slash + 1);
    }
    if (!face.subfamily.empty() && face.subfamily != "Regular") {
        label += " ";
        label += face.subfamily;
    }
    return label;
}

std::string font_capability_label(const FontFaceCatalogEntry& face) {
    std::string label = "Detected:";
    if (face.has_latin) label += " Latin";
    if (face.has_cjk) label += " CJK";
    label += face.fixed_pitch ? " fixed" : " proportional";
    if (face.italic) label += " italic";
    if (face.bold) label += " bold";
    if (face.variable) label += " variable";
    return label;
}

const char* default_font_label(FontRole role) {
    switch (role) {
    case FontRole::BodySans: return "ASCII UI";
    case FontRole::BodySansItalic: return "Outline slant";
    case FontRole::Monospace: return "Built-in DejaVu Mono";
    case FontRole::Cjk: return "None";
    case FontRole::BodySansBold: return "Synthetic bold";
    case FontRole::BodySansBoldItalic: return "Synthetic bold italic";
    case FontRole::MonospaceItalic: return "Outline slant";
    default: return "None";
    }
}

std::string document_file_label(std::string path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".tns") == 0) {
        path.resize(path.size() - 4);
    }
    return path;
}

std::string parent_path_suffix(std::string path, std::size_t depth) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    path.resize(slash);
    std::size_t begin = path.size();
    for (std::size_t level = 0; level < depth && begin != 0; ++level) {
        const std::size_t from = begin == path.size()
                                     ? begin - 1
                                     : (begin >= 2 ? begin - 2 : 0);
        const std::size_t previous = path.find_last_of("/\\", from);
        begin = previous == std::string::npos ? 0 : previous + 1;
    }
    while (begin < path.size() && (path[begin] == '/' || path[begin] == '\\')) ++begin;
    return begin < path.size() ? path.substr(begin) : path;
}

bool source_ranges_overlap(std::uint32_t left_offset,
                           std::uint32_t left_length,
                           std::uint32_t right_offset,
                           std::uint32_t right_length) {
    const std::uint64_t left_end = static_cast<std::uint64_t>(left_offset) + left_length;
    const std::uint64_t right_end = static_cast<std::uint64_t>(right_offset) + right_length;
    return left_length != 0 && right_length != 0 &&
           left_offset < right_end && right_offset < left_end;
}

Rect search_highlight_rect(const LayoutRun& run,
                           const SearchMatch& match,
                           int origin_x,
                           int baseline) {
    Fx start_x = 0;
    Fx end_x = run.math != nullptr ? run.math->metrics.width : run.glyphs.width;
    Fx ascent = run.math != nullptr ? run.math->metrics.ascent : run.glyphs.ascent;
    Fx descent = run.math != nullptr
                     ? run.math->metrics.descent
                     : (run.glyphs.descent < 0 ? -run.glyphs.descent
                                               : run.glyphs.descent);
    if (run.math == nullptr && run.exact_source_mapping && !run.glyphs.glyphs.empty()) {
        const std::uint32_t overlap_begin = std::max(run.source_offset,
                                                     match.source_offset);
        const std::uint32_t overlap_end = std::min(
            run.source_offset + run.source_length,
            match.source_offset + match.source_length);
        const std::uint32_t local_begin = overlap_begin - run.source_offset;
        const std::uint32_t local_end = overlap_end - run.source_offset;
        Fx pen = 0;
        bool started = false;
        for (const PositionedGlyph& glyph : run.glyphs.glyphs) {
            if (!started && glyph.source_cluster >= local_begin) {
                start_x = pen;
                started = true;
            }
            if (started && glyph.source_cluster >= local_end) {
                end_x = pen;
                break;
            }
            pen += glyph.x_advance;
        }
    }
    return {origin_x + fx_floor(start_x) - 1,
            baseline - fx_ceil(ascent) - 1,
            std::max(2, fx_ceil(end_x - start_x) + 2),
            std::max(2, fx_ceil(ascent + descent) + 2)};
}

std::string source_snippet_view(std::string_view source, std::uint32_t offset) {
    if (source.empty()) return "Empty document";
    const std::size_t center = std::min<std::size_t>(offset, source.size());
    std::size_t begin = center > 12 ? center - 12 : 0;
    while (begin < center &&
           (static_cast<unsigned char>(source[begin]) & 0xC0U) == 0x80U) ++begin;
    std::size_t end = std::min(source.size(), center + 64);
    while (end < source.size() &&
           (static_cast<unsigned char>(source[end]) & 0xC0U) == 0x80U) --end;
    std::string result(source.substr(begin, end - begin));
    for (char& character : result) {
        if (character == '\n' || character == '\r' || character == '\t') character = ' ';
    }
    return result;
}

std::string document_source_slice(const MarkdownDocument& document,
                                  std::size_t begin,
                                  std::size_t length) {
    const std::size_t source_size = document.source_size();
    if (begin >= source_size || length == 0) return {};
    const std::size_t end = std::min(source_size, begin + length);
    std::string result;
    result.reserve(end - begin);
    for (std::size_t index = 0; index < document.source_chunk_count(); ++index) {
        const std::string_view chunk = document.source_chunk(index);
        const std::size_t chunk_begin = document.source_chunk_offset(index);
        const std::size_t chunk_end = chunk_begin + chunk.size();
        if (chunk_end <= begin) continue;
        if (chunk_begin >= end) break;
        const std::size_t copy_begin = std::max(begin, chunk_begin);
        const std::size_t copy_end = std::min(end, chunk_end);
        result.append(chunk.data() + (copy_begin - chunk_begin),
                      copy_end - copy_begin);
    }
    return result;
}

std::string source_snippet(const MarkdownDocument& document,
                           std::uint32_t offset) {
    const std::size_t source_size = document.source_size();
    if (source_size == 0) return "Empty document";
    const std::size_t center = std::min<std::size_t>(offset, source_size);
    const std::size_t slice_begin = center > 16 ? center - 16 : 0;
    const std::string slice = document_source_slice(
        document, slice_begin, std::min<std::size_t>(96, source_size - slice_begin));
    return source_snippet_view(
        slice, static_cast<std::uint32_t>(center - slice_begin));
}

std::vector<SearchMatch> search_document_source(
    const MarkdownDocument& document,
    std::string_view query,
    SearchMode mode,
    std::size_t maximum_results) {
    std::vector<SearchMatch> results;
    if (query.empty() || maximum_results == 0) return results;

    // Search a small overlap with the preceding allocation so a match at an
    // arbitrary 32 KiB storage edge behaves like a contiguous source match.
    constexpr std::size_t kBoundaryOverlap = 256;
    std::string tail;
    for (std::size_t index = 0;
         index < document.source_chunk_count() &&
         results.size() < maximum_results;
         ++index) {
        const std::string_view chunk = document.source_chunk(index);
        const std::uint32_t chunk_offset = document.source_chunk_offset(index);
        std::string window;
        window.reserve(tail.size() + chunk.size());
        window += tail;
        window.append(chunk.data(), chunk.size());
        const std::uint32_t window_offset =
            chunk_offset - static_cast<std::uint32_t>(tail.size());
        std::vector<SearchMatch> part = search_source(
            window, query, mode, maximum_results - results.size());
        for (SearchMatch& match : part) {
            const std::uint32_t global_offset = window_offset + match.source_offset;
            const std::uint64_t global_end =
                static_cast<std::uint64_t>(global_offset) + match.source_length;
            if (index != 0 && global_end <= chunk_offset) continue;
            match.source_offset = global_offset;
            if (!results.empty() &&
                results.back().source_offset == match.source_offset &&
                results.back().source_length == match.source_length) {
                continue;
            }
            results.push_back(std::move(match));
            if (results.size() == maximum_results) break;
        }
        const std::size_t keep = std::min(kBoundaryOverlap, window.size());
        std::size_t tail_begin = window.size() - keep;
        while (tail_begin < window.size() &&
               (static_cast<unsigned char>(window[tail_begin]) & 0xC0U) ==
                   0x80U) {
            ++tail_begin;
        }
        tail.assign(window.data() + tail_begin, window.size() - tail_begin);
    }
    return results;
}

std::string trailing_utf8(std::string_view value, std::size_t maximum_bytes) {
    if (value.size() <= maximum_bytes) return std::string(value);
    std::size_t begin = value.size() - maximum_bytes;
    while (begin < value.size() &&
           (static_cast<unsigned char>(value[begin]) & 0xC0U) == 0x80U) {
        ++begin;
    }
    return std::string(u8"…") + std::string(value.substr(begin));
}

int hexadecimal_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hexadecimal_digit(value[index + 1]);
            const int low = hexadecimal_digit(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
    }
    return result;
}

std::string heading_slug(std::string_view title) {
    std::string result;
    bool pending_dash = false;
    for (unsigned char byte : title) {
        if (byte < 0x80U && std::isspace(byte)) {
            pending_dash = !result.empty();
            continue;
        }
        if (byte < 0x80U && !(std::isalnum(byte) || byte == '-' || byte == '_')) {
            continue;
        }
        if (pending_dash && !result.empty() && result.back() != '-') result.push_back('-');
        pending_dash = false;
        result.push_back(byte < 0x80U ? static_cast<char>(std::tolower(byte))
                                     : static_cast<char>(byte));
    }
    return result;
}

bool has_uri_scheme(std::string_view target) {
    if (target.empty() || !std::isalpha(static_cast<unsigned char>(target.front()))) {
        return false;
    }
    for (std::size_t index = 1; index < target.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(target[index]);
        if (target[index] == ':') return true;
        if (!(std::isalnum(value) || target[index] == '+' || target[index] == '-' ||
              target[index] == '.')) return false;
    }
    return false;
}

}  // namespace

Viewer::Viewer()
    : retained_base_frame_(static_cast<std::size_t>(kScreenWidth) *
                           kScreenHeight),
      math_(text_) {
    std::string error;
    text_ready_ = text_.initialize(error);
    if (text_ready_) {
        font_pack_signature_ = text_.font_signature();
        if (font_pack_signature_ == 0) font_pack_signature_ = 1;
        rebuild_text_runs();
    }
}

void Viewer::invalidate_retained_base_frame() {
    retained_base_frame_valid_ = false;
    display_surface_is_base_frame_ = false;
}

bool Viewer::capture_retained_base_frame(const Surface565& surface) {
    if (!surface.valid() || surface.width() != kScreenWidth ||
        surface.height() != kScreenHeight ||
        retained_base_frame_.size() !=
            static_cast<std::size_t>(kScreenWidth) * kScreenHeight) {
        retained_base_frame_valid_ = false;
        return false;
    }
    for (int y = 0; y < kScreenHeight; ++y) {
        std::copy_n(surface.row(y), kScreenWidth,
                    retained_base_frame_.data() +
                        static_cast<std::size_t>(y) * kScreenWidth);
    }
    retained_base_frame_valid_ = true;
    return true;
}

bool Viewer::restore_retained_base_frame(const Surface565& surface) {
    if (!retained_base_frame_valid_ || !surface.valid() ||
        surface.width() != kScreenWidth || surface.height() != kScreenHeight) {
        return false;
    }
    for (int y = 0; y < kScreenHeight; ++y) {
        std::copy_n(retained_base_frame_.data() +
                        static_cast<std::size_t>(y) * kScreenWidth,
                    kScreenWidth, surface.row(y));
    }
    return true;
}

bool Viewer::rebuild_chrome_title(int maximum_width) {
    maximum_width = std::max(0, maximum_width);
    if (chrome_title_max_width_ == maximum_width &&
        !chrome_title_.glyphs.empty()) {
        return true;
    }
    const std::string& title = document_title_.empty()
                                   ? default_document_title_
                                   : document_title_;
    GlyphRun shaped;
    if (!text_.shape(title.data(), title.size(), fx_from_int(13), shaped)) {
        return false;
    }
    if (fx_ceil(shaped.width) <= maximum_width) {
        chrome_title_ = std::move(shaped);
        chrome_title_max_width_ = maximum_width;
        return true;
    }

    constexpr char ellipsis[] = u8"…";
    std::size_t end = title.size();
    while (end != 0) {
        --end;
        while (end != 0 &&
               (static_cast<unsigned char>(title[end]) & 0xC0U) == 0x80U) {
            --end;
        }
        const std::string candidate = title.substr(0, end) + ellipsis;
        if (!text_.shape(candidate.data(), candidate.size(), fx_from_int(13),
                         shaped)) {
            return false;
        }
        if (fx_ceil(shaped.width) <= maximum_width) {
            chrome_title_ = std::move(shaped);
            chrome_title_max_width_ = maximum_width;
            return true;
        }
    }
    if (!text_.shape(ellipsis, sizeof(ellipsis) - 1, fx_from_int(13), shaped)) {
        return false;
    }
    chrome_title_ = std::move(shaped);
    chrome_title_max_width_ = maximum_width;
    return true;
}

void Viewer::rebuild_text_runs() {
    sample_runs_.clear();
    overlay_runs_.clear();
    chrome_title_ = {};
    chrome_title_max_width_ = -1;
    toc_title_ = {};
    search_title_ = {};
    bookmark_title_ = {};
    bookmark_hint_run_ = {};
    settings_title_ = {};
    diagnostics_title_ = {};
    document_browser_title_ = {};
    font_browser_title_ = {};
    link_title_ = {};
    link_target_runs_.clear();
    link_hint_run_ = {};
    document_error_title_run_ = {};
    document_error_message_runs_.clear();
    document_error_hint_run_ = {};
    empty_document_run_ = {};
    empty_document_hint_run_ = {};
    bookmark_empty_run_ = {};
    toc_empty_run_ = {};
    if (!text_ready_) {
        return;
    }

    text_ready_ = rebuild_chrome_title(
        kScreenWidth - kChromeX - kChromeRightPadding);
    text_ready_ = text_ready_ &&
                  text_.shape("Table of contents",
                              text_size("Table of contents"),
                              fx_from_int(kMenuTitlePixelSize),
                              toc_title_);
    text_ready_ = text_ready_ &&
                  text_.shape("Bookmarks", text_size("Bookmarks"),
                              fx_from_int(kMenuTitlePixelSize), bookmark_title_);
    text_ready_ = text_ready_ &&
                  text_.shape("+ adds · Del removes",
                              text_size("+ adds · Del removes"),
                              fx_from_int(kMenuAuxiliaryPixelSize),
                              bookmark_hint_run_);
    text_ready_ = text_ready_ &&
                  text_.shape("Reader settings", text_size("Reader settings"),
                              fx_from_int(kMenuTitlePixelSize), settings_title_);
    text_ready_ = text_ready_ &&
                  text_.shape("Reader diagnostics", text_size("Reader diagnostics"),
                              fx_from_int(kMenuTitlePixelSize), diagnostics_title_);
    text_ready_ = text_ready_ &&
                  text_.shape("Open Markdown", text_size("Open Markdown"),
                              fx_from_int(kMenuTitlePixelSize), document_browser_title_);
    text_ready_ = text_ready_ &&
                  text_.shape("Choose font role", text_size("Choose font role"),
                              fx_from_int(kMenuTitlePixelSize), font_browser_title_);
    text_ready_ = text_ready_ &&
                  text_.shape("Could not open document",
                              text_size("Could not open document"),
                              fx_from_int(16), document_error_title_run_);
    text_ready_ = text_ready_ &&
                  text_.shape("Scratchpad / Ctrl+O: files - Esc: exit",
                              text_size("Scratchpad / Ctrl+O: files - Esc: exit"),
                              fx_from_int(10), document_error_hint_run_);
    text_ready_ = text_ready_ &&
                  text_.shape("This document is empty.",
                              text_size("This document is empty."),
                              fx_from_int(15), empty_document_run_);
    text_ready_ = text_ready_ &&
                  text_.shape("Open another Markdown file or press Esc to exit.",
                              text_size("Open another Markdown file or press Esc to exit."),
                              fx_from_int(10), empty_document_hint_run_);
    text_ready_ = text_ready_ &&
                  text_.shape("No bookmarks yet - Ctrl+B adds one",
                              text_size("No bookmarks yet - Ctrl+B adds one"),
                              fx_from_int(kMenuCompactPixelSize), bookmark_empty_run_);
    text_ready_ = text_ready_ &&
                  text_.shape("No headings in this document",
                              text_size("No headings in this document"),
                              fx_from_int(kMenuCompactPixelSize), toc_empty_run_);
    if (!document_error_message_.empty()) {
        document_error_message_runs_ = wrap_text_runs(
            text_, document_error_message_, fx_from_int(11),
            kScreenWidth - 24, 2);
    }
    for (const char* sample : kSampleText) {
        GlyphRun run;
        text_ready_ = text_ready_ &&
                      text_.shape(sample,
                                  text_size(sample),
                                  fx_from_int(body_pixel_size_),
                                  run);
        sample_runs_.push_back(std::move(run));
    }
    for (const char* line : kOverlayText) {
        GlyphRun run;
        text_ready_ = text_ready_ &&
                      text_.shape(line, text_size(line),
                                  fx_from_int(kMenuListPixelSize), run);
        overlay_runs_.push_back(std::move(run));
    }
    rebuild_jump_runs();
    // A message dialog can be open while the runs are rebuilt — for example
    // the CJK font prompt shown during a document load, followed by the
    // saved-state restore. Re-shape its retained strings so the open dialog
    // does not turn blank on the next frame.
    if (link_overlay_ && !link_choice_mode_) {
        reshape_message_dialog_runs();
    }
}

void Viewer::rebuild_toc_runs() {
    toc_runs_.clear();
    if (!text_ready_ || markdown_document_ == nullptr) return;
    toc_runs_.reserve(markdown_document_->ir.headings.size());
    for (const HeadingEntry& heading : markdown_document_->ir.headings) {
        std::string_view title = markdown_document_->text(heading.title);
        if (title.empty()) title = "Untitled heading";
        GlyphRun run;
        if (!text_.shape(title.data(), title.size(),
                         fx_from_int(kMenuListPixelSize), run)) run = {};
        toc_runs_.push_back(std::move(run));
    }
    if (toc_selected_ >= toc_runs_.size()) toc_selected_ = 0;
}

void Viewer::rebuild_search_runs() {
    search_result_runs_.clear();
    search_title_ = {};
    search_query_run_ = {};
    search_status_run_ = {};
    if (!text_ready_) return;

    const char* mode_name = "ASCII fold";
    switch (search_mode_) {
    case SearchMode::ExactUtf8: mode_name = "exact"; break;
    case SearchMode::AsciiCaseInsensitive: mode_name = "ASCII fold"; break;
    case SearchMode::CanonicalUtf8: mode_name = "canonical"; break;
    case SearchMode::UnicodeCaseInsensitive: mode_name = "Unicode fold"; break;
    }
    const std::string title = std::string("Search · ") + mode_name;
    const std::string query = search_query_.empty()
                                  ? "> Type a query"
                                  : "> " + trailing_utf8(search_query_, 42);
    const std::string status = std::to_string(search_results_.size()) +
                               (search_results_.size() == 1 ? " match" : " matches") +
                               " · " + std::to_string(search_query_.size()) +
                               "/64 bytes";
    text_.shape(title.data(), title.size(), fx_from_int(kMenuTitlePixelSize),
                search_title_);
    text_.shape(query.data(), query.size(),
                fx_from_int(kMenuSearchInputPixelSize), search_query_run_);
    text_.shape(status.data(), status.size(),
                fx_from_int(kMenuAuxiliaryPixelSize), search_status_run_);
    search_result_runs_.reserve(search_results_.size());
    for (const SearchMatch& match : search_results_) {
        std::string label;
        if (markdown_document_ != nullptr) {
            for (const HeadingEntry& heading : markdown_document_->ir.headings) {
                if (heading.source_offset > match.source_offset) break;
                const std::string_view heading_text = markdown_document_->text(heading.title);
                if (!heading_text.empty()) label.assign(heading_text);
            }
        }
        if (!label.empty()) label += " · ";
        label += match.snippet;
        GlyphRun run;
        text_.shape(label.data(), label.size(),
                    fx_from_int(kMenuCompactPixelSize), run);
        search_result_runs_.push_back(std::move(run));
    }
}

void Viewer::update_search() {
    search_results_.clear();
    search_selected_ = 0;
    if (markdown_document_ != nullptr && !search_query_.empty()) {
        search_results_ = search_document_source(
            *markdown_document_,
            search_query_,
            search_mode_,
            128);
    } else if (plain_text_layout_.loaded() && !search_query_.empty()) {
        std::string error;
        search_results_ = plain_text_layout_.search(
            search_query_, search_mode_, 128, error);
        if (!error.empty()) {
            search_results_.clear();
        }
    }
    rebuild_search_runs();
    dirty_ = true;
}

void Viewer::rebuild_jump_runs() {
    jump_title_ = {};
    jump_query_run_ = {};
    jump_hint_run_ = {};
    if (!text_ready_) return;
    const std::string query = jump_query_.empty()
                                  ? "> 0-100%"
                                  : "> " + jump_query_ + "%";
    text_.shape("Jump to percentage", text_size("Jump to percentage"),
                fx_from_int(kMenuTitlePixelSize), jump_title_);
    text_.shape(query.data(), query.size(),
                fx_from_int(kMenuSearchInputPixelSize), jump_query_run_);
    text_.shape("Type a percentage, then press Enter",
                text_size("Type a percentage, then press Enter"),
                fx_from_int(kMenuAuxiliaryPixelSize), jump_hint_run_);
}

void Viewer::rebuild_bookmark_runs() {
    bookmark_runs_.clear();
    if (!text_ready_ ||
        (markdown_document_ == nullptr && !plain_text_layout_.loaded())) {
        return;
    }
    bookmark_runs_.reserve(bookmarks_.size());
    const std::uint64_t total_bytes =
        markdown_document_ != nullptr
            ? markdown_document_->source_size()
            : plain_text_layout_.source_size();
    for (std::size_t index = 0; index < bookmarks_.size(); ++index) {
        std::string context;
        if (markdown_document_ != nullptr) {
            context = source_snippet(*markdown_document_, bookmarks_[index]);
        } else {
            std::string ignored_error;
            context = plain_text_layout_.snippet(
                bookmarks_[index], ignored_error);
            if (context.empty()) context = "Saved text position";
        }
        // Reading progress with one decimal, computed in integer permille.
        const unsigned permille =
            total_bytes == 0
                ? 0U
                : static_cast<unsigned>(std::min<std::uint64_t>(
                      1000U,
                      static_cast<std::uint64_t>(bookmarks_[index]) * 1000U /
                          total_bytes));
        const std::string label =
            std::to_string(permille / 10U) + "." +
            std::to_string(permille % 10U) + "% · " + context;
        GlyphRun run;
        text_.shape(label.data(), label.size(),
                    fx_from_int(kMenuListPixelSize), run);
        bookmark_runs_.push_back(std::move(run));
    }
    if (bookmark_selected_ >= bookmark_runs_.size()) bookmark_selected_ = 0;
}

void Viewer::rebuild_settings_runs() {
    settings_runs_.clear();
    if (!text_ready_) return;
    std::string font_summary = "Fonts: " +
                               std::to_string(text_.external_font_count()) +
                               " loaded";
    const std::string labels[] = {
        std::string("Theme: ") + (dark_theme_ ? "Dark" : "Light"),
        "Font size: " + std::to_string(body_pixel_size_) + " px",
        line_gap_px_ < 0
            ? "Line spacing: Auto"
            : "Line spacing: +" + std::to_string(line_gap_px_) + " px",
        "Side margins: " + std::to_string(side_margin_px_) + " px",
        std::string("Tables: ") + (table_mode_ == 0 ? "Responsive" : "Grid + pan"),
        std::string("Code blocks: ") + (code_wrap_ ? "Wrap" : "Pan"),
        std::string("Contrast: ") + (high_contrast_ ? "High" : "Standard"),
        "Text sharpness: " + std::to_string(render_sharpness_) + "/10",
        std::string("Touchpad mode: ") +
            (reading_mode_ == ReadingMode::VerticalScroll
                 ? "Vertical scroll"
                 : "Horizontal scroll"),
        std::string("Swipe gesture direction: ") +
            (natural_swiping_ ? "Natural" : "Reversed"),
        std::string("Scroll gesture direction: ") +
            (natural_scrolling_ ? "Natural" : "Reversed"),
        std::string("Font preload: ") +
            (resident_font_preload_ ? "Auto" : "Off"),
        std::move(font_summary),
    };
    static_assert(sizeof(labels) / sizeof(labels[0]) == kSettingsRowCount,
                  "settings label count must match navigation bounds");
    for (const std::string& label : labels) {
        GlyphRun run;
        text_.shape(label.data(), label.size(),
                    fx_from_int(kMenuCompactPixelSize), run);
        settings_runs_.push_back(std::move(run));
    }
}

void Viewer::set_resident_font_preload(bool enabled) {
    if (resident_font_preload_ == enabled) return;
    resident_font_preload_ = enabled;
    rebuild_settings_runs();
}

bool Viewer::take_font_preload_save_request() {
    const bool requested = pending_font_preload_save_request_;
    pending_font_preload_save_request_ = false;
    return requested;
}

void Viewer::set_dark_theme(bool dark) {
    if (dark_theme_ == dark) return;
    invalidate_retained_base_frame();
    dark_theme_ = dark;
    // Theme changes only affect colors. Keep layout and glyph/formula caches,
    // but reshape this label so an open settings panel updates immediately.
    rebuild_settings_runs();
}

void Viewer::show_content_jump() {
    overlay_open_ = true;
    toc_overlay_ = false;
    jump_overlay_ = false;
    search_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    link_overlay_ = false;
    if (plain_text_layout_.loaded()) {
        jump_overlay_ = true;
        jump_query_.clear();
        rebuild_jump_runs();
        return;
    }
    toc_overlay_ = markdown_document_ != nullptr &&
                   (!toc_runs_.empty() || !bookmark_runs_.empty());
    if (toc_runs_.empty() && !bookmark_runs_.empty()) bookmark_tab_ = true;
}

void Viewer::begin_settings_session() {
    if (settings_session_active_) return;
    settings_snapshot_.dark_theme = dark_theme_;
    settings_snapshot_.high_contrast = high_contrast_;
    settings_snapshot_.body_pixel_size = body_pixel_size_;
    settings_snapshot_.line_gap_px = line_gap_px_;
    settings_snapshot_.side_margin_px = side_margin_px_;
    settings_snapshot_.code_wrap = code_wrap_;
    settings_snapshot_.table_mode = table_mode_;
    settings_snapshot_.reading_mode = reading_mode_;
    settings_snapshot_.natural_scrolling = natural_scrolling_;
    settings_snapshot_.natural_swiping = natural_swiping_;
    settings_snapshot_.resident_font_preload = resident_font_preload_;
    settings_snapshot_.render_sharpness = render_sharpness_;
    settings_snapshot_.scroll_y = scroll_y_;
    settings_snapshot_.max_scroll_y = max_scroll_y();
    settings_snapshot_.plain_text_offset =
        plain_text_layout_.current_source_offset();
    settings_snapshot_.anchor = markdown_document_ != nullptr
                                    ? markdown_layout_.anchor_at(
                                          fx_from_int(scroll_y_))
                                    : ViewAnchor{};
    settings_session_active_ = true;
    settings_overlay_repaint_only_ = false;
}

void Viewer::commit_settings_session() {
    if (!settings_session_active_) return;
    settings_session_active_ = false;
    settings_overlay_repaint_only_ = false;

    const bool font_size_changed =
        body_pixel_size_ != settings_snapshot_.body_pixel_size;
    const bool layout_changed = font_size_changed ||
        line_gap_px_ != settings_snapshot_.line_gap_px ||
        side_margin_px_ != settings_snapshot_.side_margin_px ||
        code_wrap_ != settings_snapshot_.code_wrap ||
        table_mode_ != settings_snapshot_.table_mode;
    const bool reading_mode_changed =
        reading_mode_ != settings_snapshot_.reading_mode;
    const bool render_sharpness_changed =
        render_sharpness_ != settings_snapshot_.render_sharpness;
    // The preload switch persists globally (with the font preferences), so
    // request a save even when nothing else forces a repaint below.
    if (resident_font_preload_ !=
        settings_snapshot_.resident_font_preload) {
        pending_font_preload_save_request_ = true;
    }
    const bool changed = layout_changed || reading_mode_changed ||
        dark_theme_ != settings_snapshot_.dark_theme ||
        high_contrast_ != settings_snapshot_.high_contrast ||
        natural_scrolling_ != settings_snapshot_.natural_scrolling ||
        natural_swiping_ != settings_snapshot_.natural_swiping ||
        resident_font_preload_ !=
            settings_snapshot_.resident_font_preload ||
        render_sharpness_changed;
    if (!changed) return;
    invalidate_retained_base_frame();

    if (render_sharpness_changed) {
        text_.set_render_sharpness(render_sharpness_);
    }

    if (font_size_changed) {
        text_.clear_cache();
        math_.clear_cache();
        rebuild_text_runs();
        rebuild_toc_runs();
        rebuild_bookmark_runs();
        rebuild_search_runs();
    }
    if (layout_changed && markdown_document_ != nullptr) {
        std::string error;
        if (markdown_layout_.reconfigure(layout_signature(), error)) {
            document_height_ = std::max(
                kViewportHeight, fx_ceil(markdown_layout_.total_height()));
            scroll_y_ = fx_floor(
                markdown_layout_.position_of(settings_snapshot_.anchor));
        } else {
            set_document_error(error);
        }
    } else if (layout_changed && plain_text_layout_.loaded()) {
        std::string error;
        if (!plain_text_layout_.reconfigure(layout_signature(), error) ||
            !plain_text_layout_.seek_source(
                settings_snapshot_.plain_text_offset, error)) {
            set_document_error(error);
        }
    } else if (layout_changed && font_size_changed) {
        document_height_ = std::max(
            720, document_height_ * body_pixel_size_ /
                     std::max(1, settings_snapshot_.body_pixel_size));
        if (settings_snapshot_.max_scroll_y > 0) {
            scroll_y_ = static_cast<int>(
                static_cast<std::int64_t>(settings_snapshot_.scroll_y) *
                max_scroll_y() / settings_snapshot_.max_scroll_y);
        }
    }

    if (layout_changed) {
        wide_focus_ = false;
        focused_node_ = kInvalidNode;
        focused_maximum_pan_ = 0;
        focused_code_layout_ = {};
        focused_code_layout_valid_ = false;
        focused_code_top_y_ = 0;
        focused_scroll_restore_valid_ = false;
        pan_x_ = 0;
    }
    if (reading_mode_changed) {
    }
    rebuild_settings_runs();
    clamp_view();
    dirty_ = true;
}

void Viewer::rebuild_diagnostics_runs() {
    diagnostics_runs_.clear();
    if (!text_ready_) return;

    const GlyphCacheStats glyph = text_.cache_stats();
    const FormulaCacheStats formula = math_.cache_stats();
    const auto percentage = [](std::uint64_t hits, std::uint64_t misses) {
        const std::uint64_t total = hits + misses;
        return total == 0 ? 0U : static_cast<unsigned>(hits * 100U / total);
    };
    std::uint32_t source_offset = 0;
    if (markdown_document_ != nullptr && markdown_layout_.unit_count() != 0) {
        source_offset = markdown_layout_.anchor_at(fx_from_int(scroll_y_)).source_offset;
    } else if (plain_text_layout_.loaded()) {
        source_offset = plain_text_layout_.current_source_offset();
    }
    const std::size_t source_kib = markdown_document_ == nullptr
                                       ? static_cast<std::size_t>((document_.size + 1023U) / 1024U)
                                       : (markdown_document_->source_size() + 1023U) / 1024U;
    const std::size_t blocks = markdown_document_ == nullptr
                                   ? 0U : markdown_document_->ir.blocks.size();
    const std::size_t tokens = markdown_document_ == nullptr
                                   ? 0U : markdown_document_->ir.tokens.size();
    std::vector<std::string> labels = {
        "Load / parse: " + std::to_string(performance_metrics_.document_load_parse_ms) + " ms",
        "First / last render: " +
            std::to_string(performance_metrics_.first_visible_render_ms) + " / " +
            std::to_string(performance_metrics_.last_visible_render_ms) + " ms",
        "Peak render / present: " +
            std::to_string(performance_metrics_.peak_visible_render_ms) + " / " +
            std::to_string(performance_metrics_.peak_present_ms) + " ms",
        "Source: " + std::to_string(source_kib) + " KiB; IR: " +
            std::to_string(blocks) + " blocks, " + std::to_string(tokens) + " tokens",
        "Current source offset: " + std::to_string(source_offset),
        "Layout: " + std::to_string(markdown_layout_.measured_count()) + " / " +
            std::to_string(markdown_layout_.unit_count()) + " measured; " +
            std::to_string(markdown_layout_.cache_size()) + " cached",
        "Glyph cache: " + std::to_string(percentage(glyph.hits, glyph.misses)) +
            "% hit; " + std::to_string(glyph.misses) + " rasterized; " +
            std::to_string(glyph.evictions) + " evicted",
        "Formula cache: " + std::to_string(percentage(formula.hits, formula.misses)) +
            "% hit; " + std::to_string(formula.misses) + " layouts; " +
            std::to_string(formula.entries) + " cached",
        "External fonts: " +
            std::to_string((text_.external_font_bytes() + 1023U) / 1024U) +
            " KiB on disk (streamed)",
    };
    const AllocationStats allocation = allocation_stats();
    if (allocation.available) {
        const auto kib = [](std::uint64_t bytes) {
            return (bytes + 1023U) / 1024U;
        };
        std::string label =
            "Heap KiB: now " + std::to_string(kib(allocation.current_bytes)) +
            " / peak " +
            std::to_string(kib(allocation.lifetime_peak_bytes));
        if (allocation.checkpoint_available) {
            label += " / font " +
                     std::to_string(kib(allocation.checkpoint_peak_bytes));
        }
        labels.push_back(std::move(label));
    }
    for (const std::string& label : labels) {
        GlyphRun run;
        text_.shape(label.data(), label.size(),
                    fx_from_int(kMenuAuxiliaryPixelSize), run);
        diagnostics_runs_.push_back(std::move(run));
    }
}

void Viewer::rebuild_document_browser_runs() {
    document_browser_runs_.clear();
    if (!text_ready_) return;
    if (document_browser_paths_.empty()) {
        GlyphRun run;
        constexpr char message[] = "No Markdown files found";
        text_.shape(message, sizeof(message) - 1,
                    fx_from_int(kMenuListPixelSize), run);
        document_browser_runs_.push_back(std::move(run));
        document_browser_selected_ = 0;
        return;
    }
    std::vector<std::string> labels;
    labels.reserve(document_browser_paths_.size());
    for (const std::string& path : document_browser_paths_) {
        labels.push_back(document_file_label(path));
    }
    const std::vector<std::string> base_labels = labels;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const bool duplicate = std::count(base_labels.begin(), base_labels.end(),
                                          base_labels[index]) > 1;
        if (!duplicate) continue;
        std::string parent;
        for (std::size_t depth = 1; depth <= 12; ++depth) {
            parent = parent_path_suffix(document_browser_paths_[index], depth);
            bool unique = true;
            for (std::size_t other = 0; other < labels.size(); ++other) {
                if (other == index || base_labels[other] != base_labels[index]) continue;
                if (parent_path_suffix(document_browser_paths_[other], depth) == parent) {
                    unique = false;
                    break;
                }
            }
            if (unique) break;
        }
        labels[index] += " · " + parent;
    }
    document_browser_runs_.reserve(document_browser_paths_.size() +
                                   (document_browser_truncated_ ? 1U : 0U));
    for (const std::string& label : labels) {
        GlyphRun run;
        text_.shape(label.data(), label.size(),
                    fx_from_int(kMenuListPixelSize), run);
        document_browser_runs_.push_back(std::move(run));
    }
    if (document_browser_truncated_) {
        GlyphRun warning;
        constexpr char message[] = "More documents not shown";
        text_.shape(message, sizeof(message) - 1,
                    fx_from_int(kMenuListPixelSize), warning);
        document_browser_runs_.push_back(std::move(warning));
    }
    if (document_browser_selected_ >= document_browser_paths_.size()) {
        document_browser_selected_ = 0;
    }
}

void Viewer::show_document_browser(const std::vector<std::string>& paths,
                                   bool truncated) {
    commit_settings_session();
    document_browser_paths_ = paths;
    document_browser_truncated_ = truncated;
    document_browser_selected_ = 0;
    pending_document_open_.clear();
    pending_document_browser_request_ = false;
    rebuild_document_browser_runs();
    overlay_open_ = true;
    document_browser_overlay_ = true;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    font_browser_overlay_ = false;
    link_overlay_ = false;
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    dirty_ = true;
}

bool Viewer::take_document_open_request(std::string& path) {
    if (pending_document_open_.empty()) return false;
    path = std::move(pending_document_open_);
    pending_document_open_.clear();
    return true;
}

bool Viewer::take_document_browser_request() {
    const bool requested = pending_document_browser_request_;
    pending_document_browser_request_ = false;
    return requested;
}

bool Viewer::take_state_save_request() {
    const bool requested = pending_state_save_request_;
    pending_state_save_request_ = false;
    return requested;
}

void Viewer::rebuild_font_browser_runs() {
    font_browser_labels_.clear();
    if (!text_ready_) return;
    std::string title = "Installed fonts";
    if (font_detail_open_ && font_detail_index_ < font_file_catalog_.size()) {
        const FontFaceCatalogEntry& face =
            font_file_catalog_[font_detail_index_];
        title = font_face_label(face);
        font_browser_labels_.push_back(font_capability_label(face));
        for (std::size_t index = 0; index < kExternalFontRoleCount;
             ++index) {
            const bool assigned = pending_font_paths_[index] == face.path;
            std::string label = font_role_name(index);
            if (!assigned && suggested_font_role(face, index)) {
                label += " (suggested)";
            }
            font_browser_labels_.push_back(std::move(label));
        }
        font_browser_labels_.emplace_back("Use suggested roles");
        font_browser_labels_.emplace_back("Unload from roles");
    } else {
        for (const FontFaceCatalogEntry& face : font_file_catalog_) {
            font_browser_labels_.push_back(font_face_label(face));
        }
        font_browser_labels_.emplace_back("Apply changes");
        if (font_browser_truncated_) {
            font_browser_labels_.emplace_back("Font search stopped early");
        }
    }
    font_browser_title_ = {};
    text_.shape(title.data(), title.size(), fx_from_int(kMenuTitlePixelSize),
                font_browser_title_);
}

void Viewer::show_font_manager(
    const std::vector<FontFaceCatalogEntry>& fonts,
    const std::array<std::string, kExternalFontRoleCount>& active_paths,
    bool truncated) {
    commit_settings_session();
    font_detail_open_ = false;
    font_detail_index_ = 0;
    font_file_catalog_ = fonts;
    active_font_paths_ = active_paths;
    pending_font_paths_ = active_paths;
    font_browser_truncated_ = truncated;
    font_browser_selected_ = 0;
    pending_font_assignments_available_ = false;
    rebuild_font_browser_runs();
    overlay_open_ = true;
    font_browser_overlay_ = true;
    document_browser_overlay_ = false;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    link_overlay_ = false;
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    dirty_ = true;
}

bool Viewer::take_font_assignments(
    std::array<std::string, kExternalFontRoleCount>& paths) {
    if (!pending_font_assignments_available_) return false;
    paths = pending_font_paths_;
    pending_font_assignments_available_ = false;
    return true;
}

bool Viewer::take_font_menu_request() {
    const bool requested = pending_font_menu_request_;
    pending_font_menu_request_ = false;
    return requested;
}

void Viewer::activate_search_result(std::size_t index) {
    chrome_visible_ = true;
    if (index >= search_results_.size()) return;
    invalidate_retained_base_frame();
    search_selected_ = index;
    active_search_match_ = search_results_[index];
    has_active_search_match_ = true;
    if (plain_text_layout_.loaded()) {
        std::string error;
        if (!plain_text_layout_.seek_source(
                active_search_match_.source_offset, error)) {
            show_message("TXT read error", error);
        }
    } else {
        scroll_y_ = fx_floor(markdown_layout_.position_for_source(
            kInvalidNode, active_search_match_.source_offset, 0));
    }
    clamp_view();
}

bool Viewer::current_block_is_wide(NodeId& node, int& maximum_pan) {
    node = kInvalidNode;
    maximum_pan = 0;
    focused_code_layout_valid_ = false;
    focused_code_layout_ = {};
    focused_code_top_y_ = 0;
    if (markdown_document_ == nullptr || markdown_layout_.unit_count() == 0) return false;
    const auto inspect = [&](const BlockLayout& layout,
                             std::size_t unit_index,
                             bool formula_only) {
        bool overflowing_formula = false;
        for (const LayoutLine& line : layout.lines) {
            for (const LayoutRun& run : line.runs) {
                overflowing_formula = overflowing_formula ||
                    (run.math != nullptr && run.math->overflow);
            }
        }

        const BlockLayout* focus_layout = &layout;
        BlockLayout unwrapped_code;
        if (!formula_only && layout.code_background && code_wrap_) {
            if (!markdown_layout_.layout_unwrapped_code_unit(unit_index,
                                                              unwrapped_code)) {
                return false;
            }
            focus_layout = &unwrapped_code;
        }

        const int horizontal_padding =
            static_cast<int>(focus_layout->horizontal_inset_px) * 2;
        const Fx available_width = fx_from_int(std::max(
            24, content_width() - static_cast<int>(focus_layout->indent_px) -
                    horizontal_padding));
        const bool horizontally_overflowing =
            focus_layout->maximum_line_width > available_width;
        const bool focusable = overflowing_formula ||
            (!formula_only && focus_layout->code_background &&
             horizontally_overflowing) ||
            (!formula_only && focus_layout->kind == BlockKind::TableRow &&
             table_mode_ != 0 && horizontally_overflowing);
        if (!focusable || !horizontally_overflowing) return false;

        node = focus_layout->node;
        maximum_pan = std::max(
            1, fx_ceil(focus_layout->maximum_line_width - available_width));
        if (focus_layout == &unwrapped_code) {
            focused_code_layout_ = std::move(unwrapped_code);
            focused_code_layout_valid_ = true;
            focused_code_top_y_ = fx_floor(markdown_layout_.unit_top(unit_index));
        }
        return true;
    };

    // A touchpad click has Enter semantics rather than screen coordinates.
    // Prefer the first overflowing formula that is actually visible so a
    // short prose block at the viewport top cannot hide the equation the user
    // is trying to focus.
    const std::vector<VisibleBlock> visible = markdown_layout_.layout_window(
        fx_from_int(scroll_y_), fx_from_int(kViewportHeight), 0);
    for (const VisibleBlock& block : visible) {
        if (block.layout == nullptr) continue;
        const int top = fx_floor(block.document_y);
        const int bottom = top + std::max(1, fx_ceil(block.layout->height));
        if (bottom <= scroll_y_ || top >= scroll_y_ + kViewportHeight) continue;
        if (inspect(*block.layout, block.unit_index, true)) return true;
    }

    // Wrapped code is readable in the document, but focusing it exposes the
    // original source lines in a temporary pannable canvas. Give every visible
    // structured block the same discoverability as a visible formula instead
    // of requiring its top edge to match the viewport anchor exactly.
    for (const VisibleBlock& block : visible) {
        if (block.layout == nullptr) continue;
        const int top = fx_floor(block.document_y);
        const int bottom = top + std::max(1, fx_ceil(block.layout->height));
        if (bottom <= scroll_y_ || top >= scroll_y_ + kViewportHeight) continue;
        if (inspect(*block.layout, block.unit_index, false)) return true;
    }

    // Fallback for a lazily measured block at the current anchor.
    const std::size_t unit = markdown_layout_.unit_at(fx_from_int(scroll_y_));
    const BlockLayout* layout = markdown_layout_.layout_unit(unit);
    return layout != nullptr && inspect(*layout, unit, false);
}

void Viewer::enter_wide_focus(NodeId node, int maximum_pan) {
    wide_focus_ = true;
    focused_node_ = node;
    focused_maximum_pan_ = maximum_pan;
    pan_x_ = 0;
    if (focused_code_layout_valid_) {
        focused_return_scroll_y_ = scroll_y_;
        focused_scroll_restore_valid_ = true;
        scroll_y_ = focused_code_top_y_;
    } else {
        focused_scroll_restore_valid_ = false;
    }
}

void Viewer::exit_wide_focus() {
    const int return_scroll = focused_return_scroll_y_;
    const bool restore_scroll = focused_scroll_restore_valid_;
    wide_focus_ = false;
    focused_node_ = kInvalidNode;
    focused_maximum_pan_ = 0;
    focused_code_layout_ = {};
    focused_code_layout_valid_ = false;
    focused_code_top_y_ = 0;
    focused_scroll_restore_valid_ = false;
    pan_x_ = 0;
    if (restore_scroll) scroll_y_ = return_scroll;
}

// Horizontal input over content that needs panning is reserved for that pan.
// When the viewport holds a pannable block — an overflowing formula, code
// line, or grid table row — a left/right key, touchpad edge click, or swipe
// engages the focus canvas directly and moves it; page navigation never sees
// the event, and at the pan limits the position simply saturates instead of
// rolling a page. Plain text documents and viewports without pannable
// content return false so page turns behave exactly as before.
bool Viewer::consume_wide_pan(int pan_delta) {
    if (!wide_focus_) {
        NodeId node = kInvalidNode;
        int maximum_pan = 0;
        if (!current_block_is_wide(node, maximum_pan)) return false;
        enter_wide_focus(node, maximum_pan);
    }
    pan_x_ += pan_delta;
    return true;
}

bool Viewer::activate_link(std::uint32_t link_id) {
    if (markdown_document_ == nullptr ||
        link_id >= markdown_document_->ir.links.size()) return false;
    const LinkRecord& link = markdown_document_->ir.links[link_id];
    const std::string_view target = markdown_document_->text(link.target);
    if (target.empty()) return true;
    if (target.front() == '#') {
        if (!navigate_to_anchor(target.substr(1))) {
            show_message("Link target not found", std::string(target));
        }
    } else if (has_uri_scheme(target)) {
        show_message("External link", std::string(target));
    } else {
        pending_document_link_.assign(target.data(), target.size());
    }
    return true;
}

bool Viewer::activate_current_link() {
    if (markdown_document_ == nullptr || markdown_layout_.unit_count() == 0) return false;
    const std::size_t unit = markdown_layout_.unit_at(fx_from_int(scroll_y_));
    const BlockLayout* layout = markdown_layout_.layout_unit(unit);
    if (layout == nullptr) return false;
    std::vector<std::uint32_t> links;
    for (const LayoutLine& line : layout->lines) {
        for (const LayoutRun& run : line.runs) {
            if ((run.style_flags & InlineStyleLink) == 0 ||
                run.link_id == kInvalidToken ||
                run.link_id >= markdown_document_->ir.links.size() ||
                std::find(links.begin(), links.end(), run.link_id) != links.end()) {
                continue;
            }
            links.push_back(run.link_id);
        }
    }
    if (links.empty()) return false;
    if (links.size() == 1) return activate_link(links.front());

    link_choice_mode_ = true;
    link_choice_ids_ = std::move(links);
    link_choice_runs_.clear();
    link_choice_runs_.reserve(link_choice_ids_.size());
    link_choice_selected_ = 0;
    link_title_ = {};
    constexpr char title[] = "Choose link";
    text_.shape(title, sizeof(title) - 1,
                fx_from_int(kMenuTitlePixelSize), link_title_);
    for (std::size_t index = 0; index < link_choice_ids_.size(); ++index) {
        const LinkRecord& link = markdown_document_->ir.links[link_choice_ids_[index]];
        const std::string_view target = markdown_document_->text(link.target);
        const std::string label = std::to_string(index + 1) + " · " +
                                  std::string(target);
        GlyphRun run;
        text_.shape(label.data(), label.size(),
                    fx_from_int(kMenuCompactPixelSize), run);
        link_choice_runs_.push_back(std::move(run));
    }
    overlay_open_ = true;
    link_overlay_ = true;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    document_browser_overlay_ = false;
    font_browser_overlay_ = false;
    dirty_ = true;
    return true;
}

bool Viewer::take_document_link_request(std::string& target) {
    if (pending_document_link_.empty()) return false;
    target = std::move(pending_document_link_);
    pending_document_link_.clear();
    return true;
}

bool Viewer::navigate_to_anchor(std::string_view fragment) {
    if (markdown_document_ == nullptr) return false;
    const std::string decoded = percent_decode(fragment);
    const std::string requested = heading_slug(decoded);
    for (const HeadingEntry& heading : markdown_document_->ir.headings) {
        const std::string_view title = markdown_document_->text(heading.title);
        if (heading_slug(title) != requested && title != decoded) continue;
        invalidate_retained_base_frame();
        scroll_y_ = fx_floor(markdown_layout_.position_for_source(
            heading.block, heading.source_offset, 0));
        clamp_view();
        dirty_ = true;
        return true;
    }
    return false;
}

void Viewer::reshape_message_dialog_runs() {
    link_title_ = {};
    link_target_runs_.clear();
    link_hint_run_ = {};
    if (!text_ready_) return;
    text_.shape(link_dialog_title_.data(), link_dialog_title_.size(),
                fx_from_int(kMenuTitlePixelSize), link_title_);

    // The body box is 256 px wide inside the 272 px message panel; its text
    // sits at symmetric 5 px insets across at most two wrapped lines.
    constexpr int kMessageBodyWidthPx = 246;
    constexpr std::size_t kMessageBodyLines = 2;
    link_target_runs_ = wrap_text_runs(
        text_, link_dialog_target_, fx_from_int(kMenuCompactPixelSize),
        kMessageBodyWidthPx, kMessageBodyLines);

    constexpr char kPromptHint[] = "Enter opens Fonts, Esc continues";
    constexpr char kCloseHint[] = "Enter or Esc closes";
    if (message_confirm_opens_font_menu_) {
        text_.shape(kPromptHint, sizeof(kPromptHint) - 1,
                    fx_from_int(kMenuAuxiliaryPixelSize), link_hint_run_);
    } else {
        text_.shape(kCloseHint, sizeof(kCloseHint) - 1,
                    fx_from_int(kMenuAuxiliaryPixelSize), link_hint_run_);
    }
}

void Viewer::show_message(std::string title, std::string message) {
    commit_settings_session();
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    link_choice_selected_ = 0;
    link_dialog_title_ = std::move(title);
    link_dialog_target_ = std::move(message);
    // Ordinary messages close on any key; only show_cjk_font_prompt() arms
    // the Enter-opens-fonts action, and the hint text follows that flag.
    message_confirm_opens_font_menu_ = false;
    reshape_message_dialog_runs();
    overlay_open_ = true;
    link_overlay_ = true;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    document_browser_overlay_ = false;
    font_browser_overlay_ = false;
    dirty_ = true;
}

void Viewer::show_cjk_font_prompt() {
    show_message("CJK font needed",
                 "This document contains CJK text without a font");
    message_confirm_opens_font_menu_ = true;
    reshape_message_dialog_runs();
}

void Viewer::show_loading_feedback(std::string title,
                                   std::string detail,
                                   int progress_percent) {
    const bool newly_visible = !loading_feedback_visible_;
    loading_feedback_visible_ = true;
    loading_feedback_progress_ =
        progress_percent < 0 ? -1 : std::min(100, progress_percent);
    if (newly_visible) {
        loading_feedback_painted_ = false;
        loading_feedback_phase_ = 0;
    } else {
        loading_feedback_phase_ =
            static_cast<std::uint8_t>((loading_feedback_phase_ + 1U) % 3U);
    }
    loading_title_run_ = {};
    loading_detail_run_ = {};
    if (text_ready_) {
        text_.shape(title.data(), title.size(), fx_from_int(kMenuTitlePixelSize),
                    loading_title_run_);
        // The card has room for one detail line; long font or document
        // labels are ellipsized instead of overflowing the panel.
        constexpr int kLoadingDetailWidthPx = 240;
        std::vector<GlyphRun> detail_runs = wrap_text_runs(
            text_, detail, fx_from_int(kMenuAuxiliaryPixelSize),
            kLoadingDetailWidthPx, 1);
        if (!detail_runs.empty()) {
            loading_detail_run_ = std::move(detail_runs.front());
        }
    }
    dirty_ = true;
}

void Viewer::clear_loading_feedback() {
    if (!loading_feedback_visible_) return;
    loading_feedback_visible_ = false;
    loading_feedback_painted_ = false;
    loading_feedback_phase_ = 0;
    loading_feedback_progress_ = -1;
    loading_title_run_ = {};
    loading_detail_run_ = {};
    dirty_ = true;
}

bool Viewer::set_font_registry(
    FontRegistryState registry,
    const std::array<std::string, kExternalFontRoleCount>& labels,
    std::string& error) {
    const FontRegistryState previous_registry = text_.font_registry();
    const auto previous_labels = active_font_labels_;
    const std::uint32_t previous_signature = font_pack_signature_;
    const bool had_document = markdown_document_ != nullptr;
    const bool had_plain_text = plain_text_layout_.loaded();
    const std::uint32_t plain_text_offset =
        plain_text_layout_.current_source_offset();
    const ViewAnchor anchor = had_document
                                  ? markdown_layout_.anchor_at(
                                        fx_from_int(scroll_y_))
                                  : ViewAnchor{};
    FontRegistryState discarded;
    if (!text_.replace_font_registry(std::move(registry), discarded, error)) {
        text_ready_ = text_.ready();
        return false;
    }
    text_ready_ = true;
    active_font_labels_ = labels;
    font_pack_signature_ = text_.font_signature();
    if (font_pack_signature_ == 0) font_pack_signature_ = 1;
    math_.clear_cache();
    rebuild_text_runs();
    rebuild_toc_runs();
    rebuild_bookmark_runs();
    rebuild_search_runs();
    rebuild_settings_runs();
    rebuild_diagnostics_runs();
    rebuild_document_browser_runs();
    rebuild_font_browser_runs();
    if (had_document && !markdown_layout_.reconfigure(layout_signature(), error)) {
        const std::string reflow_error = error;
        std::string restore_error;
        FontRegistryState ignored;
        text_.replace_font_registry(previous_registry, ignored, restore_error);
        text_ready_ = text_.ready();
        active_font_labels_ = previous_labels;
        font_pack_signature_ = previous_signature;
        rebuild_text_runs();
        rebuild_toc_runs();
        rebuild_bookmark_runs();
        rebuild_search_runs();
        rebuild_settings_runs();
        rebuild_diagnostics_runs();
        rebuild_document_browser_runs();
        rebuild_font_browser_runs();
        markdown_layout_.reconfigure(layout_signature(), restore_error);
        error = reflow_error;
        return false;
    }
    if (had_plain_text &&
        (!plain_text_layout_.reconfigure(layout_signature(), error) ||
         !plain_text_layout_.seek_source(plain_text_offset, error))) {
        const std::string reflow_error = error;
        std::string restore_error;
        FontRegistryState ignored;
        text_.replace_font_registry(previous_registry, ignored, restore_error);
        text_ready_ = text_.ready();
        active_font_labels_ = previous_labels;
        font_pack_signature_ = previous_signature;
        rebuild_text_runs();
        rebuild_bookmark_runs();
        rebuild_search_runs();
        rebuild_settings_runs();
        rebuild_diagnostics_runs();
        rebuild_document_browser_runs();
        rebuild_font_browser_runs();
        plain_text_layout_.reconfigure(layout_signature(), restore_error);
        plain_text_layout_.seek_source(plain_text_offset, restore_error);
        error = reflow_error;
        return false;
    }
    if (had_plain_text) {
        prepare_plain_text_cache(plain_text_layout_);
    }
    if (had_document) {
        document_height_ = std::max(kViewportHeight,
                                    fx_ceil(markdown_layout_.total_height()));
        scroll_y_ = fx_floor(markdown_layout_.position_of(anchor));
        wide_focus_ = false;
        focused_node_ = kInvalidNode;
        focused_maximum_pan_ = 0;
        focused_code_layout_ = {};
        focused_code_layout_valid_ = false;
        pan_x_ = 0;
        clamp_view();
    }
    invalidate_retained_base_frame();
    dirty_ = true;
    return true;
}

bool Viewer::set_font(FontRole role,
                      std::vector<std::uint8_t> font,
                      std::string label,
                      std::string& error) {
    std::vector<ExternalFontUpdate> updates;
    updates.push_back({role, std::move(font)});
    return set_fonts(std::move(updates), {std::move(label)}, error);
}

bool Viewer::set_fonts(std::vector<ExternalFontUpdate> fonts,
                       const std::vector<std::string>& labels,
                       std::string& error) {
    if (fonts.empty() || labels.size() != fonts.size()) {
        error = "font update is empty or has invalid labels";
        return false;
    }
    std::vector<FontRole> roles;
    std::vector<bool> clearing;
    std::vector<std::string> previous_labels;
    roles.reserve(fonts.size());
    clearing.reserve(fonts.size());
    previous_labels.reserve(fonts.size());
    for (const ExternalFontUpdate& font : fonts) {
        const int slot = font_slot_index(font.role);
        if (slot < 0) {
            error = "unsupported font role";
            return false;
        }
        roles.push_back(font.role);
        clearing.push_back(font.data.empty());
        previous_labels.push_back(active_font_labels_[slot]);
    }
    std::vector<ExternalFontUpdate> previous;
    const std::uint32_t previous_signature = font_pack_signature_;
    const bool had_document = markdown_document_ != nullptr;
    const bool had_plain_text = plain_text_layout_.loaded();
    const std::uint32_t plain_text_offset =
        plain_text_layout_.current_source_offset();
    const ViewAnchor anchor = had_document
                                  ? markdown_layout_.anchor_at(fx_from_int(scroll_y_))
                                  : ViewAnchor{};
    if (!text_.replace_external_fonts(std::move(fonts), previous, error)) {
        text_ready_ = text_.ready();
        return false;
    }
    text_ready_ = true;

    for (std::size_t index = 0; index < roles.size(); ++index) {
        const int slot = font_slot_index(roles[index]);
        active_font_labels_[slot] =
            clearing[index]
                ? default_font_label(roles[index])
                : (labels[index].empty() ? "Custom" : labels[index]);
    }
    font_pack_signature_ = text_.font_signature();
    if (font_pack_signature_ == 0) font_pack_signature_ = 1;
    math_.clear_cache();
    rebuild_text_runs();
    rebuild_toc_runs();
    rebuild_bookmark_runs();
    rebuild_search_runs();
    rebuild_settings_runs();
    rebuild_diagnostics_runs();
    rebuild_document_browser_runs();
    rebuild_font_browser_runs();
    if (had_document && !markdown_layout_.reconfigure(layout_signature(), error)) {
        const std::string reflow_error = error;
        std::string restore_error;
        std::vector<ExternalFontUpdate> discarded;
        text_.replace_external_fonts(std::move(previous), discarded,
                                     restore_error);
        text_ready_ = text_.ready();
        for (std::size_t index = 0; index < roles.size(); ++index) {
            active_font_labels_[font_slot_index(roles[index])] =
                previous_labels[index];
        }
        font_pack_signature_ = previous_signature;
        rebuild_text_runs();
        rebuild_toc_runs();
        rebuild_bookmark_runs();
        rebuild_search_runs();
        rebuild_settings_runs();
        markdown_layout_.reconfigure(layout_signature(), restore_error);
        error = reflow_error;
        return false;
    }
    if (had_plain_text &&
        (!plain_text_layout_.reconfigure(layout_signature(), error) ||
         !plain_text_layout_.seek_source(plain_text_offset, error))) {
        const std::string reflow_error = error;
        std::string restore_error;
        std::vector<ExternalFontUpdate> discarded;
        text_.replace_external_fonts(std::move(previous), discarded,
                                     restore_error);
        text_ready_ = text_.ready();
        for (std::size_t index = 0; index < roles.size(); ++index) {
            active_font_labels_[font_slot_index(roles[index])] =
                previous_labels[index];
        }
        font_pack_signature_ = previous_signature;
        rebuild_text_runs();
        rebuild_bookmark_runs();
        rebuild_search_runs();
        rebuild_settings_runs();
        plain_text_layout_.reconfigure(layout_signature(), restore_error);
        plain_text_layout_.seek_source(plain_text_offset, restore_error);
        error = reflow_error;
        return false;
    }
    if (had_plain_text) {
        prepare_plain_text_cache(plain_text_layout_);
    }
    if (had_document) {
        document_height_ = std::max(kViewportHeight,
                                    fx_ceil(markdown_layout_.total_height()));
        scroll_y_ = fx_floor(markdown_layout_.position_of(anchor));
        wide_focus_ = false;
        focused_node_ = kInvalidNode;
        focused_maximum_pan_ = 0;
        focused_code_layout_ = {};
        focused_code_layout_valid_ = false;
        pan_x_ = 0;
        clamp_view();
    }
    invalidate_retained_base_frame();
    dirty_ = true;
    return true;
}

int Viewer::block_stride() const {
    return std::max(96, body_pixel_size_ * 5 + 26);
}

int Viewer::content_width() const {
    return std::max(32, kPageWidth - side_margin_px_ * 2);
}

void Viewer::set_document(const DocumentProbe& document) {
    invalidate_retained_base_frame();
    markdown_layout_.clear();
    markdown_document_.reset();
    plain_text_layout_.clear();
    document_ = document;
    document_loaded_ = true;
    document_error_ = false;
    overlay_open_ = false;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    document_browser_overlay_ = false;
    font_browser_overlay_ = false;
    link_overlay_ = false;
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    pending_document_link_.clear();
    pending_document_browser_request_ = false;
    pending_state_save_request_ = false;
    pending_document_open_.clear();
    toc_runs_.clear();
    search_results_.clear();
    search_result_runs_.clear();
    has_active_search_match_ = false;
    bookmarks_.clear();
    bookmark_runs_.clear();
    const std::uint64_t estimated = 720U + document.size / 3U;
    const std::uint64_t scaled = estimated * static_cast<unsigned>(body_pixel_size_) / 15U;
    document_height_ = static_cast<int>(std::min<std::uint64_t>(12000U,
                                                                 std::max<std::uint64_t>(720U, scaled)));
    scroll_y_ = 0;
    screen_step_history_.clear();
    line_step_history_.clear();
    pan_x_ = 0;
    wide_focus_ = false;
    focused_node_ = kInvalidNode;
    focused_maximum_pan_ = 0;
    focused_code_layout_ = {};
    focused_code_layout_valid_ = false;
    pending_final_page_restore_ = false;
    dirty_ = true;
}

LayoutSignature Viewer::layout_signature() const {
    LayoutSignature signature;
    signature.content_width = static_cast<std::uint16_t>(content_width());
    signature.body_px = static_cast<std::uint16_t>(body_pixel_size_);
    signature.line_height_px = line_gap_px_ < 0
                                   ? 0
                                   : static_cast<std::uint16_t>(body_pixel_size_ +
                                                                line_gap_px_);
    signature.code_wrap = code_wrap_ ? 1 : 0;
    signature.table_mode = table_mode_;
    signature.font_pack_version = font_pack_signature_;
    return signature;
}

void Viewer::set_document_title(std::string title) {
    invalidate_retained_base_frame();
    document_title_ = std::move(title);
    if (document_title_.empty()) document_title_ = default_document_title_;
    if (text_ready_) {
        chrome_title_max_width_ = -1;
        rebuild_chrome_title(kScreenWidth - kChromeX - kChromeRightPadding);
    }
    dirty_ = true;
}

bool Viewer::set_markdown_document(std::unique_ptr<MarkdownDocument> document,
                                   const DocumentProbe& probe,
                                   std::string& error) {
    error.clear();
    if (document == nullptr) {
        error = "parsed Markdown document is missing";
        return false;
    }
    invalidate_retained_base_frame();
    plain_text_layout_.clear();
    markdown_layout_.clear();
    markdown_document_ = std::move(document);
    if (!markdown_layout_.initialize(*markdown_document_, text_, layout_signature(),
                                     error, &math_)) {
        markdown_document_.reset();
        return false;
    }
    document_ = probe;
    document_loaded_ = true;
    document_error_ = false;
    document_error_message_.clear();
    overlay_open_ = false;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    document_browser_overlay_ = false;
    font_browser_overlay_ = false;
    link_overlay_ = false;
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    pending_document_link_.clear();
    pending_document_browser_request_ = false;
    pending_state_save_request_ = false;
    pending_document_open_.clear();
    bookmarks_.clear();
    toc_selected_ = 0;
    rebuild_toc_runs();
    rebuild_bookmark_runs();
    rebuild_settings_runs();
    rebuild_diagnostics_runs();
    search_results_.clear();
    search_result_runs_.clear();
    has_active_search_match_ = false;
    document_height_ = std::max(kViewportHeight,
                                fx_ceil(markdown_layout_.total_height()));
    scroll_y_ = 0;
    screen_step_history_.clear();
    line_step_history_.clear();
    pan_x_ = 0;
    wide_focus_ = false;
    focused_node_ = kInvalidNode;
    focused_maximum_pan_ = 0;
    focused_code_layout_ = {};
    focused_code_layout_valid_ = false;
    pending_final_page_restore_ = false;
    chrome_visible_ = true;
    if (!cjk_font_hint_shown_ &&
        text_.external_font_id(FontRole::Cjk) == 0 &&
        [&]() {
            const std::size_t chunk_count =
                markdown_document_->source_chunk_count();
            for (std::size_t index = 0; index < chunk_count; ++index) {
                if (contains_cjk_text(markdown_document_->source_chunk(index))) {
                    return true;
                }
            }
            return false;
        }()) {
        cjk_font_hint_shown_ = true;
        show_cjk_font_prompt();
    }
    dirty_ = true;
    return true;
}

bool Viewer::set_plain_text_document(
    std::shared_ptr<RandomAccessData> source,
    std::uint32_t source_offset,
    std::uint32_t source_size,
    const Utf8ValidationResult& sampled_validation,
    const DocumentProbe& probe,
    std::string& error) {
    error.clear();
    PlainTextLayout next_layout;
    if (!next_layout.initialize(
            std::move(source), source_offset, source_size, sampled_validation,
            text_, layout_signature(), kViewportHeight, error)) {
        return false;
    }
    prepare_plain_text_cache(next_layout);
    invalidate_retained_base_frame();
    markdown_layout_.clear();
    markdown_document_.reset();
    plain_text_layout_ = std::move(next_layout);

    document_ = probe;
    document_loaded_ = true;
    document_error_ = false;
    document_error_message_.clear();
    overlay_open_ = false;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    document_browser_overlay_ = false;
    font_browser_overlay_ = false;
    link_overlay_ = false;
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    pending_document_link_.clear();
    pending_document_browser_request_ = false;
    pending_state_save_request_ = false;
    pending_document_open_.clear();
    bookmarks_.clear();
    toc_selected_ = 0;
    toc_runs_.clear();
    rebuild_bookmark_runs();
    rebuild_settings_runs();
    rebuild_diagnostics_runs();
    search_results_.clear();
    search_result_runs_.clear();
    has_active_search_match_ = false;
    document_height_ = kViewportHeight;
    scroll_y_ = 0;
    screen_step_history_.clear();
    line_step_history_.clear();
    pan_x_ = 0;
    wide_focus_ = false;
    focused_node_ = kInvalidNode;
    focused_maximum_pan_ = 0;
    focused_code_layout_ = {};
    focused_code_layout_valid_ = false;
    pending_final_page_restore_ = false;
    chrome_visible_ = true;
    if (!cjk_font_hint_shown_ &&
        text_.external_font_id(FontRole::Cjk) == 0 &&
        plain_text_layout_.initial_cache_contains_cjk()) {
        cjk_font_hint_shown_ = true;
        show_cjk_font_prompt();
    }
    dirty_ = true;
    return true;
}

void Viewer::prepare_plain_text_cache(PlainTextLayout& layout) {
    // Initial layout already keeps every screen produced by the first
    // HarfBuzz paragraph pass. Do not synchronously fill the complete future
    // window or rasterize off-screen glyphs here: both delayed the first
    // visible frame enough to look like a frozen launch. Idle work resumes
    // those optimizations after the first presentation.
    (void)layout;
}

void Viewer::set_document_error(std::string message) {
    invalidate_retained_base_frame();
    markdown_layout_.clear();
    markdown_document_.reset();
    plain_text_layout_.clear();
    document_loaded_ = false;
    document_error_ = true;
    document_error_message_ = std::move(message);
    overlay_open_ = false;
    toc_overlay_ = false;
    search_overlay_ = false;
    jump_overlay_ = false;
    settings_overlay_ = false;
    diagnostics_overlay_ = false;
    document_browser_overlay_ = false;
    font_browser_overlay_ = false;
    link_overlay_ = false;
    link_choice_mode_ = false;
    link_choice_ids_.clear();
    link_choice_runs_.clear();
    pending_state_save_request_ = false;
    toc_runs_.clear();
    search_results_.clear();
    search_result_runs_.clear();
    has_active_search_match_ = false;
    document_height_ = 720 * body_pixel_size_ / 15;
    scroll_y_ = 0;
    screen_step_history_.clear();
    line_step_history_.clear();
    pan_x_ = 0;
    wide_focus_ = false;
    focused_node_ = kInvalidNode;
    focused_maximum_pan_ = 0;
    focused_code_layout_ = {};
    focused_code_layout_valid_ = false;
    pending_final_page_restore_ = false;
    document_error_message_runs_.clear();
    if (text_ready_ && !document_error_message_.empty()) {
        document_error_message_runs_ = wrap_text_runs(
            text_, document_error_message_, fx_from_int(11),
            kScreenWidth - 24, 2);
    }
    dirty_ = true;
}

void Viewer::set_reading_mode(ReadingMode mode) {
    const ReadingMode normalized = mode == ReadingMode::HorizontalScroll
                                       ? ReadingMode::HorizontalScroll
                                       : ReadingMode::VerticalScroll;
    if (reading_mode_ == normalized) return;
    invalidate_retained_base_frame();
    reading_mode_ = normalized;
    clamp_view();
    rebuild_settings_runs();
    dirty_ = true;
}

bool Viewer::apply_reader_state(const ReaderState& state,
                                std::uint64_t identity) {
    if ((markdown_document_ == nullptr && !plain_text_layout_.loaded()) ||
        state.position.document_identity != identity) {
        return false;
    }
    invalidate_retained_base_frame();
    dark_theme_ = state.dark_theme;
    high_contrast_ = state.high_contrast;
    code_wrap_ = state.code_wrap;
    table_mode_ = state.table_mode > 1 ? 0 : state.table_mode;
    reading_mode_ = state.reading_mode == ReadingMode::HorizontalScroll
                        ? ReadingMode::HorizontalScroll
                        : ReadingMode::VerticalScroll;
    natural_scrolling_ = state.natural_scrolling;
    natural_swiping_ = state.natural_swiping;
    render_sharpness_ = clamp_render_sharpness(state.render_sharpness);
    text_.set_render_sharpness(render_sharpness_);
    body_pixel_size_ = std::max(12, std::min(22, static_cast<int>(state.font_size)));
    line_gap_px_ = state.line_gap < 0 ? -1 : std::min(10, state.line_gap);
    side_margin_px_ = std::max(2, std::min(18, static_cast<int>(state.side_margin)));
    bookmarks_ = state.bookmarks;
    toc_selected_ = toc_runs_.empty()
                        ? 0
                        : std::min<std::size_t>(state.last_selected_heading,
                                                toc_runs_.size() - 1);
    text_.clear_cache();
    rebuild_text_runs();
    rebuild_toc_runs();
    rebuild_bookmark_runs();
    rebuild_settings_runs();
    std::string error;
    if (markdown_document_ != nullptr) {
        if (!markdown_layout_.reconfigure(layout_signature(), error)) {
            return false;
        }
    } else if (!plain_text_layout_.reconfigure(layout_signature(), error)) {
        return false;
    }
    wide_focus_ = false;
    focused_node_ = kInvalidNode;
    focused_maximum_pan_ = 0;
    focused_code_layout_ = {};
    focused_code_layout_valid_ = false;
    pan_x_ = 0;
    if (plain_text_layout_.loaded()) {
        if (!plain_text_layout_.seek_source(
                state.position.source_offset, error)) {
            return false;
        }
        prepare_plain_text_cache(plain_text_layout_);
        scroll_y_ = 0;
    } else {
        scroll_y_ = fx_floor(markdown_layout_.position_for_source(
            state.position.nearest_block,
            state.position.source_offset,
            state.position.relative_position_0_65535));
    }
    screen_step_history_.clear();
    line_step_history_.clear();
    document_height_ = std::max(kViewportHeight,
                                fx_ceil(markdown_layout_.total_height()));
    pending_final_page_restore_ =
        state.position.relative_position_0_65535 == 65535;
    // A persisted semantic anchor is more accurate than synthetic page
    // geometry. Keep it visible; re-snapping through estimated heights can move
    // the viewport into an empty tail before surrounding blocks are measured.
    clamp_view();
    dirty_ = true;
    return true;
}

ReaderState Viewer::reader_state(std::uint64_t identity) const {
    ReaderState state;
    state.position.document_identity = identity;
    state.font_size = static_cast<std::uint8_t>(body_pixel_size_);
    state.line_gap = line_gap_px_;
    state.side_margin = static_cast<std::uint8_t>(side_margin_px_);
    state.dark_theme = dark_theme_;
    state.high_contrast = high_contrast_;
    state.code_wrap = code_wrap_;
    state.table_mode = table_mode_;
    state.reading_mode = reading_mode_;
    state.natural_scrolling = natural_scrolling_;
    state.natural_swiping = natural_swiping_;
    state.render_sharpness = render_sharpness_;
    state.bookmarks = bookmarks_;
    state.last_selected_heading = static_cast<std::uint32_t>(toc_selected_);
    if (markdown_document_ != nullptr && markdown_layout_.unit_count() != 0) {
        if (scroll_y_ >= max_scroll_y()) {
            const NodeId last = markdown_layout_.unit_node(
                markdown_layout_.unit_count() - 1);
            if (last != kInvalidNode && last < markdown_document_->ir.blocks.size()) {
                state.position.nearest_block = last;
                state.position.source_offset =
                    markdown_document_->ir.blocks[last].source_offset;
                state.position.relative_position_0_65535 = 65535;
                return state;
            }
        }
        const Fx position = fx_from_int(scroll_y_);
        const ViewAnchor anchor = markdown_layout_.anchor_at(position);
        state.position.nearest_block = anchor.block;
        state.position.source_offset = anchor.source_offset;
        const std::size_t unit = markdown_layout_.unit_at(position);
        const Fx height = markdown_layout_.unit_height(unit);
        if (height > 0) {
            state.position.relative_position_0_65535 =
                static_cast<std::uint16_t>(std::min<std::int64_t>(
                    65535,
                    static_cast<std::int64_t>(std::max<Fx>(0, anchor.local_y_26_6)) *
                        65535 / height));
        }
    } else if (plain_text_layout_.loaded()) {
        state.position.nearest_block = kInvalidNode;
        state.position.source_offset =
            plain_text_layout_.current_source_offset();
        state.position.relative_position_0_65535 =
            plain_text_layout_.at_end() ? 65535 : 0;
    }
    return state;
}

int Viewer::max_scroll_y() const {
    if (plain_text_layout_.loaded()) return 0;
    return std::max(0, document_height_ - kViewportHeight);
}

int Viewer::total_pages() const {
    if (plain_text_layout_.loaded()) {
        // Zero explicitly means unknown: TXT deliberately avoids an exact
        // document-wide page count.
        return 0;
    }
    int pages = 1 + std::max(0, document_height_ - 1) / kViewportHeight;
    // A tiny estimated/layout tail must not become a nearly blank page. Once
    // there are already two full pages, absorb less than one text line into a
    // bottom-aligned final page.
    const int tail = document_height_ % kViewportHeight;
    const int minimum_tail = std::max(18, body_pixel_size_ +
        (line_gap_px_ < 0 ? 3 : line_gap_px_));
    if (pages > 2 && tail > 0 && tail < minimum_tail) --pages;
    return pages;
}

int Viewer::current_page() const {
    if (plain_text_layout_.loaded()) {
        return plain_text_layout_.approximate_current_page();
    }
    const int page_count = total_pages();
    if (reading_mode_ == ReadingMode::HorizontalScroll) {
        if (page_count > 1 && scroll_y_ >= max_scroll_y()) return page_count;
        // The final page number is also the 100% reading position. Do not
        // round a near-tail, line-aligned viewport up to it before the actual
        // document end has been reached.
        // A two-position document conventionally labels the first forward
        // turn as page 2 even when line overlap leaves a few tail pixels for
        // one more key press. Longer documents reserve their final number for
        // the actual end so navigation loops cannot stop inside tall content.
        const int nonfinal_maximum = page_count <= 2
                                         ? page_count
                                         : page_count - 1;
        return std::max(1, std::min(nonfinal_maximum,
                                    1 + (scroll_y_ + kViewportHeight / 2) /
                                            kViewportHeight));
    }
    const int maximum_scroll = max_scroll_y();
    if (maximum_scroll == 0 || page_count == 1) {
        return 1;
    }
    const std::int64_t scaled =
        static_cast<std::int64_t>(scroll_y_) * (page_count - 1);
    return std::max(1,
                    std::min(page_count,
                             1 + static_cast<int>((scaled + maximum_scroll / 2) /
                                                  maximum_scroll)));
}

int Viewer::reading_progress_width() const {
    // Errors are a status state, not a reading position. Keep their red alert
    // visible across the full-width strip even though no document is loaded.
    if (document_error_) return kScreenWidth;
    if (!document_loaded_) return 0;

    if (plain_text_layout_.loaded()) {
        const std::uint32_t source_size = plain_text_layout_.source_size();
        if (source_size == 0) return 0;
        if (plain_text_layout_.at_end()) return kScreenWidth;
        return static_cast<int>(
            static_cast<std::uint64_t>(kScreenWidth) *
            plain_text_layout_.current_source_offset() / source_size);
    }

    // Both touchpad layouts navigate the same continuous reflowed document.
    // Progress therefore follows the viewport position and never depends on
    // an unstable synthetic page count.
    const int maximum = max_scroll_y();
    if (maximum <= 0) return 0;
    const int position = std::max(0, std::min(maximum, scroll_y_));

    // Floor the pixel extent so a full-width bar is reserved for the exact
    // end position. Both modes therefore map start -> 0 and end -> 320.
    return static_cast<int>(static_cast<std::int64_t>(kScreenWidth) * position /
                            maximum);
}

bool Viewer::current_markdown_view_is_text_only() {
    if (markdown_document_ == nullptr || markdown_layout_.unit_count() == 0) {
        return false;
    }

    // Edge alignment is intentionally a text-only affordance. Formula
    // boxes, rules, task controls, and structured table rows have their own
    // geometry; a line-derived target could jump through or bisect them. The
    // scan is bounded to the current viewport and does not build a global
    // index, which keeps very large documents responsive.
    const ViewAnchor old_anchor =
        markdown_layout_.anchor_at(fx_from_int(scroll_y_));
    const std::vector<VisibleBlock> current_view =
        markdown_layout_.layout_window(fx_from_int(scroll_y_),
                                       fx_from_int(kViewportHeight), 0);
    document_height_ = std::max(
        kViewportHeight, fx_ceil(markdown_layout_.total_height()));
    scroll_y_ = fx_floor(markdown_layout_.position_of(old_anchor));
    clamp_view();
    bool text_only = !current_view.empty();
    for (const VisibleBlock& visible : current_view) {
        const BlockLayout* block = visible.layout;
        if (block == nullptr ||
            (block->kind != BlockKind::Paragraph &&
             block->kind != BlockKind::Heading &&
             block->kind != BlockKind::CodeBlock)) {
            text_only = false;
            break;
        }
        for (const LayoutLine& line : block->lines) {
            const int block_top = fx_floor(visible.document_y);
            const Fx descent = line.descent < 0 ? -line.descent
                                                : line.descent;
            const int line_top = block_top + fx_floor(line.baseline_y) -
                                 fx_ceil(line.ascent);
            const int line_bottom = block_top + fx_floor(line.baseline_y) +
                                    fx_ceil(descent);
            if (line_bottom <= scroll_y_ ||
                line_top >= scroll_y_ + kViewportHeight) {
                continue;
            }
            for (const LayoutRun& run : line.runs) {
                if (run.math != nullptr || run.task_checkbox) {
                    text_only = false;
                    break;
                }
            }
            if (!text_only) break;
        }
        if (!text_only) break;
    }
    return text_only;
}

int Viewer::previous_line_scroll_y() {
    if (scroll_y_ <= 0) return scroll_y_;
    if (!current_markdown_view_is_text_only()) {
        return std::max(0, scroll_y_ - kLineScroll);
    }

    // If the top row is clipped, its own top is the preceding boundary. If it
    // is already aligned, resolve one pixel above it to select the prior row.
    // Either way Scroll Up finishes with a complete row flush at the top.
    const int nominal = std::max(0, scroll_y_ - 1);
    const int target = aligned_scroll_y_near(nominal, false);
    if (target == nominal && scroll_y_ <= kLineScroll) return 0;
    if (target >= scroll_y_) {
        return std::max(0, scroll_y_ - kLineScroll);
    }
    return target;
}

int Viewer::next_line_scroll_y() {
    int maximum = max_scroll_y();
    if (scroll_y_ >= maximum) return scroll_y_;
    if (!current_markdown_view_is_text_only()) {
        return std::min(maximum, scroll_y_ + kLineScroll);
    }

    maximum = max_scroll_y();
    if (scroll_y_ >= maximum) {
        return std::min(maximum, scroll_y_ + kLineScroll);
    }

    const int viewport_bottom = scroll_y_ + kViewportHeight;
    const auto line_bounds = [this](std::size_t unit,
                                    const LayoutLine& line,
                                    int& top,
                                    int& bottom) {
        const int block_top = fx_floor(markdown_layout_.unit_top(unit));
        top = block_top + fx_floor(line.baseline_y) - fx_ceil(line.ascent);
        const Fx descent = line.descent < 0 ? -line.descent : line.descent;
        bottom = block_top + fx_floor(line.baseline_y) + fx_ceil(descent);
        if (bottom <= top) bottom = top + 1;
    };

    // Lazy block measurement can move the unit that owns the old viewport
    // boundary. Re-resolve locally until the boundary and its line geometry
    // come from the same height table.
    for (int pass = 0; pass < 3; ++pass) {
        std::size_t unit =
            markdown_layout_.unit_at(fx_from_int(viewport_bottom));
        markdown_layout_.layout_unit(unit);
        document_height_ = std::max(
            kViewportHeight, fx_ceil(markdown_layout_.total_height()));
        const std::size_t resolved =
            markdown_layout_.unit_at(fx_from_int(viewport_bottom));
        if (resolved != unit) continue;

        for (std::size_t scanned = 0;
             unit < markdown_layout_.unit_count() && scanned < 64;
             ++unit, ++scanned) {
            const BlockLayout* block = markdown_layout_.layout_unit(unit);
            document_height_ = std::max(
                kViewportHeight, fx_ceil(markdown_layout_.total_height()));
            if (block == nullptr) continue;
            for (const LayoutLine& line : block->lines) {
                int top = 0;
                int bottom = 0;
                line_bounds(unit, line, top, bottom);
                if (bottom <= viewport_bottom) continue;

                // A formula or other single visual row taller than the whole
                // viewport cannot be made fully visible. Preserve incremental
                // progress through it instead of skipping to its bottom.
                if (bottom - top > kViewportHeight) {
                    return std::min(max_scroll_y(),
                                    scroll_y_ + kLineScroll);
                }

                // Place the first not-yet-complete visual line flush with the
                // bottom edge. This makes key 2 reveal one complete last line
                // even when font metrics or spacing are not 18 pixels.
                const int target = bottom - kViewportHeight;
                const int current_maximum = max_scroll_y();
                if (current_maximum <= scroll_y_) return current_maximum;
                return std::max(scroll_y_ + 1,
                                std::min(current_maximum, target));
            }
        }
        break;
    }
    return std::min(max_scroll_y(), scroll_y_ + kLineScroll);
}

void Viewer::move_markdown_line(int direction) {
    if (direction == 0) return;
    line_step_event_ = true;
    const int previous = scroll_y_;

    if (direction < 0) {
        line_step_history_.clear();
        scroll_y_ = previous_line_scroll_y();
        return;
    }

    scroll_y_ = next_line_scroll_y();
    if (scroll_y_ > previous) {
        if (line_step_history_.size() >= kMaximumScreenStepHistory) {
            line_step_history_.erase(line_step_history_.begin());
        }
        line_step_history_.push_back({previous, scroll_y_});
    }
}

bool Viewer::jump_to_percentage(unsigned percentage) {
    if (!plain_text_layout_.loaded()) return false;
    std::string error;
    if (!plain_text_layout_.seek_percentage(percentage, error)) {
        if (!error.empty()) show_message("TXT read error", error);
        return false;
    }
    invalidate_retained_base_frame();
    has_active_search_match_ = false;
    chrome_visible_ = true;
    dirty_ = true;
    return true;
}

int Viewer::aligned_scroll_y_near(int nominal, bool forward) {
    const int maximum = max_scroll_y();
    nominal = std::max(0, std::min(nominal, maximum));
    if (nominal == 0 || markdown_document_ == nullptr ||
        markdown_layout_.unit_count() == 0) {
        return nominal;
    }

    const auto line_bounds = [this](std::size_t unit,
                                    const LayoutLine& line,
                                    int& top,
                                    int& bottom) {
        const int block_top = fx_floor(markdown_layout_.unit_top(unit));
        top = block_top + fx_floor(line.baseline_y) - fx_ceil(line.ascent);
        const Fx descent = line.descent < 0 ? -line.descent : line.descent;
        bottom = block_top + fx_floor(line.baseline_y) + fx_ceil(descent);
        if (bottom <= top) bottom = top + 1;
    };

    // Measuring a candidate can replace its estimate and therefore change
    // which unit contains the nominal boundary. Re-resolve a few times; every
    // pass still touches only the local page boundary, never the whole file.
    for (int pass = 0; pass < 3; ++pass) {
        std::size_t unit = markdown_layout_.unit_at(fx_from_int(nominal));
        markdown_layout_.layout_unit(unit);
        const std::size_t resolved_unit =
            markdown_layout_.unit_at(fx_from_int(nominal));
        if (resolved_unit != unit) continue;
        if (forward) {
            for (std::size_t scanned = 0;
                 unit < markdown_layout_.unit_count() && scanned < 64;
                 ++unit, ++scanned) {
                const BlockLayout* block = markdown_layout_.layout_unit(unit);
                if (block == nullptr) continue;
                for (const LayoutLine& line : block->lines) {
                    int top = 0;
                    int bottom = 0;
                    line_bounds(unit, line, top, bottom);
                    if (bottom <= nominal) continue;
                    const int line_height = std::max(1, bottom - top);
                    // A single formula line may be taller than the display.
                    // It cannot be made fully visible, so retain progress
                    // through it instead of snapping back to the same top.
                    if (line_height > kViewportHeight && top < nominal) {
                        return nominal;
                    }
                    if (top < nominal) {
                        const int visible_height = nominal - top;
                        // A trailing line that was already at least 85%
                        // visible is considered read and can be omitted from
                        // the next screen. Less-visible clipped lines repeat
                        // from their top so their missing portion is not lost.
                        if (visible_height * 100 >=
                            line_height * kMostlyVisiblePercent) {
                            continue;
                        }
                    }
                    return std::max(0, std::min(top, maximum));
                }
            }
        } else {
            for (std::size_t scanned = 0; scanned < 64; ++scanned) {
                const BlockLayout* block = markdown_layout_.layout_unit(unit);
                if (block != nullptr) {
                    for (auto line = block->lines.rbegin();
                         line != block->lines.rend(); ++line) {
                        int top = 0;
                        int bottom = 0;
                        line_bounds(unit, *line, top, bottom);
                        if (top > nominal) continue;
                        if (bottom - top > kViewportHeight && bottom > nominal) {
                            return nominal;
                        }
                        return std::max(0, std::min(top, maximum));
                    }
                }
                if (unit == 0) break;
                --unit;
            }
        }
        break;
    }
    return nominal;
}

int Viewer::next_page_scroll_y(int direction) {
    if (direction == 0) return scroll_y_;
    int maximum = max_scroll_y();
    if ((direction > 0 && scroll_y_ >= maximum) ||
        (direction < 0 && scroll_y_ <= 0)) {
        return scroll_y_;
    }
    if (direction > 0 && scroll_y_ + kViewportHeight >= maximum &&
        markdown_document_ != nullptr && markdown_layout_.unit_count() != 0) {
        // Reaching an estimated tail is the one place where a speculative
        // height could skip newly revealed pages. Validate the final block,
        // then recompute this single forward step from the unchanged origin.
        markdown_layout_.layout_unit(markdown_layout_.unit_count() - 1);
        document_height_ = std::max(kViewportHeight,
                                    fx_ceil(markdown_layout_.total_height()));
        maximum = max_scroll_y();
    }

    if (direction > 0 && maximum - scroll_y_ <= kViewportHeight) {
        return maximum;
    }

    // Align at the physical viewport boundary. A row clipped by that boundary
    // is repeated from its top so it becomes fully readable; a row whose
    // bottom is at or above the boundary has already been displayed and is
    // skipped.
    const int nominal = direction > 0
                            ? std::min(maximum,
                                       scroll_y_ + kViewportHeight)
                            : std::max(0, scroll_y_ - kViewportHeight);
    if (direction > 0 && nominal == maximum) return maximum;
    int target = aligned_scroll_y_near(nominal, direction > 0);
    // The only legitimate non-aligned fallback is movement through a glyph
    // line taller than the viewport. Guarantee monotonic progress even when
    // estimates change around that line.
    if (direction > 0 && target <= scroll_y_) target = nominal;
    if (direction < 0 && target >= scroll_y_) target = nominal;
    return std::max(0, std::min(target, maximum));
}

void Viewer::move_page(int direction) {
    if (plain_text_layout_.loaded()) {
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        const std::uint32_t before =
            plain_text_layout_.current_source_offset();
#endif
        std::string error;
        const bool moved = plain_text_layout_.move_page(direction, error);
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        const std::uint32_t after =
            plain_text_layout_.current_source_offset();
#endif
        if (!moved && !error.empty()) {
            show_message("TXT read error", error);
        }
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        std::printf(
            "NMARKDOWN_IT/1 TXT_PAGE_REQUEST direction=%d before=%lu "
            "after=%lu moved=%d error=%d\n",
            direction,
            static_cast<unsigned long>(before),
            static_cast<unsigned long>(after),
            moved ? 1 : 0,
            error.empty() ? 0 : 1);
        if (after != before) {
            std::printf(
                "NMARKDOWN_IT/1 TXT_PAGE_COMMIT source=input before=%lu "
                "after=%lu\n",
                static_cast<unsigned long>(before),
                static_cast<unsigned long>(after));
        }
        std::fflush(stdout);
#endif
        return;
    }
#if defined(NMARKDOWN_FIREBIRD_PAGE_FIXTURE)
    const int previous = current_page();
#endif
    screen_step_event_ = true;
    const int previous_scroll = scroll_y_;
    if (direction < 0 && !screen_step_history_.empty() &&
        screen_step_history_.back().to == scroll_y_) {
        const int prior_screen = screen_step_history_.back().from;
        // A preceding text-only screen starts with a complete top row even
        // when its saved origin came from bottom-aligned line scrolling. Keep
        // mixed-content screens on their established exact-history path.
        scroll_y_ = current_markdown_view_is_text_only()
                        ? (prior_screen < kLineScroll
                               ? 0
                               : aligned_scroll_y_near(prior_screen, false))
                        : prior_screen;
        screen_step_history_.pop_back();
    } else {
        if (direction < 0) screen_step_history_.clear();
        scroll_y_ = next_page_scroll_y(direction);
        if (direction > 0 && scroll_y_ > previous_scroll) {
            if (screen_step_history_.size() >= kMaximumScreenStepHistory) {
                screen_step_history_.erase(screen_step_history_.begin());
            }
            screen_step_history_.push_back({previous_scroll, scroll_y_});
        }
    }
#if defined(NMARKDOWN_FIREBIRD_PAGE_FIXTURE)
    const int pages_before = total_pages();
    const int target = std::max(1, std::min(pages_before,
                                            previous + direction));
    std::printf("NMARKDOWN_IT/1 PAGE_MOVE from=%d/%d target=%d scroll=%d now=%d/%d\n",
                previous, pages_before, target, scroll_y_, current_page(),
                total_pages());
#endif
}

void Viewer::clamp_view() {
    if (!plain_text_layout_.loaded()) {
        scroll_y_ = std::max(0, std::min(scroll_y_, max_scroll_y()));
    } else {
        scroll_y_ = 0;
    }
    const int maximum_pan = wide_focus_ ? focused_maximum_pan_ : 0;
    pan_x_ = std::max(0, std::min(pan_x_, maximum_pan));
}

bool Viewer::handle_event(const InputEvent& event) {
    // A restored final-page anchor is only a one-frame layout instruction.
    // Any user input before that frame takes ownership of the position.
    pending_final_page_restore_ = false;
    screen_step_event_ = false;
    line_step_event_ = false;
    if (event.type == InputEventType::Quit) {
        quit_requested_ = true;
        return true;
    }

    // A light contact/release is not a menu click. Treating it as Activate
    // made font assignment and other list actions fire while the user was
    // merely positioning a finger on the touchpad. Every overlay therefore
    // accepts the physical touchpad click (TouchpadActivation) and Enter, but
    // ignores TouchpadTap. Touchpad swipes continue to navigate supported
    // lists; Reader Settings deliberately ignores those swipes as well.
    InputEvent routed_event = event;
    if (overlay_open_) {
        if (event.type == InputEventType::Activate &&
            event.origin == InputEventOrigin::TouchpadTap) {
            routed_event.type = InputEventType::None;
        }
        const bool swipe =
            event.type == InputEventType::SwipeUp ||
            event.type == InputEventType::SwipeDown ||
            event.type == InputEventType::SwipeLeft ||
            event.type == InputEventType::SwipeRight;
        if ((settings_overlay_ || jump_overlay_) && swipe) {
            routed_event.type = InputEventType::None;
        } else {
            switch (event.type) {
            case InputEventType::SwipeUp:
                routed_event.type = InputEventType::PageDown;
                break;
            case InputEventType::SwipeDown:
                routed_event.type = InputEventType::PageUp;
                break;
            case InputEventType::SwipeLeft:
                routed_event.type = InputEventType::PanLeft;
                break;
            case InputEventType::SwipeRight:
                routed_event.type = InputEventType::PanRight;
                break;
            default:
                break;
            }
        }
    }
    const int old_scroll = scroll_y_;
    const std::uint32_t old_plain_text_offset =
        plain_text_layout_.current_source_offset();
    const int old_pan = pan_x_;
    const bool old_overlay = overlay_open_;
    const bool old_toc_overlay = toc_overlay_;
    const std::size_t old_toc_selected = toc_selected_;
    const bool old_search_overlay = search_overlay_;
    const bool old_jump_overlay = jump_overlay_;
    const std::string old_jump_query = jump_query_;
    const std::size_t old_search_selected = search_selected_;
    const SearchMode old_search_mode = search_mode_;
    const std::string old_search_query = search_query_;
    const bool old_settings_overlay = settings_overlay_;
    const bool old_diagnostics_overlay = diagnostics_overlay_;
    const bool old_document_browser_overlay = document_browser_overlay_;
    const std::size_t old_document_browser_selected = document_browser_selected_;
    const bool old_font_browser_overlay = font_browser_overlay_;
    const std::size_t old_font_browser_selected = font_browser_selected_;
    const bool old_link_overlay = link_overlay_;
    const bool old_link_choice_mode = link_choice_mode_;
    const std::size_t old_link_choice_selected = link_choice_selected_;
    const std::size_t old_settings_selected = settings_selected_;
    const bool old_bookmark_tab = bookmark_tab_;
    const std::size_t old_bookmark_selected = bookmark_selected_;
    const bool old_wide_focus = wide_focus_;
    bool bookmark_changed = false;
    const bool old_theme = dark_theme_;
    const bool old_high_contrast = high_contrast_;
    const int old_pixel_size = body_pixel_size_;
    const int old_line_gap = line_gap_px_;
    const int old_side_margin = side_margin_px_;
    const bool old_code_wrap = code_wrap_;
    const std::uint8_t old_table_mode = table_mode_;
    const ReadingMode old_reading_mode = reading_mode_;
    const bool old_natural_scrolling = natural_scrolling_;
    const bool old_natural_swiping = natural_swiping_;
    const bool old_resident_font_preload = resident_font_preload_;
    const bool old_chrome_visible = chrome_visible_;
    const RenderSharpness old_render_sharpness = render_sharpness_;
    const int old_max_scroll = max_scroll_y();
    const bool markdown_reflow = markdown_document_ != nullptr;
    const ViewAnchor reflow_anchor = markdown_reflow
                                         ? markdown_layout_.anchor_at(fx_from_int(scroll_y_))
                                         : ViewAnchor{};
    const auto adjust_setting = [this](int direction) {
        switch (settings_selected_) {
        case 0:
            set_dark_theme(!dark_theme_);
            break;
        case 1:
            body_pixel_size_ = std::max(
                12, std::min(22, body_pixel_size_ + (direction < 0 ? -1 : 1)));
            break;
        case 2:
            if (direction < 0) {
                // Manual spacing reaches a true zero-pixel gap; one step
                // below that returns to automatic leading.
                line_gap_px_ = line_gap_px_ <= 0 ? -1 : line_gap_px_ - 1;
            } else {
                line_gap_px_ = line_gap_px_ < 0 ? 0
                                                 : std::min(10, line_gap_px_ + 1);
            }
            break;
        case 3:
            side_margin_px_ = std::max(
                2, std::min(18, side_margin_px_ + (direction < 0 ? -1 : 1)));
            break;
        case 4:
            table_mode_ = table_mode_ == 0 ? 1 : 0;
            break;
        case 5:
            code_wrap_ = !code_wrap_;
            break;
        case 6:
            high_contrast_ = !high_contrast_;
            rebuild_settings_runs();
            break;
        case 7: {
            const int next = static_cast<int>(render_sharpness_) +
                             (direction < 0 ? -1 : 1);
            render_sharpness_ = static_cast<RenderSharpness>(
                std::max<int>(kMinimumRenderSharpness,
                              std::min<int>(kMaximumRenderSharpness, next)));
            break;
        }
        case 8:
            reading_mode_ = reading_mode_ == ReadingMode::VerticalScroll
                                ? ReadingMode::HorizontalScroll
                                : ReadingMode::VerticalScroll;
            break;
        case 9:
            natural_swiping_ = !natural_swiping_;
            break;
        case 10:
            natural_scrolling_ = !natural_scrolling_;
            break;
        case 11:
            resident_font_preload_ = !resident_font_preload_;
            rebuild_settings_runs();
            break;
        default:
            break;
        }
    };

    if (font_browser_overlay_) {
            const std::size_t row_count = font_browser_labels_.size();
            const std::size_t minimum_row = font_detail_open_ ? 1U : 0U;
            bool manager_action = false;
            switch (routed_event.type) {
            case InputEventType::ScrollLineUp:
                font_browser_selected_ = wrap_previous_row(
                    font_browser_selected_, row_count, minimum_row);
                break;
            case InputEventType::ScrollLineDown:
                font_browser_selected_ = wrap_next_row(
                    font_browser_selected_, row_count, minimum_row);
                break;
            case InputEventType::PageUp:
                font_browser_selected_ =
                    font_browser_selected_ > minimum_row + 5U
                        ? font_browser_selected_ - 5U
                        : minimum_row;
                break;
            case InputEventType::PageDown:
                if (row_count != 0) {
                    font_browser_selected_ = std::min(
                        font_browser_selected_ + 5U, row_count - 1U);
                }
                break;
            case InputEventType::Activate:
                manager_action = true;
                if (!font_detail_open_) {
                    if (font_browser_selected_ == font_file_catalog_.size()) {
                        active_font_paths_ = pending_font_paths_;
                        pending_font_assignments_available_ = true;
                        overlay_open_ = false;
                        font_browser_overlay_ = false;
                    } else if (font_browser_selected_ <
                               font_file_catalog_.size()) {
                            font_detail_index_ = font_browser_selected_;
                            font_detail_open_ = true;
                            font_browser_selected_ = 1;
                            rebuild_font_browser_runs();
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
                            std::printf("NMARKDOWN_IT/1 FONT_DETAIL_READY file=%lu\n",
                                        static_cast<unsigned long>(
                                            font_detail_index_));
                            std::fflush(stdout);
#endif
                    }
                } else if (font_detail_index_ < font_file_catalog_.size()) {
                    const FontFaceCatalogEntry& face =
                        font_file_catalog_[font_detail_index_];
                    if (font_browser_selected_ >= 1U &&
                        font_browser_selected_ <= kExternalFontRoleCount) {
                        const std::size_t role = font_browser_selected_ - 1U;
                        pending_font_paths_[role] =
                            pending_font_paths_[role] == face.path
                                ? std::string()
                                : face.path;
                        rebuild_font_browser_runs();
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
                        std::printf("NMARKDOWN_IT/1 FONT_ROLE_TOGGLED role=%lu\n",
                                    static_cast<unsigned long>(role));
                        std::fflush(stdout);
#endif
                    } else if (font_browser_selected_ ==
                               kExternalFontRoleCount + 1U) {
                        for (std::size_t role = 0;
                             role < kExternalFontRoleCount; ++role) {
                            if (suggested_font_role(face, role)) {
                                pending_font_paths_[role] = face.path;
                            }
                        }
                        rebuild_font_browser_runs();
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
                        std::printf("NMARKDOWN_IT/1 FONT_SUGGESTIONS_USED\n");
                        std::fflush(stdout);
#endif
                    } else if (font_browser_selected_ ==
                               kExternalFontRoleCount + 2U) {
                        for (std::string& path : pending_font_paths_) {
                            if (path == face.path) path.clear();
                        }
                        rebuild_font_browser_runs();
                    }
                }
                break;
            case InputEventType::Back:
                manager_action = true;
                if (font_detail_open_) {
                    font_detail_open_ = false;
                    font_browser_selected_ = font_detail_index_;
                    rebuild_font_browser_runs();
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
                    std::printf("NMARKDOWN_IT/1 FONT_DETAIL_BACK\n");
                    std::fflush(stdout);
#endif
                } else {
                    pending_font_paths_ = active_font_paths_;
                    overlay_open_ = false;
                    font_browser_overlay_ = false;
                }
                break;
            default:
                break;
            }
            const bool changed = old_overlay != overlay_open_ ||
                                 old_font_browser_overlay !=
                                     font_browser_overlay_ ||
                                 old_font_browser_selected !=
                                     font_browser_selected_ ||
                                 pending_font_assignments_available_ ||
                                 manager_action;
            dirty_ = dirty_ || changed;
            return changed;
    }

    if (document_browser_overlay_) {
        switch (routed_event.type) {
        case InputEventType::ScrollLineUp:
            document_browser_selected_ = wrap_previous_row(
                document_browser_selected_, document_browser_paths_.size());
            break;
        case InputEventType::ScrollLineDown:
            document_browser_selected_ = wrap_next_row(
                document_browser_selected_, document_browser_paths_.size());
            break;
        case InputEventType::PageUp:
            document_browser_selected_ = document_browser_selected_ > 5
                                             ? document_browser_selected_ - 5 : 0;
            break;
        case InputEventType::PageDown:
            if (!document_browser_paths_.empty()) {
                document_browser_selected_ = std::min(
                    document_browser_selected_ + 5, document_browser_paths_.size() - 1);
            }
            break;
        case InputEventType::Activate:
            if (document_browser_selected_ < document_browser_paths_.size()) {
                pending_document_open_ =
                    document_browser_paths_[document_browser_selected_];
                overlay_open_ = false;
                document_browser_overlay_ = false;
            }
            break;
        case InputEventType::Back:
            overlay_open_ = false;
            document_browser_overlay_ = false;
            // With no document behind the startup browser, cancelling should
            // return to TI-OS instead of exposing an empty synthetic reader.
            if (!document_loaded_) quit_requested_ = true;
            break;
        case InputEventType::OpenDocument:
        case InputEventType::OpenMenu:
            overlay_open_ = false;
            document_browser_overlay_ = false;
            if (!document_loaded_) quit_requested_ = true;
            break;
        default:
            break;
        }
        const bool changed = old_overlay != overlay_open_ ||
                             old_document_browser_overlay != document_browser_overlay_ ||
                             old_document_browser_selected != document_browser_selected_ ||
                             !pending_document_open_.empty();
        dirty_ = dirty_ || changed;
        return changed;
    }

    if (link_overlay_) {
        if (link_choice_mode_) {
            switch (routed_event.type) {
            case InputEventType::ScrollLineUp:
                link_choice_selected_ = wrap_previous_row(
                    link_choice_selected_, link_choice_ids_.size());
                break;
            case InputEventType::ScrollLineDown:
                link_choice_selected_ = wrap_next_row(
                    link_choice_selected_, link_choice_ids_.size());
                break;
            case InputEventType::PageUp:
                link_choice_selected_ = link_choice_selected_ > 5
                                            ? link_choice_selected_ - 5 : 0;
                break;
            case InputEventType::PageDown:
                if (!link_choice_ids_.empty()) {
                    link_choice_selected_ = std::min(
                        link_choice_selected_ + 5, link_choice_ids_.size() - 1);
                }
                break;
            case InputEventType::Activate:
                if (link_choice_selected_ < link_choice_ids_.size()) {
                    const std::uint32_t link_id = link_choice_ids_[link_choice_selected_];
                    overlay_open_ = false;
                    link_overlay_ = false;
                    link_choice_mode_ = false;
                    activate_link(link_id);
                }
                break;
            case InputEventType::Back:
            case InputEventType::OpenMenu:
                overlay_open_ = false;
                link_overlay_ = false;
                link_choice_mode_ = false;
                break;
            default:
                break;
            }
        } else if (routed_event.type == InputEventType::Activate ||
                   routed_event.type == InputEventType::Back ||
                   routed_event.type == InputEventType::OpenMenu) {
            overlay_open_ = false;
            link_overlay_ = false;
            if (message_confirm_opens_font_menu_) {
                if (routed_event.type == InputEventType::Activate) {
                    pending_font_menu_request_ = true;
                }
                message_confirm_opens_font_menu_ = false;
            }
        }
        const bool changed = old_overlay != overlay_open_ ||
                             old_link_overlay != link_overlay_ ||
                             old_link_choice_mode != link_choice_mode_ ||
                             old_link_choice_selected != link_choice_selected_ ||
                             !pending_document_link_.empty();
        dirty_ = dirty_ || changed;
        return changed;
    }

    if (jump_overlay_) {
        InputEvent jump_event = routed_event;
        if (is_numeric_navigation_alias(jump_event)) {
            jump_event.type = InputEventType::TextInput;
        }
        switch (jump_event.type) {
        case InputEventType::TextInput:
            if (jump_event.amount >= '0' && jump_event.amount <= '9' &&
                jump_query_.size() < 3) {
                jump_query_.push_back(
                    static_cast<char>(jump_event.amount));
                rebuild_jump_runs();
            }
            break;
        case InputEventType::Backspace:
            if (!jump_query_.empty()) {
                jump_query_.pop_back();
                rebuild_jump_runs();
            }
            break;
        case InputEventType::Activate:
            if (!jump_query_.empty()) {
                unsigned percentage = 0;
                for (const char digit : jump_query_) {
                    percentage = percentage * 10U +
                                 static_cast<unsigned>(digit - '0');
                }
                if (jump_to_percentage(std::min(100U, percentage))) {
                    overlay_open_ = false;
                    jump_overlay_ = false;
                }
            }
            break;
        case InputEventType::OpenMenu:
        case InputEventType::Back:
            overlay_open_ = false;
            jump_overlay_ = false;
            break;
        case InputEventType::OpenSearch:
            jump_overlay_ = false;
            search_overlay_ = plain_text_layout_.loaded();
            update_search();
            break;
        case InputEventType::OpenSettings:
            jump_overlay_ = false;
            settings_overlay_ = true;
            begin_settings_session();
            rebuild_settings_runs();
            break;
        case InputEventType::OpenDiagnostics:
            jump_overlay_ = false;
            diagnostics_overlay_ = true;
            rebuild_diagnostics_runs();
            break;
        case InputEventType::OpenDocument:
            pending_document_browser_request_ = true;
            break;
        default:
            break;
        }
        const bool changed =
            old_plain_text_offset !=
                plain_text_layout_.current_source_offset() ||
            old_overlay != overlay_open_ ||
            old_jump_overlay != jump_overlay_ ||
            old_jump_query != jump_query_ ||
            old_search_overlay != search_overlay_ ||
            old_settings_overlay != settings_overlay_ ||
            old_diagnostics_overlay != diagnostics_overlay_ ||
            pending_document_browser_request_;
        dirty_ = dirty_ || changed;
        return changed;
    }

    if (diagnostics_overlay_) {
        if (routed_event.type == InputEventType::OpenDiagnostics ||
            routed_event.type == InputEventType::Back ||
            (routed_event.type == InputEventType::Activate &&
             routed_event.origin == InputEventOrigin::TouchpadActivation)) {
            overlay_open_ = false;
            diagnostics_overlay_ = false;
        } else if (routed_event.type == InputEventType::OpenSettings) {
            diagnostics_overlay_ = false;
            settings_overlay_ = true;
            begin_settings_session();
            rebuild_settings_runs();
        } else if (routed_event.type == InputEventType::OpenSearch) {
            diagnostics_overlay_ = false;
            search_overlay_ = markdown_document_ != nullptr ||
                              plain_text_layout_.loaded();
            update_search();
        } else if (routed_event.type == InputEventType::OpenMenu) {
            show_content_jump();
        } else if (routed_event.type == InputEventType::OpenDocument) {
            pending_document_browser_request_ = true;
        }
        const bool changed = old_overlay != overlay_open_ ||
                             old_diagnostics_overlay != diagnostics_overlay_ ||
                             old_settings_overlay != settings_overlay_ ||
                             old_search_overlay != search_overlay_ ||
                             old_toc_overlay != toc_overlay_ ||
                             pending_document_browser_request_;
        dirty_ = dirty_ || changed;
        return changed;
    }

    if (search_overlay_) {
        InputEvent search_event = routed_event;
        if (is_numeric_navigation_alias(search_event)) {
            search_event.type = InputEventType::TextInput;
        }
        switch (search_event.type) {
        case InputEventType::TextInput:
            if (search_event.amount >= 0x20 &&
                search_event.amount <= 0x10FFFF) {
                std::string appended;
                utf8_append(static_cast<std::uint32_t>(search_event.amount),
                            appended);
                if (search_query_.size() + appended.size() <= 64) {
                    search_query_ += appended;
                    update_search();
#if defined(NMARKDOWN_FIREBIRD_KEYMAP_FIXTURE)
                    std::printf("NMARKDOWN_IT/1 SEARCH_QUERY value=%s\n",
                                search_query_.c_str());
                    std::fflush(stdout);
#endif
                }
            }
            break;
        case InputEventType::Backspace:
            if (!search_query_.empty()) {
                std::size_t offset = search_query_.size() - 1;
                while (offset > 0 &&
                       (static_cast<unsigned char>(search_query_[offset]) & 0xC0U) == 0x80U) {
                    --offset;
                }
                search_query_.erase(offset);
                update_search();
            }
            break;
        case InputEventType::ScrollLineUp:
            search_selected_ =
                wrap_previous_row(search_selected_, search_results_.size());
            break;
        case InputEventType::ScrollLineDown:
        case InputEventType::SearchNext:
            // Search-next cycles: continuing past the last match returns to
            // the first.
            search_selected_ =
                wrap_next_row(search_selected_, search_results_.size());
            break;
        case InputEventType::PageUp:
            search_selected_ = search_selected_ > 4 ? search_selected_ - 4 : 0;
            break;
        case InputEventType::PageDown:
            if (!search_results_.empty()) {
                search_selected_ = std::min(search_selected_ + 4,
                                            search_results_.size() - 1);
            }
            break;
        case InputEventType::PanLeft: {
            const int value = static_cast<int>(search_mode_);
            search_mode_ = static_cast<SearchMode>((value + 3) % 4);
            update_search();
            break;
        }
        case InputEventType::PanRight: {
            const int value = static_cast<int>(search_mode_);
            search_mode_ = static_cast<SearchMode>((value + 1) % 4);
            update_search();
            break;
        }
        case InputEventType::Activate:
            if (!search_results_.empty()) {
                activate_search_result(search_selected_);
                overlay_open_ = false;
                search_overlay_ = false;
            }
            break;
        case InputEventType::OpenSearch:
        case InputEventType::Back:
            overlay_open_ = false;
            search_overlay_ = false;
            break;
        case InputEventType::OpenSettings:
            search_overlay_ = false;
            settings_overlay_ = true;
            begin_settings_session();
            rebuild_settings_runs();
            break;
        case InputEventType::OpenDiagnostics:
            search_overlay_ = false;
            diagnostics_overlay_ = true;
            rebuild_diagnostics_runs();
            break;
        case InputEventType::OpenMenu:
            show_content_jump();
            break;
        case InputEventType::OpenDocument:
            pending_document_browser_request_ = true;
            break;
        default:
            break;
        }
        clamp_view();
        const bool changed = old_scroll != scroll_y_ ||
                             old_overlay != overlay_open_ ||
                             old_search_overlay != search_overlay_ ||
                             old_search_selected != search_selected_ ||
                             old_search_mode != search_mode_ ||
                             old_search_query != search_query_ ||
                             old_settings_overlay != settings_overlay_ ||
                             old_diagnostics_overlay != diagnostics_overlay_ ||
                             old_toc_overlay != toc_overlay_ ||
                             pending_document_browser_request_;
        dirty_ = dirty_ || changed;
        return changed;
    }

    // Keys retain semantic directions in both touchpad layouts. Gesture
    // direction changes touch only: Page Up/Down and arrow/numeric aliases
    // always mean earlier/later content.
    switch (routed_event.type) {
    case InputEventType::ScrollLineUp:
        if (overlay_open_ && settings_overlay_) {
            settings_selected_ =
                wrap_previous_row(settings_selected_, kSettingsRowCount);
        } else if (overlay_open_ && toc_overlay_) {
            if (bookmark_tab_) {
                bookmark_selected_ = wrap_previous_row(
                    bookmark_selected_, bookmark_runs_.size());
            } else {
                toc_selected_ =
                    wrap_previous_row(toc_selected_, toc_runs_.size());
            }
        } else if (overlay_open_) {
            // Passive controls overlays consume document navigation.
        } else if (plain_text_layout_.loaded()) {
            chrome_visible_ = true;
            std::string error;
            if (!plain_text_layout_.move_line(-1, error) &&
                !error.empty()) {
                show_message("TXT read error", error);
            }
        } else {
            chrome_visible_ = true;
            move_markdown_line(-1);
        }
        break;
    case InputEventType::ScrollLineDown:
        if (overlay_open_ && settings_overlay_) {
            settings_selected_ =
                wrap_next_row(settings_selected_, kSettingsRowCount);
        } else if (overlay_open_ && toc_overlay_) {
            if (bookmark_tab_) {
                bookmark_selected_ = wrap_next_row(
                    bookmark_selected_, bookmark_runs_.size());
            } else {
                toc_selected_ =
                    wrap_next_row(toc_selected_, toc_runs_.size());
            }
        } else if (overlay_open_) {
            // Passive controls overlays consume document navigation.
        } else if (plain_text_layout_.loaded()) {
            chrome_visible_ = false;
            std::string error;
            if (!plain_text_layout_.move_line(1, error) &&
                !error.empty()) {
                show_message("TXT read error", error);
            }
        } else {
            chrome_visible_ = false;
            move_markdown_line(1);
        }
        break;
    case InputEventType::SwipeUp:
        if (!overlay_open_ && reading_mode_ == ReadingMode::HorizontalScroll) {
            chrome_visible_ = natural_swiping_;
            move_page(natural_swiping_ ? -1 : 1);
        }
        break;
    case InputEventType::PageUp:
        if (overlay_open_ && toc_overlay_) {
            if (bookmark_tab_) {
                bookmark_selected_ = bookmark_selected_ > 5
                                         ? bookmark_selected_ - 5 : 0;
            } else {
                toc_selected_ = toc_selected_ > 5 ? toc_selected_ - 5 : 0;
            }
        } else if (overlay_open_) {
            // Settings/help owns the event; do not page the document below it.
        } else {
            chrome_visible_ = true;
            move_page(-1);
        }
        break;
    case InputEventType::SwipeDown:
        if (!overlay_open_ && reading_mode_ == ReadingMode::HorizontalScroll) {
            chrome_visible_ = !natural_swiping_;
            move_page(natural_swiping_ ? 1 : -1);
        }
        break;
    case InputEventType::PageDown:
        if (overlay_open_ && toc_overlay_) {
            if (bookmark_tab_ && !bookmark_runs_.empty()) {
                bookmark_selected_ = std::min(bookmark_selected_ + 5,
                                              bookmark_runs_.size() - 1);
            } else if (!bookmark_tab_ && !toc_runs_.empty()) {
                toc_selected_ = std::min(toc_selected_ + 5, toc_runs_.size() - 1);
            }
        } else if (overlay_open_) {
            // Settings/help owns the event; do not page the document below it.
        } else {
            chrome_visible_ = false;
            move_page(1);
        }
        break;
    case InputEventType::SwipeLeft:
        if (overlay_open_) {
            // Modal layers own touch gestures; do not move the document below.
        } else if (reading_mode_ == ReadingMode::HorizontalScroll) {
            // The same physical drag also emits PointerPan. Ignore this
            // threshold marker while horizontal movement owns continuous
            // document scrolling.
        } else if (consume_wide_pan(std::max(12, content_width() - 24))) {
            // Direct-manipulation panning over wide content: moving the
            // finger left reveals content farther to the right.
        } else {
            chrome_visible_ = natural_swiping_;
            move_page(natural_swiping_ ? -1 : 1);
        }
        break;
    case InputEventType::SwipeRight:
        if (overlay_open_) {
            // Modal layers own touch gestures; do not move the document below.
        } else if (reading_mode_ == ReadingMode::HorizontalScroll) {
            // PointerPan owns horizontal movement in this mode.
        } else if (consume_wide_pan(-std::max(12, content_width() - 24))) {
            // Reserved for panning while wide content is in view.
        } else {
            chrome_visible_ = !natural_swiping_;
            move_page(natural_swiping_ ? 1 : -1);
        }
        break;
    case InputEventType::PanLeft:
        if (overlay_open_ && settings_overlay_) {
            adjust_setting(-1);
        } else if (overlay_open_ && toc_overlay_ && !bookmarks_.empty() &&
                   markdown_document_ != nullptr) {
            // Plain text has no section system; its list stays on the
            // bookmark tab.
            bookmark_tab_ = false;
        } else if (overlay_open_) {
            // Unknown modal input is consumed as a no-op.
        } else if (consume_wide_pan(-12)) {
            // Reserved for panning while wide content is in view.
        } else {
            chrome_visible_ = true;
            move_page(-1);
        }
        break;
    case InputEventType::PanRight:
        if (overlay_open_ && settings_overlay_) {
            adjust_setting(1);
        } else if (overlay_open_ && toc_overlay_ && !bookmarks_.empty()) {
            bookmark_tab_ = true;
        } else if (overlay_open_) {
            // Unknown modal input is consumed as a no-op.
        } else if (consume_wide_pan(12)) {
            // Reserved for panning while wide content is in view.
        } else {
            chrome_visible_ = false;
            move_page(1);
        }
        break;
    case InputEventType::PointerScroll:
        if (overlay_open_) {
            // Pointer gestures never reach the covered document.
        } else if (reading_mode_ == ReadingMode::VerticalScroll) {
            const int pixels = natural_scrolling_
                                   ? -routed_event.amount
                                   : routed_event.amount;
            if (pixels != 0) chrome_visible_ = pixels < 0;
            if (plain_text_layout_.loaded()) {
                std::string error;
                if (!plain_text_layout_.scroll_pixels(pixels, error) &&
                    !error.empty()) {
                    show_message("TXT read error", error);
                }
            } else {
                scroll_y_ += pixels;
            }
        }
        break;
    case InputEventType::PointerPan:
        if (overlay_open_) {
            // Pointer gestures never reach the covered document.
        } else if (reading_mode_ == ReadingMode::HorizontalScroll && wide_focus_) {
            // Direct manipulation: dragging left reveals content to the right.
            pan_x_ -= routed_event.amount;
        } else if (reading_mode_ == ReadingMode::HorizontalScroll) {
            // Natural makes a leftward drag advance; Reversed makes a
            // rightward drag advance.
            const int pixels = natural_scrolling_
                                   ? -routed_event.amount
                                   : routed_event.amount;
            if (pixels != 0) chrome_visible_ = pixels < 0;
            if (plain_text_layout_.loaded()) {
                std::string error;
                if (!plain_text_layout_.scroll_pixels(pixels, error) &&
                    !error.empty()) {
                    show_message("TXT read error", error);
                }
            } else {
                scroll_y_ += pixels;
            }
        }
        break;
    case InputEventType::OpenBookmarks:
        if (overlay_open_ && toc_overlay_ && bookmark_tab_) {
            // The Catalog key toggles its own list closed again.
            overlay_open_ = false;
            toc_overlay_ = false;
        } else if (overlay_open_) {
            // Other modal layers keep the event.
        } else if (document_loaded_) {
            commit_settings_session();
            overlay_open_ = true;
            toc_overlay_ = true;
            jump_overlay_ = false;
            search_overlay_ = false;
            settings_overlay_ = false;
            diagnostics_overlay_ = false;
            document_browser_overlay_ = false;
            font_browser_overlay_ = false;
            link_overlay_ = false;
            bookmark_tab_ = true;
            rebuild_bookmark_runs();
        }
        break;
    case InputEventType::OpenMenu:
        if (overlay_open_ && (toc_overlay_ ||
            jump_overlay_ ||
            (!settings_overlay_ && !search_overlay_ && !diagnostics_overlay_ &&
             !document_browser_overlay_ && !font_browser_overlay_ && !link_overlay_))) {
            overlay_open_ = false;
            toc_overlay_ = false;
            jump_overlay_ = false;
        } else {
            show_content_jump();
        }
        break;
    case InputEventType::OpenSearch:
        overlay_open_ = true;
        toc_overlay_ = false;
        jump_overlay_ = false;
        search_overlay_ = markdown_document_ != nullptr ||
                          plain_text_layout_.loaded();
        settings_overlay_ = false;
        diagnostics_overlay_ = false;
        link_overlay_ = false;
        update_search();
        break;
    case InputEventType::OpenSettings:
        if (overlay_open_ && settings_overlay_) {
            overlay_open_ = false;
            settings_overlay_ = false;
        } else {
            overlay_open_ = true;
            settings_overlay_ = true;
            begin_settings_session();
            toc_overlay_ = false;
            jump_overlay_ = false;
            search_overlay_ = false;
            diagnostics_overlay_ = false;
            link_overlay_ = false;
            rebuild_settings_runs();
        }
        break;
    case InputEventType::OpenDiagnostics:
        if (overlay_open_ && diagnostics_overlay_) {
            overlay_open_ = false;
            diagnostics_overlay_ = false;
        } else {
            overlay_open_ = true;
            diagnostics_overlay_ = true;
            toc_overlay_ = false;
            jump_overlay_ = false;
            search_overlay_ = false;
            settings_overlay_ = false;
            link_overlay_ = false;
            rebuild_diagnostics_runs();
        }
        break;
    case InputEventType::OpenDocument:
        pending_document_browser_request_ = true;
        break;
    case InputEventType::SearchNext:
        if (!overlay_open_ && !search_results_.empty()) {
            const std::size_t next = has_active_search_match_
                                         ? (search_selected_ + 1) % search_results_.size()
                                         : search_selected_;
            activate_search_result(next);
        }
        break;
    case InputEventType::ToggleBookmark:
        if (!overlay_open_ &&
            ((markdown_document_ != nullptr &&
              markdown_layout_.unit_count() != 0) ||
             plain_text_layout_.loaded())) {
            const std::uint32_t source_offset =
                plain_text_layout_.loaded()
                    ? plain_text_layout_.current_source_offset()
                    : markdown_layout_.anchor_at(
                          fx_from_int(scroll_y_)).source_offset;
            const auto existing = std::find(bookmarks_.begin(), bookmarks_.end(),
                                            source_offset);
            if (existing == bookmarks_.end()) {
                if (bookmarks_.size() < 256) {
                    bookmarks_.push_back(source_offset);
                    std::sort(bookmarks_.begin(), bookmarks_.end());
                    bookmark_changed = true;
                    rebuild_bookmark_runs();
                }
            } else {
                bookmarks_.erase(existing);
                bookmark_changed = true;
                rebuild_bookmark_runs();
            }
        }
        break;
    case InputEventType::TextInput:
    case InputEventType::Backspace:
        // Del removes the selected bookmark from the management list.
        if (overlay_open_ && toc_overlay_ && bookmark_tab_ &&
            bookmark_selected_ < bookmarks_.size()) {
            bookmarks_.erase(bookmarks_.begin() +
                             static_cast<std::ptrdiff_t>(bookmark_selected_));
            if (bookmark_selected_ >= bookmarks_.size() &&
                bookmark_selected_ > 0) {
                --bookmark_selected_;
            }
            bookmark_changed = true;
            rebuild_bookmark_runs();
        }
        break;
    case InputEventType::Activate:
        if (overlay_open_ && settings_overlay_) {
            // Left/Right edits the selected value. Enter confirms the whole
            // settings session without silently incrementing or toggling the
            // highlighted row. Fonts is an action row, so confirming it opens
            // the manager after the current settings session is committed.
            if (settings_selected_ == kSettingsRowCount - 1U) {
                pending_font_menu_request_ = true;
            }
            overlay_open_ = false;
            settings_overlay_ = false;
        } else if (overlay_open_ && toc_overlay_ && bookmark_tab_ &&
                   bookmark_selected_ < bookmarks_.size()) {
            if (plain_text_layout_.loaded()) {
                std::string error;
                if (!plain_text_layout_.seek_source(
                        bookmarks_[bookmark_selected_], error) &&
                    !error.empty()) {
                    show_message("TXT read error", error);
                }
            } else {
                scroll_y_ = fx_floor(markdown_layout_.position_for_source(
                    kInvalidNode, bookmarks_[bookmark_selected_], 0));
            }
            chrome_visible_ = true;
            overlay_open_ = false;
            toc_overlay_ = false;
        } else if (overlay_open_ && toc_overlay_ && markdown_document_ != nullptr &&
                   !bookmark_tab_ &&
                   toc_selected_ < markdown_document_->ir.headings.size()) {
            const HeadingEntry& heading = markdown_document_->ir.headings[toc_selected_];
            scroll_y_ = fx_floor(markdown_layout_.position_for_source(
                heading.block, heading.source_offset, 0));
            chrome_visible_ = true;
            overlay_open_ = false;
            toc_overlay_ = false;
            jump_overlay_ = false;
            search_overlay_ = false;
        } else if (overlay_open_ &&
                   routed_event.origin == InputEventOrigin::TouchpadActivation) {
            // A contact tap closes the passive Help panel. Keyboard Enter is
            // deliberately still inert here, preserving its prior behavior.
            overlay_open_ = false;
        } else if (overlay_open_) {
            // Empty lists and passive Help consume keyboard Enter without a
            // hidden action.
        } else if (!activate_current_link()) {
            NodeId node = kInvalidNode;
            int maximum_pan = 0;
            if (current_block_is_wide(node, maximum_pan)) {
                if (wide_focus_ && focused_node_ == node) {
                    exit_wide_focus();
                } else {
                    enter_wide_focus(node, maximum_pan);
                }
            }
        }
        break;
    case InputEventType::IncreaseFont:
        // While the bookmark list is open, "+" saves the reading position
        // behind it — the first line of the current screen.
        if (overlay_open_ && toc_overlay_ && bookmark_tab_) {
            if ((markdown_document_ != nullptr &&
                 markdown_layout_.unit_count() != 0) ||
                plain_text_layout_.loaded()) {
                const std::uint32_t source_offset =
                    plain_text_layout_.loaded()
                        ? plain_text_layout_.current_source_offset()
                        : markdown_layout_.anchor_at(
                              fx_from_int(scroll_y_)).source_offset;
                const auto existing = std::find(
                    bookmarks_.begin(), bookmarks_.end(), source_offset);
                if (existing == bookmarks_.end() &&
                    bookmarks_.size() < 256) {
                    bookmarks_.push_back(source_offset);
                    std::sort(bookmarks_.begin(), bookmarks_.end());
                    bookmark_changed = true;
                    rebuild_bookmark_runs();
                }
                const auto saved = std::find(
                    bookmarks_.begin(), bookmarks_.end(), source_offset);
                if (saved != bookmarks_.end()) {
                    bookmark_selected_ = static_cast<std::size_t>(
                        saved - bookmarks_.begin());
                }
            }
        } else if (!overlay_open_) {
            body_pixel_size_ = std::min(22, body_pixel_size_ + 1);
        }
        break;
    case InputEventType::DecreaseFont:
        if (!overlay_open_) body_pixel_size_ = std::max(12, body_pixel_size_ - 1);
        break;
    case InputEventType::Back:
        if (overlay_open_) {
            overlay_open_ = false;
            toc_overlay_ = false;
            search_overlay_ = false;
            settings_overlay_ = false;
            diagnostics_overlay_ = false;
            document_browser_overlay_ = false;
            font_browser_overlay_ = false;
            link_overlay_ = false;
        } else if (wide_focus_) {
            exit_wide_focus();
        } else {
            quit_requested_ = true;
        }
        break;
    case InputEventType::Quit:
        break;
    case InputEventType::None:
        break;
    }

    const bool font_size_changed = body_pixel_size_ != old_pixel_size;
    const bool layout_settings_changed = font_size_changed ||
                                         line_gap_px_ != old_line_gap ||
                                         side_margin_px_ != old_side_margin ||
                                         code_wrap_ != old_code_wrap ||
                                         table_mode_ != old_table_mode;
    const bool settings_value_changed =
        old_theme != dark_theme_ || old_high_contrast != high_contrast_ ||
        layout_settings_changed || old_reading_mode != reading_mode_ ||
        old_natural_scrolling != natural_scrolling_ ||
        old_natural_swiping != natural_swiping_ ||
        old_resident_font_preload != resident_font_preload_ ||
        old_render_sharpness != render_sharpness_;
    const bool defer_settings_apply =
        old_settings_overlay && settings_overlay_;
    if (settings_value_changed && defer_settings_apply) {
        // Keep the menu labels live, but retain the already painted document
        // and its layout until this settings session closes.
        rebuild_settings_runs();
        settings_overlay_repaint_only_ = true;
    }
    if (layout_settings_changed && !defer_settings_apply) {
        if (font_size_changed) {
            text_.clear_cache();
            math_.clear_cache();
            rebuild_text_runs();
            rebuild_toc_runs();
            rebuild_bookmark_runs();
            rebuild_search_runs();
        }
        if (markdown_reflow) {
            std::string error;
            if (markdown_layout_.reconfigure(layout_signature(), error)) {
                document_height_ = std::max(kViewportHeight,
                                            fx_ceil(markdown_layout_.total_height()));
                scroll_y_ = fx_floor(markdown_layout_.position_of(reflow_anchor));
            } else {
                set_document_error(error);
            }
        } else if (plain_text_layout_.loaded()) {
            std::string error;
            if (!plain_text_layout_.reconfigure(layout_signature(), error)) {
                set_document_error(error);
            }
        } else if (font_size_changed) {
            document_height_ = std::max(720,
                                        document_height_ * body_pixel_size_ /
                                            std::max(1, old_pixel_size));
            if (old_max_scroll > 0) {
                scroll_y_ = static_cast<int>(static_cast<std::int64_t>(old_scroll) *
                                             max_scroll_y() / old_max_scroll);
            }
        }
        wide_focus_ = false;
        focused_node_ = kInvalidNode;
        focused_maximum_pan_ = 0;
        focused_code_layout_ = {};
        focused_code_layout_valid_ = false;
        pan_x_ = 0;
        rebuild_settings_runs();
    }

    if (old_reading_mode != reading_mode_ && !defer_settings_apply) {
        rebuild_settings_runs();
    }

    if (old_settings_overlay && !settings_overlay_) {
        commit_settings_session();
    }

    if (old_theme != dark_theme_ || old_high_contrast != high_contrast_ ||
        old_pixel_size != body_pixel_size_ || old_line_gap != line_gap_px_ ||
        old_side_margin != side_margin_px_ || old_code_wrap != code_wrap_ ||
        old_table_mode != table_mode_ || old_reading_mode != reading_mode_ ||
        old_natural_scrolling != natural_scrolling_ ||
        old_natural_swiping != natural_swiping_ ||
        old_render_sharpness != render_sharpness_ ||
        bookmark_changed) {
        pending_state_save_request_ = true;
    }

    clamp_view();
    if (old_scroll != scroll_y_ && !screen_step_event_) {
        screen_step_history_.clear();
    }
    if (old_scroll != scroll_y_ && !line_step_event_) {
        line_step_history_.clear();
    }
#if defined(NMARKDOWN_FIREBIRD_THEME_FIXTURE)
    if (old_settings_overlay && settings_overlay_ &&
        old_settings_selected != settings_selected_) {
        std::printf("NMARKDOWN_IT/1 THEME_MODAL_SELECTION row=%lu\n",
                    static_cast<unsigned long>(settings_selected_));
        std::fflush(stdout);
    }
    if (old_settings_overlay && settings_overlay_ &&
        old_pixel_size != body_pixel_size_) {
        std::printf("NMARKDOWN_IT/1 THEME_MODAL_BODY_SIZE size=%d\n",
                    body_pixel_size_);
        std::fflush(stdout);
    }
#endif
#if defined(NMARKDOWN_FIREBIRD_SCROLL_SWIPE_FIXTURE)
    if (old_scroll != scroll_y_) {
        const char* event_name = "other";
        switch (event.type) {
        case InputEventType::ScrollLineUp: event_name = "line-up"; break;
        case InputEventType::ScrollLineDown: event_name = "line-down"; break;
        case InputEventType::PageUp: event_name = "page-up"; break;
        case InputEventType::PageDown: event_name = "page-down"; break;
        case InputEventType::SwipeLeft: event_name = "swipe-left"; break;
        case InputEventType::SwipeRight: event_name = "swipe-right"; break;
        case InputEventType::PointerScroll: event_name = "pointer"; break;
        default: break;
        }
        std::printf("NMARKDOWN_IT/1 SCROLL_POSITION event=%s before=%d after=%d\n",
                    event_name, old_scroll, scroll_y_);
        std::fflush(stdout);
    }
#endif
#if defined(NMARKDOWN_FIREBIRD_OVERSIZED_FORMULA_FIXTURE)
    if (old_pan != pan_x_) {
        const char* event_name = "other";
        switch (event.type) {
        case InputEventType::PageUp: event_name = "page-up"; break;
        case InputEventType::PageDown: event_name = "page-down"; break;
        case InputEventType::SwipeLeft: event_name = "swipe-left"; break;
        case InputEventType::SwipeRight: event_name = "swipe-right"; break;
        case InputEventType::PanLeft: event_name = "pan-left"; break;
        case InputEventType::PanRight: event_name = "pan-right"; break;
        default: break;
        }
        std::printf("NMARKDOWN_IT/1 FORMULA_PAN event=%s before=%d after=%d max=%d\n",
                    event_name, old_pan, pan_x_, focused_maximum_pan_);
        std::fflush(stdout);
    }
#endif
    const bool plain_text_position_changed =
        old_plain_text_offset != plain_text_layout_.current_source_offset();
    const bool changed = old_scroll != scroll_y_ ||
                         plain_text_position_changed ||
                         old_pan != pan_x_ ||
                         old_overlay != overlay_open_ || old_theme != dark_theme_ ||
                         old_high_contrast != high_contrast_ ||
                         old_pixel_size != body_pixel_size_ ||
                         old_line_gap != line_gap_px_ ||
                         old_side_margin != side_margin_px_ ||
                         old_toc_overlay != toc_overlay_ ||
                         old_toc_selected != toc_selected_ ||
                         old_search_overlay != search_overlay_ ||
                         old_jump_overlay != jump_overlay_ ||
                         old_search_selected != search_selected_ ||
                         old_search_mode != search_mode_ ||
                         old_search_query != search_query_ ||
                         old_settings_overlay != settings_overlay_ ||
                         old_diagnostics_overlay != diagnostics_overlay_ ||
                         old_document_browser_overlay != document_browser_overlay_ ||
                         old_font_browser_overlay != font_browser_overlay_ ||
                         old_link_overlay != link_overlay_ ||
                         old_settings_selected != settings_selected_ ||
                         old_bookmark_tab != bookmark_tab_ ||
                         old_bookmark_selected != bookmark_selected_ ||
                         old_wide_focus != wide_focus_ ||
                         old_code_wrap != code_wrap_ ||
                         old_table_mode != table_mode_ ||
                         old_reading_mode != reading_mode_ ||
                         old_natural_scrolling != natural_scrolling_ ||
                         old_natural_swiping != natural_swiping_ ||
                         old_resident_font_preload !=
                             resident_font_preload_ ||
                         old_chrome_visible != chrome_visible_ ||
                         old_render_sharpness != render_sharpness_ ||
                         bookmark_changed ||
                         !pending_document_link_.empty() ||
                         pending_document_browser_request_ ||
                         !pending_document_open_.empty() ||
                         pending_font_menu_request_ ||
                         pending_font_assignments_available_;
    const bool base_visual_changed =
        old_scroll != scroll_y_ || plain_text_position_changed ||
        old_pan != pan_x_ ||
        old_theme != dark_theme_ || old_high_contrast != high_contrast_ ||
        old_pixel_size != body_pixel_size_ || old_line_gap != line_gap_px_ ||
        old_side_margin != side_margin_px_ || old_wide_focus != wide_focus_ ||
        old_code_wrap != code_wrap_ || old_table_mode != table_mode_ ||
        old_reading_mode != reading_mode_ ||
        old_render_sharpness != render_sharpness_ || bookmark_changed;
    if (base_visual_changed) invalidate_retained_base_frame();
    if (old_settings_overlay && settings_overlay_ && changed) {
        settings_overlay_repaint_only_ = true;
    }
    dirty_ = dirty_ || changed;
    return changed;
}

bool Viewer::perform_incremental_work(
    const Clock& clock,
    std::uint64_t deadline_ms) {
    // This return value means "background work was performed", not "the
    // framebuffer changed." The application polls input again after every
    // bounded quantum and never repaints solely for speculative work.
    if (plain_text_layout_.loaded()) {
        if (overlay_open_) return false;
        // A streamed external font can perform several storage seeks for one
        // cold FreeType glyph. Bound speculative work by cold cache misses,
        // rather than an arbitrary glyph count: already-rasterized repeats are
        // RAM-only and can be skipped quickly, while at most one flash-prone
        // glyph is admitted between input polls on calculator builds.
        constexpr std::size_t kMaximumGlyphsExaminedPerPoll = 32;
        const std::size_t maximum_cold_glyphs =
            text_.has_streamed_external_fonts() ? 1U : 4U;
        std::size_t warmed = 0;
        std::size_t cold_glyphs = 0;
        while (warmed < kMaximumGlyphsExaminedPerPoll &&
               cold_glyphs < maximum_cold_glyphs) {
            const std::uint64_t misses_before =
                text_.cache_stats().misses;
            const std::size_t cached =
                plain_text_layout_.deferred_forward_page_count() != 0
                    ? plain_text_layout_.preload_deferred_page_glyphs(1)
                    : plain_text_layout_.preload_next_glyphs(1);
            const std::uint64_t misses_after =
                text_.cache_stats().misses;
            if (misses_after > misses_before) {
                cold_glyphs += static_cast<std::size_t>(
                    misses_after - misses_before);
            }
            if (cached == 0) {
                // A failed cold rasterization may still have performed font
                // I/O. Yield before any layout work so it remains the only
                // potentially expensive operation in this input quantum.
                if (misses_after > misses_before) return true;
                break;
            }
            warmed += cached;
            if (deadline_ms != 0 &&
                clock.milliseconds() >= deadline_ms) {
                break;
            }
        }
        if (warmed != 0) return true;
        if (deadline_ms != 0 &&
            clock.milliseconds() >= deadline_ms) {
            return false;
        }
        std::string error;
        const std::uint32_t before =
            plain_text_layout_.current_source_offset();
        const bool did_work =
            plain_text_layout_.perform_incremental_work(error);
        const std::uint32_t after =
            plain_text_layout_.current_source_offset();
        if (after != before) {
            invalidate_retained_base_frame();
            dirty_ = true;
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
            std::printf(
                "NMARKDOWN_IT/1 TXT_PAGE_COMMIT source=deferred before=%lu "
                "after=%lu\n",
                static_cast<unsigned long>(before),
                static_cast<unsigned long>(after));
            std::fflush(stdout);
#endif
        }
        return did_work;
    }
    if (markdown_document_ == nullptr || overlay_open_ ||
        markdown_layout_.unit_count() == 0) {
        return false;
    }
    const Fx preload_from = fx_from_int(std::min(
        document_height_, scroll_y_ + kViewportHeight));
    const LayoutLine* line = markdown_layout_.preload_next_line(
        preload_from,
        fx_from_int(kIdlePreloadViewports * kViewportHeight));
    bool did_work = line != nullptr;
    if (line != nullptr) {
        std::size_t remaining = 48;
        for (const LayoutRun& run : line->runs) {
            if (remaining == 0) break;
            // Formula layout has its own glyph collection/cache path. Text and
            // code dominate large TXT documents and are safe to warm here.
            if (run.math != nullptr) continue;
            const std::uint8_t synthesis = static_cast<std::uint8_t>(
                ((run.style_flags & InlineStyleStrong) != 0
                     ? TextSynthesisBold : TextSynthesisNone) |
                ((run.style_flags & InlineStyleEmphasis) != 0
                     ? TextSynthesisItalic : TextSynthesisNone));
            const std::size_t cached =
                text_.cache_run(run.glyphs, run.pixel_size, remaining,
                                synthesis);
            remaining -= std::min(remaining, cached);
        }
    }
    document_height_ = std::max(kViewportHeight,
                                fx_ceil(markdown_layout_.total_height()));
    return did_work;
}

void Viewer::render_document(const Surface565& surface, Rect viewport) {
    if (document_error_) {
        const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
        fill_rect(surface, viewport, colors.paper, viewport);
        if (text_ready_) {
            text_.draw_run(surface, document_error_title_run_, 12,
                           viewport.y + 38, fx_from_int(16), colors.failure,
                           colors.paper, dark_theme_, true, viewport);
            for (std::size_t line = 0;
                 line < document_error_message_runs_.size(); ++line) {
                text_.draw_run(surface, document_error_message_runs_[line],
                               12,
                               viewport.y + 70 + static_cast<int>(line) * 16,
                               fx_from_int(11), colors.ink,
                               colors.paper, dark_theme_, true,
                               {12, viewport.y, kScreenWidth - 24,
                                viewport.height});
            }
            text_.draw_run(surface, document_error_hint_run_, 12,
                           viewport.y + 105, fx_from_int(10), colors.muted_ink,
                           colors.paper, dark_theme_, true, viewport);
        }
        return;
    }
    // There is no synthetic reader page behind the startup browser.  Keep the
    // canvas empty until a real document has been selected (or a load error is
    // available to show); otherwise launch briefly exposes sample text that
    // looks like document content.
    if (!document_loaded_ && markdown_document_ == nullptr) {
        const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
        fill_rect(surface, viewport, colors.paper, viewport);
        return;
    }
    if (plain_text_layout_.loaded()) {
        render_plain_text_document(surface, viewport);
        return;
    }
    if (markdown_document_ != nullptr) {
        render_markdown_document(surface, viewport);
        return;
    }
    const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
    const int page_x = kPageX - pan_x_;
    // Every render_document branch must cover the whole viewport itself;
    // render() no longer paints an inherited paper background beneath it.
    fill_rect(surface, viewport, colors.paper, viewport);

    const int stride = block_stride();
    const int first_block = std::max(0, (scroll_y_ - stride) / stride);
    const int last_block = (scroll_y_ + viewport.height + stride) / stride;
    const int line_height = body_pixel_size_ +
                            (line_gap_px_ < 0
                                 ? std::max(2, (body_pixel_size_ + 4) / 5)
                                 : line_gap_px_);

    for (int block = first_block; block <= last_block; ++block) {
        const int virtual_y = block * stride + 10;
        const int y = viewport.y + virtual_y - scroll_y_;
        const int content_x = page_x + side_margin_px_;
        const bool code_style = (block % 4) == 3;
        const int text_x = content_x + (code_style ? 6 : 0);
        const std::uint16_t block_background = code_style ? colors.code : colors.paper;
        if (code_style) {
            const Rect background{content_x,
                                  y - 3,
                                  content_width(),
                                  line_height * 4 + 8};
            fill_rect(surface, background, colors.code, viewport);
            stroke_rect(surface, background, colors.border, viewport);
        }

        if (!text_ready_ || sample_runs_.empty()) {
            for (int line = 0; line < 4; ++line) {
                fill_rect(surface,
                          {text_x, y + 4 + line * line_height,
                           180 - line * 13, 4},
                          line == 0 ? colors.accent : colors.muted_ink,
                          viewport);
            }
            continue;
        }

        for (int line = 0; line < 4; ++line) {
            const std::size_t run_index =
                static_cast<std::size_t>(block * 3 + line) % sample_runs_.size();
            text_.draw_run(surface,
                           sample_runs_[run_index],
                           text_x,
                           y + body_pixel_size_ + line * line_height,
                           fx_from_int(body_pixel_size_),
                           line == 0 ? colors.accent
                                     : (code_style ? colors.ink : colors.muted_ink),
                           block_background,
                           dark_theme_,
                           true,
                           viewport);
        }
    }
}

void Viewer::render_plain_text_document(const Surface565& surface,
                                        Rect viewport) {
    const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
    fill_rect(surface, viewport, colors.paper, viewport);
    if (plain_text_layout_.empty()) {
        if (text_ready_) {
            text_.draw_run(surface, empty_document_run_, side_margin_px_,
                           viewport.y + 38, fx_from_int(15), colors.ink,
                           colors.paper, dark_theme_, true, viewport);
        }
        return;
    }

    std::string error;
    const std::vector<PlainTextVisibleLine> visible =
        plain_text_layout_.visible_lines(error);
    const int content_x = side_margin_px_;
    for (const PlainTextVisibleLine& positioned : visible) {
        if (positioned.line == nullptr) continue;
        const LayoutLine& line = *positioned.line;
        const int baseline = viewport.y + positioned.baseline_y;
        const int ink_top = baseline - fx_ceil(line.ascent) - 1;
        const int ink_bottom =
            baseline + fx_ceil(line.descent < 0 ? -line.descent
                                               : line.descent) + 2;
        if (ink_bottom <= viewport.y ||
            ink_top >= viewport.y + viewport.height) {
            continue;
        }
        for (const LayoutRun& run : line.runs) {
            const int run_x = content_x + fx_floor(run.x);
            const bool highlighted = has_active_search_match_ &&
                source_ranges_overlap(run.source_offset, run.source_length,
                                      active_search_match_.source_offset,
                                      active_search_match_.source_length);
            if (highlighted) {
                fill_rect(surface,
                          search_highlight_rect(
                              run, active_search_match_, run_x, baseline),
                          dark_theme_ ? rgb565(103, 86, 39)
                                      : rgb565(255, 235, 145),
                          viewport);
            }
            text_.draw_run(surface, run.glyphs, run_x, baseline,
                           run.pixel_size, colors.ink, colors.paper,
                           dark_theme_, !highlighted, viewport);
        }
    }
}

void Viewer::render_markdown_document(const Surface565& surface, Rect viewport) {
    const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
    const int page_x = kPageX;
    fill_rect(surface, {page_x, viewport.y, kPageWidth, viewport.height},
              colors.paper, viewport);

    if (markdown_layout_.unit_count() == 0) {
        if (text_ready_) {
            text_.draw_run(surface, empty_document_run_, side_margin_px_,
                           viewport.y + 38, fx_from_int(15), colors.ink,
                           colors.paper, dark_theme_, true, viewport);
            text_.draw_run(surface, empty_document_hint_run_, side_margin_px_,
                           viewport.y + 65, fx_from_int(10), colors.muted_ink,
                           colors.paper, dark_theme_, true, viewport);
        }
        return;
    }

    const ViewAnchor anchor = markdown_layout_.anchor_at(fx_from_int(scroll_y_));
    std::vector<VisibleBlock> visible = markdown_layout_.layout_window(
        fx_from_int(scroll_y_), fx_from_int(viewport.height),
        kSynchronousLayoutOverscan);
    document_height_ = std::max(kViewportHeight,
                                fx_ceil(markdown_layout_.total_height()));
    // Determine whether this is the semantic final page only after measuring
    // the visible window. Estimated block heights can reveal another page;
    // pinning against the stale estimate would turn one Page Down into a jump
    // from page 1 to the newly discovered final page.
    const bool pin_final_page = pending_final_page_restore_ ||
                                scroll_y_ >= max_scroll_y();
    if (pin_final_page) {
        const std::size_t last = markdown_layout_.unit_count() - 1;
        markdown_layout_.layout_unit(last);
        document_height_ = std::max(kViewportHeight,
                                    fx_ceil(markdown_layout_.total_height()));
        const int bottom = fx_ceil(markdown_layout_.unit_top(last) +
                                   markdown_layout_.unit_height(last));
        scroll_y_ = std::max(0, bottom - kViewportHeight);
        clamp_view();
        // The first lazy window may have changed estimated heights. Build the
        // actual paint list again at the semantic final-page position.
        visible = markdown_layout_.layout_window(
            fx_from_int(scroll_y_), fx_from_int(viewport.height),
            kSynchronousLayoutOverscan);
        document_height_ = std::max(kViewportHeight,
                                    fx_ceil(markdown_layout_.total_height()));
        const int stable_bottom = fx_ceil(markdown_layout_.unit_top(last) +
                                          markdown_layout_.unit_height(last));
        scroll_y_ = std::max(0, stable_bottom - kViewportHeight);
        pending_final_page_restore_ = false;
    } else {
        // Measuring the first window can replace rough block estimates with
        // much taller real layouts.  That moves the semantic anchor, so the
        // paint list built above may no longer overlap the adjusted viewport;
        // the old behavior produced an all-paper frame until the next input.
        // Rebuild at the moved anchor until both the position and the list are
        // from the same layout state. A far TOC jump can encounter a run of
        // underestimated multi-line blocks: each bounded pass measures only
        // the newly exposed overscan, so allow enough corrective passes to
        // reach the requested block without measuring the whole document.
        for (int pass = 0; pass < 16; ++pass) {
            const int anchored = fx_floor(markdown_layout_.position_of(anchor));
            scroll_y_ = anchored;
            clamp_view();
            visible = markdown_layout_.layout_window(
                fx_from_int(scroll_y_), fx_from_int(viewport.height),
                kSynchronousLayoutOverscan);
            document_height_ = std::max(kViewportHeight,
                                        fx_ceil(markdown_layout_.total_height()));
            const int resolved = fx_floor(markdown_layout_.position_of(anchor));
            const bool anchor_is_present = std::any_of(
                visible.begin(), visible.end(), [&anchor](const VisibleBlock& block) {
                    return block.node == anchor.block && block.layout != nullptr;
                });
            if (resolved == scroll_y_ && anchor_is_present) break;
        }
        scroll_y_ = fx_floor(markdown_layout_.position_of(anchor));
    }
    clamp_view();

    for (const VisibleBlock& visible_block : visible) {
        if (visible_block.layout == nullptr) continue;
        const BlockLayout& block = *visible_block.layout;
        const BlockLayout& content_block =
            wide_focus_ && focused_code_layout_valid_ &&
                    block.node == focused_node_
                ? focused_code_layout_
                : block;
        const int y = viewport.y + fx_floor(visible_block.document_y) - scroll_y_;
        const int height = std::max(1, fx_ceil(block.height));
        if (y + height < viewport.y || y >= viewport.y + viewport.height) continue;
        const Rect block_clip = intersect(viewport, {page_x, y, kPageWidth, height});

        const int outer_content_x =
            page_x + side_margin_px_ + block.indent_px;
        const int outer_content_width = std::max(
            24, this->content_width() - static_cast<int>(block.indent_px));
        const int horizontal_inset =
            static_cast<int>(content_block.horizontal_inset_px);
        const int content_x = outer_content_x + horizontal_inset;
        const int content_width = std::max(
            24, outer_content_width - horizontal_inset * 2);
        std::uint16_t background = colors.paper;
        if (block.kind == BlockKind::TableRow) {
            background = dark_theme_ ? rgb565(33, 40, 51)
                                     : rgb565(248, 249, 251);
            fill_rect(surface,
                      {outer_content_x, y, outer_content_width, height},
                      background, viewport);
            fill_rect(surface,
                      {outer_content_x, y + height - 1,
                       outer_content_width, 1},
                      colors.border, viewport);
        }
        if (block.code_background) {
            background = colors.code;
            const Rect code_rect{outer_content_x, y + 1,
                                 outer_content_width, height - 2};
            fill_rect(surface, code_rect, background, viewport);
            stroke_rect(surface, code_rect, colors.border, viewport);
        }
        if (wide_focus_ && block.node == focused_node_) {
            stroke_rect(surface,
                        {outer_content_x, y, outer_content_width, height},
                        colors.accent,
                        viewport);
        }
        for (unsigned level = 0; level < block.quote_depth; ++level) {
            fill_rect(surface,
                      {page_x + side_margin_px_ + static_cast<int>(level) * 8,
                       y + 1, 2, height - 2},
                      colors.accent,
                      viewport);
        }
        if (block.horizontal_rule) {
            fill_rect(surface,
                      {outer_content_x, y + height / 2,
                       outer_content_width, 1},
                      colors.border,
                      viewport);
            continue;
        }

        bool formula_overflow = false;
        for (const LayoutLine& line : content_block.lines) {
            const int baseline = y + fx_floor(line.baseline_y);
            const Fx line_descent = line.descent < 0 ? -line.descent
                                                     : line.descent;
            // A plain-text chunk can contain many physical lines. Walking
            // every glyph in that chunk used to rasterize and composite up to
            // 128 off-screen lines on each page turn, even though the final
            // compositor clipped every pixel away. Cull at the shaped-line
            // boundary first. The small lower pad retains link underlines and
            // synthetic decoration at baseline + 2.
            const int line_ink_top = baseline - fx_ceil(line.ascent) - 1;
            const int line_ink_bottom = baseline + fx_ceil(line_descent) + 3;
            if (line_ink_bottom <= block_clip.y ||
                line_ink_top >= block_clip.y + block_clip.height) {
                // Overflow markers describe the focused block, including a
                // formula line just beyond the current clip. Preserve that
                // metadata scan while avoiding glyph-cache/compositor work.
                for (const LayoutRun& run : line.runs) {
                    formula_overflow = formula_overflow ||
                        (run.math != nullptr && run.math->overflow);
                }
                continue;
            }
            for (const LayoutRun& run : line.runs) {
                formula_overflow = formula_overflow ||
                    (run.math != nullptr && run.math->overflow);
                const bool link = (run.style_flags & InlineStyleLink) != 0;
                const bool math = run.source_kind == InlineKind::InlineMath ||
                                  run.source_kind == InlineKind::DisplayMath;
                const bool inline_code = !block.code_background &&
                    (run.style_flags & InlineStyleCode) != 0;
                const std::uint16_t foreground =
                    link || math || block.kind == BlockKind::Heading
                        ? colors.accent
                        : colors.ink;
                // A focused node was admitted only after an actual overflow
                // check, so pan the whole node consistently (including a wide
                // display formula), not only code/table runs.
                const int horizontal_pan = wide_focus_ &&
                                                   block.node == focused_node_
                                               ? pan_x_
                                               : 0;
                const int run_x = content_x + fx_floor(run.x) - horizontal_pan;
                const int run_width = std::max(0, fx_ceil(run.glyphs.width));
                const Fx run_descent = run.glyphs.descent < 0
                                           ? -run.glyphs.descent
                                           : run.glyphs.descent;
                if (inline_code && run_width > 0) {
                    fill_rect(surface,
                              {run_x - 2,
                               baseline - fx_ceil(run.glyphs.ascent) - 1,
                               run_width + 4,
                               std::max(3, fx_ceil(run.glyphs.ascent + run_descent) + 2)},
                              colors.code,
                              block_clip);
                }
                const bool highlighted = has_active_search_match_ &&
                    source_ranges_overlap(run.source_offset, run.source_length,
                                          active_search_match_.source_offset,
                                          active_search_match_.source_length);
                if (highlighted) {
                    fill_rect(surface,
                              search_highlight_rect(run, active_search_match_,
                                                    run_x, baseline),
                              dark_theme_ ? rgb565(103, 86, 39)
                                          : rgb565(255, 235, 145),
                              block_clip);
                }
                if (run.math != nullptr) {
                    const std::uint16_t math_color = run.math->valid
                                                         ? colors.ink
                                                         : colors.failure;
                    // run_x already includes the focused node's pan. Passing
                    // it into MathSystem as well would move formula glyphs and
                    // rules twice as far as code or table content.
                    math_.draw(surface,
                               *run.math,
                               run_x,
                               baseline,
                               0,
                               math_color,
                               background,
                               dark_theme_,
                               block_clip);
                    continue;
                }
                if (run.task_checkbox) {
                    draw_github_task_checkbox(
                        surface, run_x,
                        // The native control's visible bottom sits on the text
                        // baseline. The 13-pixel storage box includes one
                        // discarded matte row, so compensate here.
                        baseline - kGitHubCheckedCheckboxSize + 1,
                        run.task_checked,
                        task_checkbox_unchecked_tint(
                            dark_theme_, high_contrast_, false),
                        block_clip);
                    continue;
                }
                text_.draw_run(surface,
                               run.glyphs,
                               run_x,
                               baseline,
                               run.pixel_size,
                               foreground,
                               inline_code ? colors.code : background,
                               dark_theme_,
                               !highlighted,
                               block_clip,
                               static_cast<std::uint8_t>(
                                   (block.kind == BlockKind::Heading ||
                                    (run.style_flags & InlineStyleStrong) != 0
                                        ? TextSynthesisBold : TextSynthesisNone) |
                                   ((run.style_flags & InlineStyleEmphasis) != 0
                                        ? TextSynthesisItalic : TextSynthesisNone)));
                if (link) {
                    fill_rect(surface,
                              {run_x, baseline + 2, run_width, 1},
                              colors.accent,
                              block_clip);
                }
                if ((run.style_flags & InlineStyleStrikethrough) != 0) {
                    fill_rect(surface,
                              {run_x, baseline - fx_ceil(run.pixel_size) / 3,
                              run_width, 1},
                              foreground,
                              block_clip);
                }
            }
        }
        const bool structured_overflow =
            (block.code_background || block.kind == BlockKind::TableRow) &&
            content_block.maximum_line_width > fx_from_int(content_width);
        const bool focused_here = wide_focus_ && block.node == focused_node_;
        const int marker_height = std::max(1, std::min(10, height - 4));
        if ((formula_overflow || structured_overflow) &&
            (!focused_here || pan_x_ < focused_maximum_pan_)) {
            fill_rect(surface,
                      {outer_content_x + outer_content_width - 2,
                       y + 3, 2, marker_height},
                      colors.accent, viewport);
        }
        if (formula_overflow && focused_here && pan_x_ > 0) {
            fill_rect(surface,
                      {outer_content_x, y + 3, 2, marker_height},
                      colors.accent, viewport);
        }
    }
}

void Viewer::render_overlay(const Surface565& surface, bool apply_scrim) {
    const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
    const std::uint16_t scrim = rgb565(0, 0, 0);
    const Rect scrim_area = chrome_visible_
                                ? Rect{0, kHeaderHeight, kScreenWidth,
                                       kScreenHeight - kHeaderHeight}
                                : Rect{0, 0, kScreenWidth, kScreenHeight};
    if (apply_scrim) {
        for (int y = scrim_area.y; y < scrim_area.y + scrim_area.height; ++y) {
            std::uint16_t* row = surface.row(y);
            for (int x = 0; x < kScreenWidth; ++x) {
                row[x] = blend565(row[x], scrim, dark_theme_ ? 72 : 64);
            }
        }
    }
    const Rect panel = (diagnostics_overlay_ || settings_overlay_)
                           ? Rect{14, 18, 292, 204}
                       : search_overlay_ ? Rect{16, 22, 288, 196}
                       : jump_overlay_ ? Rect{36, 54, 248, 126}
                       : link_overlay_ && !link_choice_mode_
                           ? Rect{24, 56, 272, 126}
                       : (toc_overlay_ || document_browser_overlay_ ||
                          font_browser_overlay_ || link_choice_mode_)
                           ? Rect{24, 24, 272, 192}
                                                            : Rect{42, 30, 236, 166};
    fill_rect(surface, panel, colors.overlay);
    stroke_rect(surface, panel, colors.accent);
    fill_rect(surface,
              {panel.x, panel.y, panel.width, kMenuPanelHeaderHeight},
              colors.header);

    if (!text_ready_) return;
    if (font_browser_overlay_) {
        text_.draw_run(surface, font_browser_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        const std::size_t row_count = font_browser_labels_.size();
        std::size_t first = font_browser_selected_ > kMenuListVisibleRows / 2
                                ? font_browser_selected_ -
                                      kMenuListVisibleRows / 2
                                : 0;
        if (first + kMenuListVisibleRows > row_count) {
            first = row_count > kMenuListVisibleRows
                        ? row_count - kMenuListVisibleRows
                        : 0;
        }
        const std::size_t last = std::min(
            row_count, first + kMenuListVisibleRows);
        for (std::size_t index = first; index < last; ++index) {
            const int row_y = panel.y + kMenuListFirstRowY +
                              static_cast<int>(index - first) *
                                  kMenuListRowStride;
            const bool selected = index == font_browser_selected_ &&
                                  !(font_detail_open_ && index == 0);
            const std::uint16_t background = selected ? colors.accent : colors.overlay;
            if (selected) {
                fill_rect(surface,
                          {panel.x + 5, row_y, panel.width - 10,
                           kMenuListRowHeight},
                          background, panel);
                fill_rect(surface, {panel.x + 7, row_y + 5, 2, 13},
                          rgb565(250, 252, 255), panel);
            }
            GlyphRun visible_run;
            const GlyphRun* run = nullptr;
            int pixel_size = kMenuListPixelSize;
            const std::string& label = font_browser_labels_[index];
            if (font_detail_open_ && index == 0) {
                pixel_size = kMenuCompactPixelSize;
            }
            int label_x = panel.x + 10;
            const bool role_row = font_detail_open_ && index >= 1 &&
                                  index <= kExternalFontRoleCount;
            if (role_row && font_detail_index_ < font_file_catalog_.size()) {
                const std::size_t role_index = index - 1;
                const bool assigned =
                    pending_font_paths_[role_index] ==
                    font_file_catalog_[font_detail_index_].path;
                draw_github_task_checkbox(
                    surface, panel.x + 11, row_y + 5,
                    assigned,
                    task_checkbox_unchecked_tint(
                        dark_theme_, high_contrast_, selected),
                    panel);
                label_x = panel.x + 26;
            }
            text_.shape(label.data(), label.size(), fx_from_int(pixel_size),
                        visible_run);
            run = &visible_run;
            text_.draw_run(surface, *run, label_x,
                           row_y + kMenuListBaseline, fx_from_int(pixel_size),
                           selected ? rgb565(250, 252, 255) : colors.ink,
                           background, dark_theme_, true, panel);
        }
        draw_list_overflow_hints(surface, panel, colors.accent,
                                 panel.y + kMenuListFirstRowY,
                                 first > 0, last < row_count);
        return;
    }
    if (document_browser_overlay_) {
        text_.draw_run(surface, document_browser_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        std::size_t first = document_browser_selected_ > kMenuListVisibleRows / 2
                                ? document_browser_selected_ -
                                      kMenuListVisibleRows / 2
                                : 0;
        if (first + kMenuListVisibleRows > document_browser_runs_.size()) {
            first = document_browser_runs_.size() > kMenuListVisibleRows
                        ? document_browser_runs_.size() - kMenuListVisibleRows
                        : 0;
        }
        const std::size_t last = std::min(document_browser_runs_.size(),
                                          first + kMenuListVisibleRows);
        for (std::size_t index = first; index < last; ++index) {
            const int row_y = panel.y + kMenuListFirstRowY +
                              static_cast<int>(index - first) *
                                  kMenuListRowStride;
            const bool selected = !document_browser_paths_.empty() &&
                                  index == document_browser_selected_;
            const std::uint16_t background = selected ? colors.accent : colors.overlay;
            if (selected) {
                fill_rect(surface,
                          {panel.x + 5, row_y, panel.width - 10,
                           kMenuListRowHeight},
                          background, panel);
                fill_rect(surface, {panel.x + 7, row_y + 5, 2, 13},
                          rgb565(250, 252, 255), panel);
            }
            text_.draw_run(surface, document_browser_runs_[index], panel.x + 10,
                           row_y + kMenuListBaseline,
                           fx_from_int(kMenuListPixelSize),
                           selected ? rgb565(250, 252, 255) : colors.ink,
                           background, dark_theme_, true, panel);
        }
        draw_list_overflow_hints(surface, panel, colors.accent,
                                 panel.y + kMenuListFirstRowY,
                                 first > 0, last < document_browser_runs_.size());
        return;
    }
    if (diagnostics_overlay_) {
        text_.draw_run(surface, diagnostics_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        const int row_stride = diagnostics_runs_.size() > 9 ? 16 : 18;
        for (std::size_t index = 0; index < diagnostics_runs_.size(); ++index) {
            text_.draw_run(surface, diagnostics_runs_[index], panel.x + 9,
                           panel.y + 41 +
                               static_cast<int>(index) * row_stride,
                           fx_from_int(kMenuAuxiliaryPixelSize),
                           colors.ink, colors.overlay,
                           dark_theme_, true, panel);
        }
        return;
    }
    if (link_overlay_) {
        text_.draw_run(surface, link_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        if (link_choice_mode_) {
            std::size_t first = link_choice_selected_ > kMenuListVisibleRows / 2
                                    ? link_choice_selected_ -
                                          kMenuListVisibleRows / 2
                                    : 0;
            if (first + kMenuListVisibleRows > link_choice_runs_.size()) {
                first = link_choice_runs_.size() > kMenuListVisibleRows
                            ? link_choice_runs_.size() - kMenuListVisibleRows
                            : 0;
            }
            const std::size_t last = std::min(link_choice_runs_.size(),
                                               first + kMenuListVisibleRows);
            for (std::size_t index = first; index < last; ++index) {
                const int row_y = panel.y + kMenuListFirstRowY +
                                  static_cast<int>(index - first) *
                                      kMenuListRowStride;
                const bool selected = index == link_choice_selected_;
                const std::uint16_t background = selected
                                                     ? colors.accent
                                                     : colors.overlay;
                if (selected) {
                    fill_rect(surface,
                              {panel.x + 5, row_y, panel.width - 10,
                               kMenuListRowHeight},
                              background, panel);
                    fill_rect(surface, {panel.x + 7, row_y + 5, 2, 13},
                              rgb565(250, 252, 255), panel);
                }
                text_.draw_run(surface, link_choice_runs_[index], panel.x + 10,
                               row_y + kMenuListBaseline,
                               fx_from_int(kMenuCompactPixelSize),
                               selected ? rgb565(250, 252, 255) : colors.ink,
                               background, dark_theme_, true, panel);
            }
            draw_list_overflow_hints(surface, panel, colors.accent,
                                     panel.y + kMenuListFirstRowY,
                                     first > 0, last < link_choice_runs_.size());
            return;
        }
        fill_rect(surface, {panel.x + 8, panel.y + 34, panel.width - 16, 48},
                  colors.paper, panel);
        stroke_rect(surface, {panel.x + 8, panel.y + 34, panel.width - 16, 48},
                    colors.border, panel);
        // A single body line sits centered in the box; two wrapped lines
        // share it with an 18 px stride.
        const int first_body_baseline =
            link_target_runs_.size() > 1 ? 52 : 61;
        for (std::size_t line = 0; line < link_target_runs_.size();
             ++line) {
            text_.draw_run(surface, link_target_runs_[line], panel.x + 13,
                           panel.y + first_body_baseline +
                               static_cast<int>(line) * 18,
                           fx_from_int(kMenuCompactPixelSize),
                           colors.ink, colors.paper,
                           dark_theme_, true, panel);
        }
        text_.draw_run(surface, link_hint_run_, panel.x + 10, panel.y + 105,
                       fx_from_int(kMenuAuxiliaryPixelSize),
                       colors.muted_ink, colors.overlay,
                       dark_theme_, true, panel);
        return;
    }
    if (search_overlay_) {
        text_.draw_run(surface, search_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        fill_rect(surface, {panel.x + 7, panel.y + 30, panel.width - 14, 27},
                  colors.paper, panel);
        stroke_rect(surface, {panel.x + 7, panel.y + 30, panel.width - 14, 27},
                    colors.border, panel);
        text_.draw_run(surface, search_query_run_, panel.x + 12, panel.y + 49,
                       fx_from_int(kMenuSearchInputPixelSize),
                       colors.ink, colors.paper,
                       dark_theme_, true, panel);
        text_.draw_run(surface, search_status_run_, panel.x + 10, panel.y + 70,
                       fx_from_int(kMenuAuxiliaryPixelSize),
                       colors.muted_ink, colors.overlay,
                       dark_theme_, true, panel);

        std::size_t first = search_selected_ > 1 ? search_selected_ - 1 : 0;
        if (first + kSearchVisibleRows > search_result_runs_.size()) {
            first = search_result_runs_.size() > kSearchVisibleRows
                        ? search_result_runs_.size() - kSearchVisibleRows
                        : 0;
        }
        const std::size_t last = std::min(search_result_runs_.size(),
                                          first + kSearchVisibleRows);
        for (std::size_t index = first; index < last; ++index) {
            const int row = static_cast<int>(index - first);
            const int row_y = panel.y + kSearchFirstRowY +
                              row * kSearchRowStride;
            const bool selected = index == search_selected_;
            const std::uint16_t row_background = selected ? colors.accent
                                                           : colors.overlay;
            if (selected) {
                fill_rect(surface,
                          {panel.x + 6, row_y, panel.width - 12,
                           kSearchRowHeight},
                          row_background, panel);
                fill_rect(surface, {panel.x + 8, row_y + 5, 2, 14},
                          rgb565(250, 252, 255), panel);
            }
            text_.draw_run(surface, search_result_runs_[index], panel.x + 10,
                           row_y + 17, fx_from_int(kMenuCompactPixelSize),
                           selected ? rgb565(250, 252, 255) : colors.ink,
                           row_background, dark_theme_, true, panel);
        }
        draw_list_overflow_hints(surface, panel, colors.accent,
                                 panel.y + kSearchFirstRowY,
                                 first > 0, last < search_result_runs_.size());
        return;
    }
    if (jump_overlay_) {
        text_.draw_run(surface, jump_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        fill_rect(surface, {panel.x + 10, panel.y + 38,
                            panel.width - 20, 31},
                  colors.paper, panel);
        stroke_rect(surface, {panel.x + 10, panel.y + 38,
                              panel.width - 20, 31},
                    colors.border, panel);
        text_.draw_run(surface, jump_query_run_, panel.x + 16, panel.y + 60,
                       fx_from_int(kMenuSearchInputPixelSize),
                       colors.ink, colors.paper,
                       dark_theme_, true, panel);
        text_.draw_run(surface, jump_hint_run_, panel.x + 12, panel.y + 94,
                       fx_from_int(kMenuAuxiliaryPixelSize),
                       colors.muted_ink, colors.overlay,
                       dark_theme_, true, panel);
        return;
    }
    if (settings_overlay_) {
        text_.draw_run(surface, settings_title_, panel.x + 10, panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header,
                       dark_theme_, true, panel);
        const std::size_t first =
            settings_selected_ >= kSettingsVisibleRows
                ? settings_selected_ + 1 - kSettingsVisibleRows
                : 0;
        const std::size_t last = std::min(
            settings_runs_.size(), first + kSettingsVisibleRows);
        for (std::size_t index = first; index < last; ++index) {
            const int row_y = panel.y + kSettingsFirstRowY +
                              static_cast<int>(index - first) *
                                  kSettingsRowStride;
            const bool selected = index == settings_selected_;
            const std::uint16_t background = selected ? colors.accent : colors.overlay;
            if (selected) {
                fill_rect(surface,
                          {panel.x + 7, row_y, panel.width - 14,
                           kSettingsRowHeight},
                          background, panel);
                fill_rect(surface, {panel.x + 9, row_y + 4, 2, 10},
                          rgb565(250, 252, 255), panel);
            }
            text_.draw_run(surface, settings_runs_[index], panel.x + 13,
                           row_y + kSettingsBaseline,
                           fx_from_int(kMenuCompactPixelSize),
                           selected ? rgb565(250, 252, 255) : colors.ink,
                           background, dark_theme_, true, panel);
        }
        draw_list_overflow_hints(surface, panel, colors.accent,
                                 panel.y + kSettingsFirstRowY,
                                 first > 0, last < settings_runs_.size());
        return;
    }
    // Bookmarks are independent of the Markdown section system, so the list
    // renders for plain-text documents as well; only the sections tab needs
    // a Markdown document behind it.
    if (toc_overlay_ &&
        (markdown_document_ != nullptr || plain_text_layout_.loaded())) {
        const bool show_bookmarks = bookmark_tab_;
        const std::vector<GlyphRun>& rows = show_bookmarks ? bookmark_runs_ : toc_runs_;
        const std::size_t selected_index = show_bookmarks ? bookmark_selected_ : toc_selected_;
        const GlyphRun& title = show_bookmarks ? bookmark_title_ : toc_title_;
        text_.draw_run(surface,
                       title,
                       panel.x + 10,
                       panel.y + 17,
                       fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252),
                       colors.header,
                       dark_theme_,
                       true,
                       panel);
        if (show_bookmarks) {
            // Key instructions live right-aligned in the header so the
            // management actions are discoverable without a help page.
            text_.draw_run(surface, bookmark_hint_run_,
                           panel.x + panel.width - 10 -
                               fx_ceil(bookmark_hint_run_.width),
                           panel.y + 16,
                           fx_from_int(kMenuAuxiliaryPixelSize),
                           rgb565(196, 208, 226), colors.header,
                           dark_theme_, true, panel);
        }
        if (!bookmarks_.empty()) {
            const int tab_y = panel.y + 22;
            fill_rect(surface,
                      {show_bookmarks ? panel.x + panel.width - 62 : panel.x + 6,
                       tab_y, 56, 2},
                      colors.accent, panel);
        }
        std::size_t first = selected_index > kMenuListVisibleRows / 2
                                ? selected_index - kMenuListVisibleRows / 2
                                : 0;
        if (first + kMenuListVisibleRows > rows.size()) {
            first = rows.size() > kMenuListVisibleRows
                        ? rows.size() - kMenuListVisibleRows
                        : 0;
        }
        const std::size_t last = std::min(
            rows.size(), first + kMenuListVisibleRows);
        for (std::size_t index = first; index < last; ++index) {
            const int row = static_cast<int>(index - first);
            const int row_y = panel.y + kMenuListFirstRowY +
                              row * kMenuListRowStride;
            const bool selected = index == selected_index;
            if (selected) {
                fill_rect(surface,
                          {panel.x + 5, row_y, panel.width - 10,
                           kMenuListRowHeight},
                          colors.accent,
                          panel);
                fill_rect(surface, {panel.x + 7, row_y + 5, 2, 13},
                          rgb565(250, 252, 255), panel);
            }
            const unsigned level =
                show_bookmarks || markdown_document_ == nullptr
                    ? 1U
                    : markdown_document_->ir.headings[index].level;
            text_.draw_run(surface,
                           rows[index],
                           panel.x + 10 +
                               static_cast<int>(std::min(5U,
                                   level > 0 ? level - 1U : 0U)) * 9,
                           row_y + kMenuListBaseline,
                           fx_from_int(kMenuListPixelSize),
                           selected ? rgb565(250, 252, 255) : colors.ink,
                           selected ? colors.accent : colors.overlay,
                           dark_theme_,
                           true,
                           panel);
        }
        draw_list_overflow_hints(surface, panel, colors.accent,
                                 panel.y + kMenuListFirstRowY,
                                 first > 0, last < rows.size());
        if (rows.empty()) {
            const GlyphRun& empty_message = show_bookmarks
                                                ? bookmark_empty_run_
                                                : toc_empty_run_;
            text_.draw_run(surface, empty_message, panel.x + 12, panel.y + 55,
                           fx_from_int(kMenuCompactPixelSize),
                           colors.muted_ink, colors.overlay,
                           dark_theme_, true, panel);
        }
        return;
    }

    if (overlay_runs_.size() != 5) return;
    text_.draw_run(surface,
                   overlay_runs_[0],
                   panel.x + 12,
                   panel.y + 17,
                   fx_from_int(kMenuListPixelSize),
                   rgb565(246, 248, 252),
                   colors.header,
                   dark_theme_,
                   true,
                   panel);
    for (std::size_t line = 1; line < overlay_runs_.size(); ++line) {
        text_.draw_run(surface,
                       overlay_runs_[line],
                       panel.x + 14,
                       panel.y + 43 + static_cast<int>(line - 1) * 25,
                       fx_from_int(kMenuListPixelSize),
                       colors.ink,
                       colors.overlay,
                       dark_theme_,
                       true,
                       panel);
    }
}

void Viewer::render_loading_feedback(const Surface565& surface,
                                     bool apply_scrim) {
    if (!loading_feedback_visible_) return;
    const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
    if (apply_scrim) {
        const std::uint16_t scrim = rgb565(0, 0, 0);
        for (int y = 0; y < kScreenHeight; ++y) {
            std::uint16_t* row = surface.row(y);
            for (int x = 0; x < kScreenWidth; ++x) {
                row[x] = blend565(row[x], scrim, dark_theme_ ? 72 : 64);
            }
        }
    }

    // A compact, non-interactive status card. File reads report determinate
    // byte progress; directory scans and parse/layout stages use a moving
    // indeterminate segment.
    const Rect panel{28, 82, 264, 76};
    fill_rect(surface, panel, colors.overlay);
    fill_rect(surface,
              {panel.x, panel.y, panel.width, kMenuPanelHeaderHeight},
              colors.header, panel);
    if (text_ready_) {
        text_.draw_run(surface, loading_title_run_, panel.x + 11,
                       panel.y + 17, fx_from_int(kMenuTitlePixelSize),
                       rgb565(246, 248, 252), colors.header, dark_theme_, true,
                       panel);
        text_.draw_run(surface, loading_detail_run_, panel.x + 12,
                       panel.y + 48, fx_from_int(kMenuAuxiliaryPixelSize),
                       colors.ink, colors.overlay, dark_theme_, true, panel);
    }
    const Rect track{panel.x + 12, panel.y + 61, panel.width - 24, 4};
    fill_rect(surface, track, colors.border, panel);
    if (loading_feedback_progress_ >= 0) {
        const int width =
            track.width * loading_feedback_progress_ / 100;
        if (width > 0) {
            fill_rect(surface,
                      {track.x, track.y, width, track.height},
                      colors.accent, panel);
        }
    } else {
        constexpr int kSegmentWidth = 52;
        const int travel = std::max(1, track.width - kSegmentWidth);
        const int x = track.x +
            static_cast<int>(loading_feedback_phase_) * travel / 2;
        fill_rect(surface,
                  {x, track.y, kSegmentWidth, track.height},
                  colors.accent, panel);
    }
    stroke_rect(surface, panel, colors.accent);
}

void Viewer::render(const Surface565& surface) {
    if (!surface.valid()) {
        return;
    }
    std::uint16_t* const render_surface = surface.row(0);
    const bool same_surface = full_frame_available_ &&
                              render_surface == last_render_surface_;
    if (loading_feedback_visible_ && full_frame_available_ &&
        same_surface) {
        // A loading operation can begin immediately after a modal closes.
        // Restore the clean document frame before painting the first loading
        // card, then advance only the small card for later animation phases.
        if (!loading_feedback_painted_) {
            if (retained_base_frame_valid_) {
                restore_retained_base_frame(surface);
            } else if (display_surface_is_base_frame_) {
                capture_retained_base_frame(surface);
            }
        }
        render_loading_feedback(surface, !loading_feedback_painted_);
        loading_feedback_painted_ = true;
        display_surface_is_base_frame_ = false;
        ++retained_frame_fast_path_count_;
        return;
    }
    if (settings_overlay_ && settings_overlay_repaint_only_ &&
        same_surface) {
        // Settings navigation and value edits redraw only the opaque panel.
        // The existing scrim and document remain untouched until the menu
        // closes, when commit_settings_session() performs one full refresh.
        render_overlay(surface, false);
        settings_overlay_repaint_only_ = false;
        return;
    }

    if (same_surface && retained_base_frame_valid_) {
        // Modal navigation starts from an unmodified base every time, avoiding
        // another document layout/glyph pass and preventing repeated scrims
        // from accumulating. Closing the final modal restores the exact frame.
        restore_retained_base_frame(surface);
        if (overlay_open_) {
            render_overlay(surface);
            display_surface_is_base_frame_ = false;
        } else {
            display_surface_is_base_frame_ = true;
            retained_base_frame_valid_ = false;
        }
        last_render_surface_ = render_surface;
        full_frame_available_ = true;
        settings_overlay_repaint_only_ = false;
        ++retained_frame_fast_path_count_;
        return;
    }

    if (same_surface && overlay_open_ && display_surface_is_base_frame_ &&
        capture_retained_base_frame(surface)) {
        // First modal opening: snapshot the already presented clean frame and
        // paint only the overlay. Ordinary reading never pays this copy cost.
        render_overlay(surface);
        display_surface_is_base_frame_ = false;
        last_render_surface_ = render_surface;
        full_frame_available_ = true;
        settings_overlay_repaint_only_ = false;
        ++retained_frame_fast_path_count_;
        return;
    }

    const ThemeColors colors = theme_colors(dark_theme_, high_contrast_);
    // Full-frame coverage without a whole-surface clear: the document
    // viewport starts below the filename bar while it is visible and owns
    // the full height once reading forward hides it, the bar fills the top
    // rows in the former case, and the two-row strip at the bottom stays
    // outside the document clip so glyphs can never touch the physical
    // bottom edge of the screen. Together these paint every pixel, so the
    // presented frame and the retained base snapshot never see stale data.
    // Scroll geometry always uses the full-height kViewportHeight; a
    // visible bar only clips the bottom of the drawn window, so layout
    // never reflows with bar visibility. With the bar hidden the content
    // starts below the progress strip plus its two-pixel margin.
    const int content_top = chrome_visible_ ? kHeaderHeight
                                            : kTopContentInset;
    const Rect viewport{0, content_top, kScreenWidth,
                        kScreenHeight - kBottomContentInset - content_top};
    render_document(surface, viewport);
    if (!chrome_visible_) {
        fill_rect(surface,
                  {0, 0, kScreenWidth, kTopContentInset},
                  colors.paper);
    }
    fill_rect(surface,
              {0, kScreenHeight - kBottomContentInset,
               kScreenWidth, kBottomContentInset},
              colors.paper);

#if defined(NMARKDOWN_FIREBIRD_PROGRESS_FIXTURE)
    {
        const int page = current_page();
        const int page_count = total_pages();
        std::printf(
            "NMARKDOWN_IT/1 READING_PROGRESS mode=%s scroll=%d max=%d "
            "page=%d total=%d width=%d\n",
            reading_mode_ == ReadingMode::HorizontalScroll ? "horizontal"
                                                           : "vertical",
            scroll_y_, max_scroll_y(), page, page_count,
            reading_progress_width());
        std::fflush(stdout);
    }
#endif
    // The title bar hides while reading forward, but the two-pixel reading
    // progress strip stays as a permanent, unobtrusive overlay.
    if (chrome_visible_) {
        fill_rect(surface, {0, 0, kScreenWidth, kHeaderHeight},
                  colors.header);
    }
    // Markdown layout is populated lazily while painting the visible units,
    // so continuous document progress is calculated after that pass.
    const std::uint16_t progress_color =
        document_error_
            ? colors.failure
            : (document_loaded_ ? colors.success : colors.accent);
    fill_rect(surface,
              {0, 0, kScreenWidth, kProgressHeight},
              colors.border);
    fill_rect(surface,
              {0, 0, reading_progress_width(), kProgressHeight},
              progress_color);
    if (chrome_visible_ && text_ready_) {
        const int title_clip_right = kScreenWidth - kChromeRightPadding;
        rebuild_chrome_title(std::max(0, title_clip_right - kChromeX));
        text_.draw_run(surface,
                       chrome_title_,
                       kChromeX,
                       kChromeBaseline,
                       fx_from_int(13),
                       rgb565(246, 248, 252),
                       colors.header,
                       dark_theme_,
                       true,
                       {0, kProgressHeight,
                        title_clip_right,
                        kHeaderHeight - kProgressHeight});
    }

    if (markdown_document_ != nullptr && markdown_layout_.unit_count() != 0) {
        const ViewAnchor anchor = markdown_layout_.anchor_at(fx_from_int(scroll_y_));
        if (std::find(bookmarks_.begin(), bookmarks_.end(), anchor.source_offset) !=
            bookmarks_.end()) {
            fill_rect(surface, {4, kScreenHeight - 8, 6, 5}, colors.accent);
        }
    } else if (plain_text_layout_.loaded() &&
               std::find(bookmarks_.begin(), bookmarks_.end(),
                         plain_text_layout_.current_source_offset()) !=
                   bookmarks_.end()) {
        fill_rect(surface, {4, kScreenHeight - 8, 6, 5}, colors.accent);
    }

    retained_base_frame_valid_ = false;
    display_surface_is_base_frame_ = true;
    if (overlay_open_ || loading_feedback_visible_) {
        capture_retained_base_frame(surface);
    }
    if (overlay_open_) {
        render_overlay(surface);
        display_surface_is_base_frame_ = false;
    }
    if (loading_feedback_visible_) {
        render_loading_feedback(surface);
        loading_feedback_painted_ = true;
        display_surface_is_base_frame_ = false;
    }
    last_render_surface_ = render_surface;
    full_frame_available_ = true;
    settings_overlay_repaint_only_ = false;
}

}  // namespace nmarkdown
