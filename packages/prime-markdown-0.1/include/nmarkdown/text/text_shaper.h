#ifndef NMARKDOWN_TEXT_TEXT_SHAPER_H
#define NMARKDOWN_TEXT_TEXT_SHAPER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nmarkdown/layout/fixed.h"
#include "nmarkdown/text/font.h"

namespace nmarkdown {

struct PositionedGlyph {
    FontFaceId face = 0;
    GlyphId glyph = 0;
    Fx x_advance = 0;
    Fx y_advance = 0;
    Fx x_offset = 0;
    Fx y_offset = 0;
    std::uint32_t source_cluster = 0;
    std::uint32_t codepoint = 0;
    FontRole resolved_role = FontRole::BodySans;
    bool substituted = false;
    // True when this output glyph is, or contains after composition, a
    // combining-mark source codepoint.
    bool combining_mark = false;
};

struct GlyphRun {
    std::vector<PositionedGlyph> glyphs;
    Fx width = 0;
    Fx ascent = 0;
    Fx descent = 0;
    std::size_t invalid_sequence_count = 0;
    std::size_t substitution_count = 0;
    std::size_t unsupported_script_count = 0;
};

// Natural spacing uses the font's OpenType advances and positioning. Tracked
// spacing adds one physical pixel between shaped clusters, never between the
// glyphs that make up a cluster and never after the final cluster.
enum class TextSpacing : std::uint8_t {
    Natural,
    Tracked,
};

class TextShaper {
public:
    virtual ~TextShaper() = default;
    virtual bool shape(const std::uint8_t* utf8,
                       std::size_t size,
                       Fx pixel_size,
                       GlyphRun& output,
                       FontRole preferred_role = FontRole::BodySans,
                       TextSpacing spacing = TextSpacing::Natural) const = 0;
    virtual bool shape_math_stretchy(std::uint32_t codepoint,
                                     Fx pixel_size,
                                     Fx target_height,
                                     GlyphRun& output,
                                     Fx& render_pixel_size) const = 0;
};

}  // namespace nmarkdown

#endif
