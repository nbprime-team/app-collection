#ifndef NMARKDOWN_MATH_MATH_LAYOUT_H
#define NMARKDOWN_MATH_MATH_LAYOUT_H

#include <cstdint>
#include <string>
#include <vector>

#include "nmarkdown/layout/fixed.h"
#include "nmarkdown/math/math_atoms.h"
#include "nmarkdown/text/text_shaper.h"

namespace nmarkdown {

class TextSystem;

enum class MathStyle : std::uint8_t {
    Display,
    Text,
    Script,
    ScriptScript,
};

struct MathFontConstants {
    // 16.16 em-relative constants. These tuned defaults are used when the
    // bundled font has no OpenType MATH table.
    std::int32_t axis_height = 16384;
    std::int32_t fraction_rule = 4096;
    std::int32_t fraction_num_gap = 13107;
    std::int32_t fraction_den_gap = 13107;
    std::int32_t superscript_shift = 39322;
    std::int32_t subscript_shift = 19661;
    std::int32_t radical_rule = 4096;
    std::int32_t radical_gap = 9830;
};

struct MathBoxMetrics {
    Fx width = 0;
    Fx ascent = 0;
    Fx descent = 0;
};

struct MathDrawRun {
    GlyphRun glyphs;
    Fx x = 0;
    Fx baseline_y = 0;
    Fx pixel_size = 0;
    MathVariant variant = MathVariant::Normal;
};

struct MathRule {
    Fx x = 0;
    Fx y = 0;
    Fx width = 0;
    Fx height = 0;
    std::uint8_t coverage = 255;
};

struct MathLayoutResult {
    MathBoxMetrics metrics;
    std::vector<MathDrawRun> runs;
    std::vector<MathRule> rules;
    std::string diagnostic;
    bool valid = true;
    bool overflow = false;
};

bool layout_math_tree(const MathTree& tree,
                      TextSystem& text,
                      MathStyle style,
                      Fx base_pixel_size,
                      Fx maximum_width,
                      const MathFontConstants& constants,
                      MathLayoutResult& result);

}  // namespace nmarkdown

#endif
