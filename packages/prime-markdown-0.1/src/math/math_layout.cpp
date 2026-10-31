#include "nmarkdown/math/math_layout.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "nmarkdown/document/utf8.h"
#include "nmarkdown/text/text_system.h"

namespace nmarkdown {
namespace {

struct Box {
    MathBoxMetrics metrics;
    AtomClass atom_class = AtomClass::Ordinary;
    std::vector<MathDrawRun> runs;
    std::vector<MathRule> rules;
};

Fx em_value(Fx pixel_size, std::int32_t value_16_16) {
    return static_cast<Fx>(static_cast<std::int64_t>(pixel_size) * value_16_16 / 65536);
}

Fx style_size(Fx base, MathStyle style) {
    switch (style) {
    case MathStyle::Script: return std::max<Fx>(fx_from_int(6), base * 75 / 100);
    case MathStyle::ScriptScript: return std::max<Fx>(fx_from_int(6), base * 60 / 100);
    case MathStyle::Display:
    case MathStyle::Text:
    default: return base;
    }
}

MathStyle fraction_child_style(MathStyle style) {
    switch (style) {
    case MathStyle::Display: return MathStyle::Text;
    case MathStyle::Text: return MathStyle::Script;
    case MathStyle::Script:
    case MathStyle::ScriptScript: return MathStyle::ScriptScript;
    }
    return MathStyle::Script;
}

MathStyle script_child_style(MathStyle style) {
    switch (style) {
    case MathStyle::Display:
    case MathStyle::Text: return MathStyle::Script;
    case MathStyle::Script:
    case MathStyle::ScriptScript: return MathStyle::ScriptScript;
    }
    return MathStyle::Script;
}

void append(Box& destination, Box source, Fx x, Fx baseline_y) {
    for (MathDrawRun& run : source.runs) {
        run.x += x;
        run.baseline_y += baseline_y;
        destination.runs.push_back(std::move(run));
    }
    for (MathRule& rule : source.rules) {
        rule.x += x;
        rule.y += baseline_y;
        destination.rules.push_back(rule);
    }
}

Fx positive_descent(Fx descent) {
    return descent < 0 ? -descent : descent;
}

bool variable_symbol(std::string_view value) {
    if (value.empty()) return false;
    const DecodedCodepoint decoded = utf8_next(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size(), 0);
    if (!decoded.valid || decoded.byte_length != value.size()) return false;
    const std::uint32_t codepoint = decoded.value;
    return (codepoint >= 'A' && codepoint <= 'Z') ||
           (codepoint >= 'a' && codepoint <= 'z') ||
           (codepoint >= 0x03B1U && codepoint <= 0x03F3U) ||
           codepoint == 0x03F5U;
}

int greek_variant_index(std::uint32_t codepoint) {
    switch (codepoint) {
    case 0x03F5U: return 0;  // epsilon symbol
    case 0x03D1U: return 1;  // theta symbol
    case 0x03F0U: return 2;  // kappa symbol
    case 0x03D5U: return 3;  // phi symbol
    case 0x03F1U: return 4;  // rho symbol
    case 0x03D6U: return 5;  // pi symbol
    default: return -1;
    }
}

std::uint32_t styled_codepoint(std::uint32_t codepoint, MathVariant variant) {
    if (variant == MathVariant::Italic) {
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D434U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            // Mathematical italic small h uses the Planck-constant codepoint;
            // U+1D455 is intentionally unassigned.
            return codepoint == 'h' ? 0x210EU
                                    : 0x1D44EU + codepoint - 'a';
        }
        if (codepoint >= 0x0391U && codepoint <= 0x03A1U) {
            return 0x1D6E2U + codepoint - 0x0391U;
        }
        if (codepoint >= 0x03A3U && codepoint <= 0x03A9U) {
            return 0x1D6E2U + codepoint - 0x0391U;
        }
        if (codepoint >= 0x03B1U && codepoint <= 0x03C9U) {
            return 0x1D6FCU + codepoint - 0x03B1U;
        }
        const int variant_index = greek_variant_index(codepoint);
        if (variant_index >= 0) {
            return 0x1D716U + static_cast<std::uint32_t>(variant_index);
        }
    }
    if (variant == MathVariant::Bold) {
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D400U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            return 0x1D41AU + codepoint - 'a';
        }
        if (codepoint >= '0' && codepoint <= '9') {
            return 0x1D7CEU + codepoint - '0';
        }
        if (codepoint >= 0x0391U && codepoint <= 0x03A1U) {
            return 0x1D6A8U + codepoint - 0x0391U;
        }
        if (codepoint >= 0x03A3U && codepoint <= 0x03A9U) {
            return 0x1D6A8U + codepoint - 0x0391U;
        }
        if (codepoint >= 0x03B1U && codepoint <= 0x03C9U) {
            return 0x1D6C2U + codepoint - 0x03B1U;
        }
        const int variant_index = greek_variant_index(codepoint);
        if (variant_index >= 0) {
            return 0x1D6DCU + static_cast<std::uint32_t>(variant_index);
        }
    }
    if (variant == MathVariant::BoldItalic) {
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D468U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            return 0x1D482U + codepoint - 'a';
        }
        // Unicode has no separate bold-italic digit alphabet. TeX keeps
        // digits upright while applying the requested bold weight.
        if (codepoint >= '0' && codepoint <= '9') {
            return 0x1D7CEU + codepoint - '0';
        }
        if (codepoint >= 0x0391U && codepoint <= 0x03A1U) {
            return 0x1D71CU + codepoint - 0x0391U;
        }
        if (codepoint >= 0x03A3U && codepoint <= 0x03A9U) {
            return 0x1D71CU + codepoint - 0x0391U;
        }
        if (codepoint >= 0x03B1U && codepoint <= 0x03C9U) {
            return 0x1D736U + codepoint - 0x03B1U;
        }
        const int variant_index = greek_variant_index(codepoint);
        if (variant_index >= 0) {
            return 0x1D750U + static_cast<std::uint32_t>(variant_index);
        }
    }
    if (variant == MathVariant::Blackboard) {
        switch (codepoint) {
        case 'C': return 0x2102U;
        case 'H': return 0x210DU;
        case 'N': return 0x2115U;
        case 'P': return 0x2119U;
        case 'Q': return 0x211AU;
        case 'R': return 0x211DU;
        case 'Z': return 0x2124U;
        default: break;
        }
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D538U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            return 0x1D552U + codepoint - 'a';
        }
        if (codepoint >= '0' && codepoint <= '9') {
            return 0x1D7D8U + codepoint - '0';
        }
    }
    if (variant == MathVariant::Calligraphic) {
        switch (codepoint) {
        case 'B': return 0x212CU;
        case 'E': return 0x2130U;
        case 'F': return 0x2131U;
        case 'H': return 0x210BU;
        case 'I': return 0x2110U;
        case 'L': return 0x2112U;
        case 'M': return 0x2133U;
        case 'R': return 0x211BU;
        default: break;
        }
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D49CU + codepoint - 'A';
        }
        // Latin Modern Math intentionally provides the traditional
        // calligraphic uppercase alphabet only. Its lowercase script cmap is
        // empty, so use the font's native mathematical italic lowercase
        // rather than emitting a visible .notdef box.
        if (codepoint >= 'a' && codepoint <= 'z') {
            return codepoint == 'h' ? 0x210EU
                                    : 0x1D44EU + codepoint - 'a';
        }
    }
    if (variant == MathVariant::SansSerif) {
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D5A0U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            return 0x1D5BAU + codepoint - 'a';
        }
        if (codepoint >= '0' && codepoint <= '9') {
            return 0x1D7E2U + codepoint - '0';
        }
    }
    if (variant == MathVariant::Monospace) {
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D670U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            return 0x1D68AU + codepoint - 'a';
        }
        if (codepoint >= '0' && codepoint <= '9') {
            return 0x1D7F6U + codepoint - '0';
        }
    }
    if (variant == MathVariant::Fraktur) {
        switch (codepoint) {
        case 'C': return 0x212DU;
        case 'H': return 0x210CU;
        case 'I': return 0x2111U;
        case 'R': return 0x211CU;
        case 'Z': return 0x2128U;
        default: break;
        }
        if (codepoint >= 'A' && codepoint <= 'Z') {
            return 0x1D504U + codepoint - 'A';
        }
        if (codepoint >= 'a' && codepoint <= 'z') {
            return 0x1D51EU + codepoint - 'a';
        }
    }
    return codepoint;
}

int spacing_mu(AtomClass left, AtomClass right) {
    // TeX's eight math-atom spacing table. Values are mu units: thin=3,
    // medium=4, and thick=5. Binary atoms are normalized before this table is
    // consulted, so cells that TeX marks impossible safely remain zero.
    constexpr std::uint8_t table[8][8] = {
        // Ord Op  Bin Rel Open Close Punct Inner
        {0,   3,  4,  5,  0,   0,    0,    3},  // Ord
        {3,   3,  0,  5,  0,   0,    0,    3},  // Op
        {4,   4,  0,  0,  4,   0,    0,    4},  // Bin
        {5,   5,  0,  0,  5,   0,    0,    5},  // Rel
        {0,   0,  0,  0,  0,   0,    0,    0},  // Open
        {0,   3,  4,  5,  0,   0,    0,    3},  // Close
        {3,   3,  0,  3,  3,   3,    3,    3},  // Punct
        {3,   3,  4,  5,  3,   0,    3,    3},  // Inner
    };
    return table[static_cast<unsigned>(left)][static_cast<unsigned>(right)];
}

bool binary_forbidden_after(AtomClass previous) {
    return previous == AtomClass::Operator || previous == AtomClass::Binary ||
           previous == AtomClass::Relation || previous == AtomClass::Opening ||
           previous == AtomClass::Punctuation;
}

bool binary_forbidden_before(AtomClass next) {
    return next == AtomClass::Relation || next == AtomClass::Closing ||
           next == AtomClass::Punctuation;
}

class Layouter {
public:
    Layouter(const MathTree& tree,
             TextSystem& text,
             Fx base_pixel_size,
             const MathFontConstants& constants)
        : tree_(tree), text_(text), base_pixel_size_(base_pixel_size), constants_(constants) {}

    bool layout(MathNodeId node, MathStyle style, Box& output) {
        return layout_node(node, style, MathVariant::Normal, 0, output);
    }

private:
    MathNodeId child(const MathNode& node, std::size_t index) const {
        const std::size_t edge = static_cast<std::size_t>(node.first_child) + index;
        return index < node.child_count && edge < tree_.children.size()
                   ? tree_.children[edge]
                   : kInvalidMathNode;
    }

    bool shape(std::string_view value,
               Fx pixel_size,
               MathVariant variant,
               AtomClass atom_class,
               Box& output) {
        output = {};
        output.atom_class = atom_class;
        std::string mapped_value;
        MathVariant mapped_variant =
            variant == MathVariant::Normal &&
                    atom_class == AtomClass::Ordinary &&
                    variable_symbol(value)
                ? MathVariant::Italic
                : variant;
        if (mapped_variant == MathVariant::Italic ||
            mapped_variant == MathVariant::Bold ||
            mapped_variant == MathVariant::BoldItalic ||
            mapped_variant == MathVariant::Blackboard ||
            mapped_variant == MathVariant::Calligraphic ||
            mapped_variant == MathVariant::SansSerif ||
            mapped_variant == MathVariant::Monospace ||
            mapped_variant == MathVariant::Fraktur) {
            const DecodedCodepoint decoded = utf8_next(
                reinterpret_cast<const std::uint8_t*>(value.data()),
                value.size(), 0);
            if (decoded.valid && decoded.byte_length == value.size()) {
                const std::uint32_t mapped =
                    styled_codepoint(decoded.value, mapped_variant);
                if (mapped != decoded.value) {
                    utf8_append(mapped, mapped_value);
                    value = mapped_value;
                    mapped_variant = MathVariant::Roman;
                }
            }
        }
        GlyphRun glyphs;
        if (!text_.shape(value.data(), value.size(), pixel_size, glyphs,
                         FontRole::Math, TextSpacing::Natural)) return false;
        output.metrics.width = glyphs.width;
        output.metrics.ascent = std::max<Fx>(0, glyphs.ascent);
        output.metrics.descent = positive_descent(glyphs.descent);
        output.runs.push_back({std::move(glyphs), 0, 0, pixel_size,
                               mapped_variant});
        return true;
    }

    bool shape_text(std::string_view value,
                    Fx pixel_size,
                    AtomClass atom_class,
                    Box& output) {
        output = {};
        output.atom_class = atom_class;
        GlyphRun glyphs;
        if (!text_.shape(value.data(), value.size(), pixel_size, glyphs,
                         FontRole::MathText, TextSpacing::Natural)) {
            return false;
        }
        output.metrics.width = glyphs.width;
        output.metrics.ascent = std::max<Fx>(0, glyphs.ascent);
        output.metrics.descent = positive_descent(glyphs.descent);
        output.runs.push_back({std::move(glyphs), 0, 0, pixel_size,
                               MathVariant::Roman});
        return true;
    }

    bool ink_bounds(const Box& box, Fx& top, Fx& bottom) const {
        bool found = false;
        top = 0;
        bottom = 0;
        for (const MathDrawRun& run : box.runs) {
            Fx run_top = 0;
            Fx run_bottom = 0;
            if (!text_.ink_bounds(run.glyphs, run.pixel_size,
                                  run_top, run_bottom)) {
                continue;
            }
            run_top += run.baseline_y;
            run_bottom += run.baseline_y;
            if (!found) {
                top = run_top;
                bottom = run_bottom;
                found = true;
            } else {
                top = std::min(top, run_top);
                bottom = std::max(bottom, run_bottom);
            }
        }
        for (const MathRule& rule : box.rules) {
            const Fx rule_top = rule.y;
            const Fx rule_bottom = rule.y + rule.height;
            if (!found) {
                top = rule_top;
                bottom = rule_bottom;
                found = true;
            } else {
                top = std::min(top, rule_top);
                bottom = std::max(bottom, rule_bottom);
            }
        }
        return found;
    }

    bool shape_stretchy(std::string_view value,
                        Fx minimum_pixel_size,
                        Fx target_height,
                        MathVariant variant,
                        AtomClass atom_class,
                        Box& output,
                        Fx& ink_top,
                        Fx& ink_bottom) {
        Fx pixel_size = std::max(minimum_pixel_size, target_height * 8 / 5);
        const Fx maximum_size = std::max(minimum_pixel_size, minimum_pixel_size * 8);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (!shape(value, pixel_size, variant, atom_class, output) ||
                !ink_bounds(output, ink_top, ink_bottom)) {
                return false;
            }
            const Fx actual_height = ink_bottom - ink_top;
            if (actual_height <= 0) return false;
            if (std::abs(actual_height - target_height) <= fx_from_int(1)) break;
            const Fx adjusted = static_cast<Fx>(std::min<std::int64_t>(
                maximum_size,
                std::max<std::int64_t>(
                    minimum_pixel_size,
                    (static_cast<std::int64_t>(pixel_size) * target_height +
                     actual_height - 1) /
                        actual_height)));
            if (adjusted == pixel_size) break;
            pixel_size = adjusted;
        }
        output.metrics.ascent = std::max<Fx>(0, -ink_top);
        output.metrics.descent = std::max<Fx>(0, ink_bottom);
        return true;
    }

    bool shape_system_delimiter(std::string_view value,
                                Fx pixel_size,
                                Fx target_height,
                                AtomClass atom_class,
                                Box& output,
                                Fx& ink_top,
                                Fx& ink_bottom) {
        const DecodedCodepoint decoded = utf8_next(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size(), 0);
        if (!decoded.valid || decoded.byte_length != value.size()) return false;
        const std::uint32_t codepoint = decoded.value;
        if (codepoint != '(' && codepoint != ')' && codepoint != '[' &&
            codepoint != ']' && codepoint != '{' && codepoint != '}' &&
            codepoint != '|' && codepoint != '<' && codepoint != '>' &&
            codepoint != '\\' &&
            codepoint != 0x2191U && codepoint != 0x2193U &&
            codepoint != 0x2195U && codepoint != 0x21D1U &&
            codepoint != 0x21D3U && codepoint != 0x21D5U &&
            codepoint != 0x2016U && codepoint != 0x2216U &&
            codepoint != 0x2308U && codepoint != 0x2309U &&
            codepoint != 0x230AU && codepoint != 0x230BU &&
            codepoint != 0x27E6U && codepoint != 0x27E7U &&
            codepoint != 0x27E8U && codepoint != 0x27E9U &&
            codepoint != 0x27EEU && codepoint != 0x27EFU) {
            return false;
        }
        GlyphRun glyphs;
        Fx render_pixel_size = pixel_size;
        if (!text_.shape_math_stretchy(codepoint, pixel_size, target_height,
                                       glyphs, render_pixel_size)) {
            return false;
        }
        output = {};
        output.atom_class = atom_class;
        output.metrics.width = glyphs.width;
        output.runs.push_back({std::move(glyphs), 0, 0,
                               render_pixel_size, MathVariant::Roman});
        if (!ink_bounds(output, ink_top, ink_bottom)) return false;
        output.metrics.ascent = std::max<Fx>(0, -ink_top);
        output.metrics.descent = std::max<Fx>(0, ink_bottom);
        return true;
    }

    bool shape_display_operator(std::string_view value,
                                Fx pixel_size,
                                MathVariant variant,
                                AtomClass atom_class,
                                Box& output) {
        const DecodedCodepoint decoded = utf8_next(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size(), 0);
        if (!decoded.valid || decoded.byte_length != value.size()) return false;

        GlyphRun glyphs;
        Fx render_pixel_size = pixel_size;
        // OpenType MATH supplies a distinct display glyph. Ask for the next
        // variant instead of synthetically scaling the text-size outline.
        if (!text_.shape_math_stretchy(decoded.value, pixel_size,
                                       pixel_size * 5 / 4,
                                       glyphs, render_pixel_size)) {
            return false;
        }
        output = {};
        output.atom_class = atom_class;
        output.metrics.width = glyphs.width;
        output.runs.push_back({std::move(glyphs), 0, 0,
                               render_pixel_size, variant});
        Fx ink_top = 0;
        Fx ink_bottom = 0;
        if (!ink_bounds(output, ink_top, ink_bottom)) return false;
        output.metrics.ascent = std::max<Fx>(0, -ink_top);
        output.metrics.descent = std::max<Fx>(0, ink_bottom);
        return true;
    }

    bool fixed_stroke_delimiter(std::string_view value,
                                Fx pixel_size,
                                Fx target_height,
                                AtomClass atom_class,
                                Box& output,
                                Fx& ink_top,
                                Fx& ink_bottom) const {
        const bool round = value == "(" || value == ")";
        const bool square = value == "[" || value == "]";
        const bool angle = value == "<" || value == ">" ||
                           value == u8"⟨" || value == u8"⟩";
        const bool brace = value == "{" || value == "}";
        const bool double_bar = value == u8"‖";
        if (target_height < pixel_size * 3 / 2 ||
            (!round && !square && !angle && !brace && !double_bar &&
             value != "|")) {
            return false;
        }

        const int height_px = std::max(fx_ceil(pixel_size),
                                       fx_ceil(target_height));
        const int font_px = fx_ceil(pixel_size);
        int width_px = 0;
        if (value == "|") width_px = 1;
        else if (double_bar) width_px = 3;
        else if (brace) {
            width_px = std::max(7, std::min(9, (font_px + 1) / 2));
        } else if (square) {
            width_px = std::max(3, std::min(5, (font_px + 3) / 4));
        } else {
            width_px = std::max(5, std::min(7, (font_px + 2) / 3));
        }
        const int top_px = -height_px / 2;
        const int bottom_px = top_px + height_px;
        auto add_rule = [&output](int x, int y, int width, int height,
                                  int coverage) {
            if (width > 0 && height > 0 && coverage > 0) {
                output.rules.push_back({fx_from_int(x), fx_from_int(y),
                                        fx_from_int(width), fx_from_int(height),
                                        static_cast<std::uint8_t>(
                                            std::min(255, coverage))});
            }
        };

        output = {};
        output.atom_class = atom_class;
        output.metrics = {fx_from_int(width_px),
                          fx_from_int(std::max(0, -top_px)),
                          fx_from_int(std::max(0, bottom_px))};
        ink_top = fx_from_int(top_px);
        ink_bottom = fx_from_int(bottom_px);

        if (value == "|") {
            add_rule(0, top_px, 1, height_px, 255);
            return true;
        }
        if (double_bar) {
            add_rule(0, top_px, 1, height_px, 255);
            add_rule(2, top_px, 1, height_px, 255);
            return true;
        }
        if (square) {
            const int vertical_x = value == "[" ? 0 : width_px - 1;
            add_rule(vertical_x, top_px, 1, height_px, 255);
            add_rule(0, top_px, width_px, 1, 255);
            add_rule(0, bottom_px - 1, width_px, 1, 255);
            return true;
        }

        const int depth = width_px - 1;
        const bool closing = value == ")" || value == ">" ||
                             value == u8"⟩" || value == "}";
        auto emit_profile = [&](const auto& opening_x_8_at) {
            constexpr int kSubpixel = 256;
            const int maximum_x_8 = depth * kSubpixel;
            for (int y = 0; y < height_px; ++y) {
                int x_8 = std::max(0, std::min(maximum_x_8,
                                               opening_x_8_at(y)));
                if (closing) x_8 = maximum_x_8 - x_8;
                const int x = x_8 / kSubpixel;
                const int secondary = x_8 % kSubpixel;
                add_rule(x, top_px + y, 1, 1, 255 - secondary);
                if (secondary != 0) {
                    add_rule(x + 1, top_px + y, 1, 1, secondary);
                }
            }
        };

        if (round) {
            // A quadratic horizontal profile leaves a long, straight middle
            // with gently stepped shoulders, approximating a thin parenthesis.
            const int denominator = std::max(1, height_px - 1);
            emit_profile([&](int y) {
                const std::int64_t distance = std::abs(2 * y - denominator);
                const std::int64_t divisor =
                    static_cast<std::int64_t>(denominator) * denominator;
                return static_cast<int>((depth * 256LL * distance * distance +
                                         divisor / 2) / divisor);
            });
            return true;
        }
        if (angle) {
            const int denominator = std::max(1, height_px - 1);
            emit_profile([&](int y) {
                const int distance = std::abs(2 * y - denominator);
                return static_cast<int>((depth * 256LL * distance +
                                         denominator / 2) / denominator);
            });
            return true;
        }

        // An opening brace has right-facing terminals, a centered stem, and a
        // left-facing cusp. The old profile put the cusp on the terminal side,
        // producing two stacked parentheses instead of a brace. Mirroring this
        // corrected centerline produces the closing brace.
        auto smooth_step_8 = [](int position, int span) {
            const int t = std::max(0, std::min(256,
                (position * 256 + span / 2) / std::max(1, span)));
            return static_cast<int>(
                static_cast<std::int64_t>(t) * t * (768 - 2 * t) /
                (256 * 256));
        };
        const int half_height = (height_px - 1) / 2;
        const int curve = std::max(3, std::min(8, height_px / 5));
        const int depth_8 = depth * 256;
        const int stem_x_8 = ((depth + 1) / 2) * 256;
        emit_profile([&](int y) {
            const int mirrored_y = std::min(y, height_px - 1 - y);
            const int edge_distance = mirrored_y;
            if (edge_distance < curve) {
                const int eased = smooth_step_8(edge_distance, curve);
                return depth_8 -
                       (depth_8 - stem_x_8) * eased / 256;
            }
            const int middle_distance = half_height - mirrored_y;
            if (middle_distance < curve) {
                const int eased = smooth_step_8(middle_distance, curve);
                return stem_x_8 * eased / 256;
            }
            return stem_x_8;
        });
        const int cap_width = std::min(2, width_px);
        auto add_brace_cap = [&](int opening_x, int y) {
            const int x = closing
                              ? depth - (opening_x + cap_width - 1)
                              : opening_x;
            add_rule(x, y, cap_width, 1, 255);
        };
        add_brace_cap(depth - cap_width + 1, top_px);
        add_brace_cap(depth - cap_width + 1, bottom_px - 1);
        add_brace_cap(0, top_px + (height_px - 1) / 2);
        return true;
    }

    bool layout_row_contents(const MathNode& node,
                             MathStyle style,
                             MathVariant variant,
                             unsigned depth,
                             bool extract_tag,
                             MathNodeId& extracted_tag,
                             Box& output) {
        struct RowItem {
            Box box;
            AtomClass effective_class = AtomClass::Ordinary;
            MathStyle style = MathStyle::Text;
            bool atom = true;
        };

        output = {};
        output.atom_class = node.atom_class;
        std::vector<RowItem> items;
        items.reserve(node.child_count);
        MathStyle active_style = style;
        for (std::size_t index = 0; index < node.child_count; ++index) {
            const MathNodeId child_id = child(node, index);
            // align/aligned owns one equation tag per row. Pull the first
            // direct tag out of its cell before spacing and width accounting;
            // a duplicate remains visible inline instead of being discarded.
            if (extract_tag && extracted_tag == kInvalidMathNode &&
                child_id < tree_.nodes.size() &&
                tree_.nodes[child_id].kind == MathNodeKind::Tag) {
                extracted_tag = child_id;
                continue;
            }
            if (child_id < tree_.nodes.size() &&
                tree_.nodes[child_id].kind == MathNodeKind::StyleChange) {
                active_style = static_cast<MathStyle>(
                    std::min<std::uint16_t>(tree_.nodes[child_id].aux, 3U));
                continue;
            }
            Box item;
            if (!layout_node(child_id, active_style, variant, depth + 1, item)) return false;
            items.push_back({std::move(item), item.atom_class, active_style,
                             tree_.nodes[child_id].kind != MathNodeKind::Space});
        }

        // TeX treats a binary atom as an ordinary atom when it is unary or
        // otherwise lacks operands on both sides. This removes the spurious
        // medium gaps in `-x`, `a=-b`, `(+x)`, and a trailing `a+`.
        bool have_previous = false;
        AtomClass previous = AtomClass::Ordinary;
        for (RowItem& item : items) {
            if (!item.atom) continue;
            if (item.effective_class == AtomClass::Binary &&
                (!have_previous || binary_forbidden_after(previous))) {
                item.effective_class = AtomClass::Ordinary;
            }
            previous = item.effective_class;
            have_previous = true;
        }
        bool have_next = false;
        AtomClass next = AtomClass::Ordinary;
        for (auto item = items.rbegin(); item != items.rend(); ++item) {
            if (!item->atom) continue;
            if (item->effective_class == AtomClass::Binary &&
                (!have_next || binary_forbidden_before(next))) {
                item->effective_class = AtomClass::Ordinary;
            }
            next = item->effective_class;
            have_next = true;
        }

        have_previous = false;
        previous = AtomClass::Ordinary;
        for (RowItem& row_item : items) {
            Box& item = row_item.box;
            Fx gap = 0;
            if (row_item.atom && have_previous &&
                (row_item.style == MathStyle::Display ||
                 row_item.style == MathStyle::Text)) {
                const Fx pixel = style_size(base_pixel_size_, row_item.style);
                gap = static_cast<Fx>(static_cast<std::int64_t>(pixel) *
                                      spacing_mu(previous,
                                                 row_item.effective_class) / 18);
            }
            const Fx x = std::max<Fx>(0, output.metrics.width + gap);
            const Fx item_width = item.metrics.width;
            const Fx item_ascent = item.metrics.ascent;
            const Fx item_descent = item.metrics.descent;
            append(output, std::move(item), x, 0);
            output.metrics.width = std::max<Fx>(0, x + item_width);
            output.metrics.ascent = std::max(output.metrics.ascent, item_ascent);
            output.metrics.descent = std::max(output.metrics.descent, item_descent);
            if (row_item.atom) {
                previous = row_item.effective_class;
                have_previous = true;
            }
        }
        return true;
    }

    bool layout_row(const MathNode& node,
                    MathStyle style,
                    MathVariant variant,
                    unsigned depth,
                    Box& output) {
        MathNodeId ignored_tag = kInvalidMathNode;
        return layout_row_contents(node, style, variant, depth, false,
                                   ignored_tag, output);
    }

    bool layout_fraction(const MathNode& node,
                         MathStyle style,
                         MathVariant variant,
                         unsigned depth,
                         Box& output) {
        Box numerator;
        Box denominator;
        if (!layout_node(child(node, 0), fraction_child_style(style), variant,
                         depth + 1, numerator) ||
            !layout_node(child(node, 1), fraction_child_style(style), variant,
                         depth + 1, denominator)) {
            return false;
        }
        const Fx pixel = style_size(base_pixel_size_, style);
        const Fx padding = std::max<Fx>(fx_from_int(1), pixel / 8);
        const Fx rule = std::max<Fx>(fx_from_int(1), em_value(pixel, constants_.fraction_rule));
        const Fx num_gap = std::max<Fx>(fx_from_int(2),
                                        em_value(pixel, constants_.fraction_num_gap));
        const Fx den_gap = std::max<Fx>(fx_from_int(2),
                                        em_value(pixel, constants_.fraction_den_gap));
        const Fx axis = em_value(pixel, constants_.axis_height);
        output = {};
        output.atom_class = AtomClass::Inner;
        output.metrics.width = std::max(numerator.metrics.width, denominator.metrics.width) +
                               padding * 2;
        const Fx rule_y = -axis - rule / 2;
        const Fx numerator_baseline = rule_y - num_gap - numerator.metrics.descent;
        const Fx denominator_baseline = rule_y + rule + den_gap + denominator.metrics.ascent;
        append(output, std::move(numerator),
               (output.metrics.width - numerator.metrics.width) / 2,
               numerator_baseline);
        append(output, std::move(denominator),
               (output.metrics.width - denominator.metrics.width) / 2,
               denominator_baseline);
        output.rules.push_back({0, rule_y, output.metrics.width, rule});
        output.metrics.ascent = std::max<Fx>(0, -numerator_baseline + numerator.metrics.ascent);
        output.metrics.descent = std::max<Fx>(0, denominator_baseline + denominator.metrics.descent);
        return true;
    }

    bool layout_scripts(const MathNode& node,
                        MathStyle style,
                        MathVariant variant,
                        unsigned depth,
                        Box& output) {
        const MathNodeId base_id = child(node, 0);
        Box base;
        if (!layout_node(base_id, style, variant, depth + 1, base)) return false;
        Box sub;
        Box sup;
        std::size_t next = 1;
        if ((node.flags & MathNodeFlagHasSubscript) != 0 &&
            !layout_node(child(node, next++), script_child_style(style), variant,
                         depth + 1, sub)) {
            return false;
        }
        if ((node.flags & MathNodeFlagHasSuperscript) != 0 &&
            !layout_node(child(node, next), script_child_style(style), variant,
                         depth + 1, sup)) {
            return false;
        }
        const Fx pixel = style_size(base_pixel_size_, style);
        const bool limits = style == MathStyle::Display && base_id < tree_.nodes.size() &&
                            (tree_.nodes[base_id].flags &
                             MathNodeFlagMovableLimits) != 0;
        output = {};
        output.atom_class = base.atom_class;
        append(output, std::move(base), 0, 0);
        output.metrics = base.metrics;
        if (limits) {
            const Fx gap = std::max<Fx>(fx_from_int(2), pixel / 8);
            Fx width = base.metrics.width;
            if (!sup.runs.empty() || !sup.rules.empty()) width = std::max(width, sup.metrics.width);
            if (!sub.runs.empty() || !sub.rules.empty()) width = std::max(width, sub.metrics.width);
            for (MathDrawRun& run : output.runs) run.x += (width - base.metrics.width) / 2;
            for (MathRule& rule : output.rules) rule.x += (width - base.metrics.width) / 2;
            if (!sup.runs.empty() || !sup.rules.empty()) {
                const Fx baseline = -base.metrics.ascent - gap - sup.metrics.descent;
                append(output, std::move(sup), (width - sup.metrics.width) / 2, baseline);
                output.metrics.ascent = std::max(output.metrics.ascent,
                                                 -baseline + sup.metrics.ascent);
            }
            if (!sub.runs.empty() || !sub.rules.empty()) {
                const Fx baseline = base.metrics.descent + gap + sub.metrics.ascent;
                append(output, std::move(sub), (width - sub.metrics.width) / 2, baseline);
                output.metrics.descent = std::max(output.metrics.descent,
                                                  baseline + sub.metrics.descent);
            }
            output.metrics.width = width;
            return true;
        }

        const Fx script_x = base.metrics.width + std::max<Fx>(fx_from_int(1), pixel / 16);
        Fx sup_baseline = -std::max(em_value(pixel, constants_.superscript_shift),
                                   base.metrics.ascent * 2 / 3);
        Fx sub_baseline = std::max(em_value(pixel, constants_.subscript_shift),
                                   base.metrics.descent + pixel / 5);
        if ((!sup.runs.empty() || !sup.rules.empty()) &&
            (!sub.runs.empty() || !sub.rules.empty())) {
            const Fx gap = (sub_baseline - sub.metrics.ascent) -
                           (sup_baseline + sup.metrics.descent);
            const Fx minimum = std::max<Fx>(fx_from_int(2), pixel / 5);
            if (gap < minimum) {
                const Fx adjustment = (minimum - gap + 1) / 2;
                sup_baseline -= adjustment;
                sub_baseline += adjustment;
            }
        }
        if (!sup.runs.empty() || !sup.rules.empty()) {
            append(output, std::move(sup), script_x, sup_baseline);
            output.metrics.ascent = std::max(output.metrics.ascent,
                                             -sup_baseline + sup.metrics.ascent);
            output.metrics.width = std::max(output.metrics.width,
                                             script_x + sup.metrics.width);
        }
        if (!sub.runs.empty() || !sub.rules.empty()) {
            append(output, std::move(sub), script_x, sub_baseline);
            output.metrics.descent = std::max(output.metrics.descent,
                                              sub_baseline + sub.metrics.descent);
            output.metrics.width = std::max(output.metrics.width,
                                             script_x + sub.metrics.width);
        }
        return true;
    }

    bool layout_radical(const MathNode& node,
                        MathStyle style,
                        MathVariant variant,
                        unsigned depth,
                        Box& output) {
        Box radicand;
        if (!layout_node(child(node, 0), style, variant, depth + 1, radicand)) return false;
        const Fx pixel = style_size(base_pixel_size_, style);
        const Fx rule = std::max<Fx>(fx_from_int(1),
                                     em_value(pixel, constants_.radical_rule));
        const Fx gap = std::max<Fx>(fx_from_int(1), em_value(pixel, constants_.radical_gap));
        const Fx target_height = radicand.metrics.ascent +
                                 radicand.metrics.descent + gap + rule;
        GlyphRun radical_glyphs;
        Fx radical_pixel_size = pixel;
        if (!text_.shape_math_stretchy(0x221AU, pixel, target_height,
                                       radical_glyphs, radical_pixel_size)) {
            return false;
        }
        Box radical;
        radical.atom_class = AtomClass::Opening;
        radical.metrics.width = radical_glyphs.width;
        radical.runs.push_back({std::move(radical_glyphs), 0, 0,
                                radical_pixel_size, MathVariant::Roman});
        Fx radical_top = 0;
        Fx radical_bottom = 0;
        if (!ink_bounds(radical, radical_top, radical_bottom)) return false;

        const Fx rule_y = -radicand.metrics.ascent - gap - rule;
        const Fx radical_baseline = rule_y - radical_top;
        const Fx join_overlap = std::max<Fx>(fx_from_int(1), pixel / 16);
        const Fx radicand_x = std::max<Fx>(0, radical.metrics.width - join_overlap);
        const Fx overbar_extension = std::max<Fx>(fx_from_int(1), pixel / 8);
        output = {};
        output.atom_class = AtomClass::Ordinary;
        append(output, std::move(radical), 0, radical_baseline);
        append(output, std::move(radicand), radicand_x, 0);
        output.rules.push_back({radicand_x, rule_y,
                                radicand.metrics.width + overbar_extension,
                                rule, 255});
        output.metrics.width = radicand_x + radicand.metrics.width +
                               overbar_extension;
        output.metrics.ascent = std::max(-rule_y,
                                         radicand.metrics.ascent + gap + rule);
        output.metrics.descent = std::max(radicand.metrics.descent,
                                          radical_baseline + radical_bottom);
        if (node.child_count > 1) {
            Box index;
            if (!layout_node(child(node, 1), MathStyle::ScriptScript, variant,
                             depth + 1, index)) return false;
            const Fx baseline = rule_y +
                std::max<Fx>(fx_from_int(2), target_height / 4);
            append(output, std::move(index), 0, baseline);
            output.metrics.ascent = std::max(output.metrics.ascent,
                                             -baseline + index.metrics.ascent);
        }
        return true;
    }

    bool layout_delimited(const MathNode& node,
                          MathStyle style,
                          MathVariant variant,
                          unsigned depth,
                          Box& output) {
        Box inner;
        if (!layout_node(child(node, 0), style, variant, depth + 1, inner)) return false;
        const std::string_view stored = tree_.text(node);
        const std::size_t opening_size = std::min<std::size_t>(node.aux, stored.size());
        const std::string_view opening = stored.substr(0, opening_size);
        const std::string_view closing = opening_size < stored.size()
                                             ? stored.substr(opening_size + 1)
                                             : std::string_view{};
        const Fx pixel = style_size(base_pixel_size_, style);
        const Fx axis = em_value(pixel, constants_.axis_height);
        const Fx center = -axis;
        Fx inner_top = -inner.metrics.ascent;
        Fx inner_bottom = inner.metrics.descent;
        ink_bounds(inner, inner_top, inner_bottom);
        const Fx radius = std::max(center - inner_top,
                                   inner_bottom - center);
        const Fx target = std::max(pixel, radius * 2);
        Box left;
        Box right;
        Fx left_top = 0;
        Fx left_bottom = 0;
        Fx right_top = 0;
        Fx right_bottom = 0;
        if (!opening.empty() &&
            !shape_system_delimiter(opening, pixel, target,
                                AtomClass::Opening,
                                left, left_top, left_bottom) &&
            !fixed_stroke_delimiter(opening, pixel, target,
                                    AtomClass::Opening,
                                    left, left_top, left_bottom) &&
            !shape_stretchy(opening, pixel, target, variant,
                            AtomClass::Opening, left, left_top, left_bottom)) {
            return false;
        }
        if (!closing.empty() &&
            !shape_system_delimiter(closing, pixel, target,
                                AtomClass::Closing,
                                right, right_top, right_bottom) &&
            !fixed_stroke_delimiter(closing, pixel, target,
                                    AtomClass::Closing,
                                    right, right_top, right_bottom) &&
            !shape_stretchy(closing, pixel, target, variant,
                            AtomClass::Closing, right, right_top, right_bottom)) {
            return false;
        }
        const Fx gap = std::max<Fx>(fx_from_int(1), pixel / 12);
        output = {};
        output.atom_class = AtomClass::Inner;
        Fx x = 0;
        if (!opening.empty()) {
            const Fx baseline = center - (left_top + left_bottom) / 2;
            const Fx width = left.metrics.width;
            append(output, std::move(left), x, baseline);
            output.metrics.ascent = std::max<Fx>(
                output.metrics.ascent, -(left_top + baseline));
            output.metrics.descent = std::max<Fx>(
                output.metrics.descent, left_bottom + baseline);
            x += width + gap;
        }
        append(output, std::move(inner), x, 0);
        output.metrics.ascent = std::max(output.metrics.ascent, inner.metrics.ascent);
        output.metrics.descent = std::max(output.metrics.descent, inner.metrics.descent);
        x += inner.metrics.width;
        if (!closing.empty()) {
            x += gap;
            const Fx baseline = center - (right_top + right_bottom) / 2;
            const Fx width = right.metrics.width;
            append(output, std::move(right), x, baseline);
            output.metrics.ascent = std::max<Fx>(
                output.metrics.ascent, -(right_top + baseline));
            output.metrics.descent = std::max<Fx>(
                output.metrics.descent, right_bottom + baseline);
            x += width;
        }
        output.metrics.width = x;
        return true;
    }

    bool layout_accent(const MathNode& node,
                       MathStyle style,
                       MathVariant variant,
                       unsigned depth,
                       Box& output) {
        const MathAccent accent = static_cast<MathAccent>(node.aux);
        std::uint32_t combining_mark = 0;
        switch (accent) {
        case MathAccent::Hat: combining_mark = 0x0302U; break;
        case MathAccent::Acute: combining_mark = 0x0301U; break;
        case MathAccent::Grave: combining_mark = 0x0300U; break;
        case MathAccent::Breve: combining_mark = 0x0306U; break;
        case MathAccent::Check: combining_mark = 0x030CU; break;
        case MathAccent::Tilde: combining_mark = 0x0303U; break;
        case MathAccent::Ring: combining_mark = 0x030AU; break;
        case MathAccent::Vector: combining_mark = 0x20D7U; break;
        case MathAccent::Dot: combining_mark = 0x0307U; break;
        case MathAccent::DoubleDot: combining_mark = 0x0308U; break;
        default: break;
        }

        const MathNodeId base_id = child(node, 0);
        MathNodeId simple_base_id = base_id;
        if (base_id < tree_.nodes.size() &&
            tree_.nodes[base_id].kind == MathNodeKind::Row &&
            tree_.nodes[base_id].child_count == 1) {
            const std::size_t edge = tree_.nodes[base_id].first_child;
            if (edge < tree_.children.size()) simple_base_id = tree_.children[edge];
        }
        if (combining_mark != 0 && simple_base_id < tree_.nodes.size() &&
            tree_.nodes[simple_base_id].kind == MathNodeKind::Symbol) {
            const MathNode& base_node = tree_.nodes[simple_base_id];
            const std::string_view base_text = tree_.text(base_node);
            const DecodedCodepoint decoded = utf8_next(
                reinterpret_cast<const std::uint8_t*>(base_text.data()),
                base_text.size(), 0);
            if (decoded.valid && decoded.byte_length == base_text.size()) {
                MathVariant mapped_variant =
                    variant == MathVariant::Normal &&
                            base_node.atom_class == AtomClass::Ordinary &&
                            variable_symbol(base_text)
                        ? MathVariant::Italic
                        : variant;
                const std::uint32_t mapped =
                    styled_codepoint(decoded.value, mapped_variant);
                std::string composed;
                utf8_append(mapped, composed);
                utf8_append(combining_mark, composed);
                if (shape(composed, style_size(base_pixel_size_, style),
                          MathVariant::Roman, AtomClass::Ordinary, output)) {
                    Fx ink_top = 0;
                    Fx ink_bottom = 0;
                    if (ink_bounds(output, ink_top, ink_bottom)) {
                        output.metrics.ascent = std::max(output.metrics.ascent,
                                                         -ink_top);
                        output.metrics.descent = std::max(output.metrics.descent,
                                                          ink_bottom);
                    }
                    return true;
                }
            }
        }

        Box base;
        if (!layout_node(base_id, style, variant, depth + 1, base)) return false;
        const Fx pixel = style_size(base_pixel_size_, style);
        Fx base_top = -base.metrics.ascent;
        Fx base_bottom = base.metrics.descent;
        ink_bounds(base, base_top, base_bottom);
        output = {};
        output.atom_class = AtomClass::Ordinary;
        append(output, std::move(base), 0, 0);
        output.metrics = base.metrics;
        const Fx rule = std::max<Fx>(fx_from_int(1), pixel / 18);
        const Fx gap = fx_from_int(1);
        if (accent == MathAccent::Overline || accent == MathAccent::Bar) {
            const Fx rule_y = base_top - gap - rule;
            output.rules.push_back({0, rule_y,
                                    base.metrics.width, rule});
            output.metrics.ascent = std::max(output.metrics.ascent, -rule_y);
        } else if (accent == MathAccent::Underline) {
            const Fx rule_y = base_bottom + gap;
            output.rules.push_back({0, rule_y,
                                    base.metrics.width, rule});
            output.metrics.descent = std::max(output.metrics.descent,
                                              rule_y + rule);
        } else if (accent == MathAccent::OverLeftArrow ||
                   accent == MathAccent::OverRightArrow ||
                   accent == MathAccent::OverLeftRightArrow ||
                   accent == MathAccent::UnderLeftArrow ||
                   accent == MathAccent::UnderRightArrow ||
                   accent == MathAccent::UnderLeftRightArrow) {
            const bool under = accent == MathAccent::UnderLeftArrow ||
                               accent == MathAccent::UnderRightArrow ||
                               accent == MathAccent::UnderLeftRightArrow;
            const bool left = accent == MathAccent::OverLeftArrow ||
                              accent == MathAccent::OverLeftRightArrow ||
                              accent == MathAccent::UnderLeftArrow ||
                              accent == MathAccent::UnderLeftRightArrow;
            const bool right = accent == MathAccent::OverRightArrow ||
                               accent == MathAccent::OverLeftRightArrow ||
                               accent == MathAccent::UnderRightArrow ||
                               accent == MathAccent::UnderLeftRightArrow;
            const Fx line_y = under ? base_bottom + gap + fx_from_int(2)
                                    : base_top - gap - rule - fx_from_int(2);
            const Fx width = std::max<Fx>(base.metrics.width, fx_from_int(5));
            output.rules.push_back({0, line_y, width, rule});
            const Fx one = fx_from_int(1);
            const Fx two = fx_from_int(2);
            if (left) {
                output.rules.push_back({0, line_y, one, one});
                output.rules.push_back({one, line_y - one, one, one});
                output.rules.push_back({one, line_y + one, one, one});
                output.rules.push_back({two, line_y - two, one, one, 160});
                output.rules.push_back({two, line_y + two, one, one, 160});
            }
            if (right) {
                output.rules.push_back({width - one, line_y, one, one});
                output.rules.push_back({width - two, line_y - one, one, one});
                output.rules.push_back({width - two, line_y + one, one, one});
                output.rules.push_back({width - fx_from_int(3), line_y - two,
                                        one, one, 160});
                output.rules.push_back({width - fx_from_int(3), line_y + two,
                                        one, one, 160});
            }
            if (under) {
                output.metrics.descent = std::max(output.metrics.descent,
                                                  line_y + fx_from_int(3));
            } else {
                output.metrics.ascent = std::max(output.metrics.ascent,
                                                 -line_y + fx_from_int(2));
            }
        } else if (accent == MathAccent::OverBrace ||
                   accent == MathAccent::UnderBrace) {
            const bool under = accent == MathAccent::UnderBrace;
            const Fx one = fx_from_int(1);
            const Fx two = fx_from_int(2);
            const Fx width = std::max<Fx>(base.metrics.width, fx_from_int(7));
            const Fx middle = width / 2;
            const Fx line_y = under ? base_bottom + gap + two
                                    : base_top - gap - two;
            output.rules.push_back({one, line_y,
                                    std::max<Fx>(one, width - two), rule});
            const Fx outer_y = under ? line_y - one : line_y + one;
            const Fx inner_y = under ? line_y + one : line_y - one;
            const Fx point_y = under ? line_y + two : line_y - two;
            output.rules.push_back({0, outer_y, one, one, 192});
            output.rules.push_back({width - one, outer_y, one, one, 192});
            output.rules.push_back({middle - one, inner_y, one, one, 192});
            output.rules.push_back({middle + one, inner_y, one, one, 192});
            output.rules.push_back({middle, point_y, one, one});
            if (under) {
                output.metrics.descent = std::max(output.metrics.descent,
                                                  point_y + one);
            } else {
                output.metrics.ascent = std::max(output.metrics.ascent,
                                                 -point_y + one);
            }
        } else {
            const char* mark = accent == MathAccent::Hat ? "^" :
                               accent == MathAccent::Acute ? u8"´" :
                               accent == MathAccent::Grave ? "`" :
                               accent == MathAccent::Breve ? u8"˘" :
                               accent == MathAccent::Check ? u8"ˇ" :
                               accent == MathAccent::Tilde ? u8"˜" :
                               accent == MathAccent::Ring ? u8"˚" :
                               accent == MathAccent::Vector ? u8"→" :
                               accent == MathAccent::Dot ? u8"˙" :
                               u8"¨";
            Box mark_box;
            if (!shape(mark, std::max<Fx>(fx_from_int(7), pixel * 3 / 4), variant,
                       AtomClass::Ordinary, mark_box)) return false;
            Fx mark_top = -mark_box.metrics.ascent;
            Fx mark_bottom = mark_box.metrics.descent;
            ink_bounds(mark_box, mark_top, mark_bottom);
            const Fx baseline = base_top - gap - mark_bottom;
            append(output, std::move(mark_box),
                   (base.metrics.width - mark_box.metrics.width) / 2, baseline);
            output.metrics.ascent = std::max(output.metrics.ascent,
                                             -(baseline + mark_top));
        }
        return true;
    }

    bool layout_array(const MathNode& node,
                      MathStyle style,
                      MathVariant variant,
                      unsigned depth,
                      Box& output) {
        const std::size_t rows = node.aux;
        const std::size_t columns = static_cast<std::size_t>(
            std::max<std::int32_t>(0, node.value));
        if (rows == 0 || columns == 0 || rows * columns > node.child_count) return false;
        const std::string_view descriptor = tree_.text(node);
        const std::size_t descriptor_separator = descriptor.find('\0');
        const std::string_view environment = descriptor.substr(0, descriptor_separator);
        const std::string_view preamble = descriptor_separator == std::string_view::npos
                                              ? std::string_view{}
                                              : descriptor.substr(descriptor_separator + 1);
        const bool aligned_environment = environment == "aligned" ||
                                         environment == "align" ||
                                         environment == "align*";
        std::vector<Box> cells(rows * columns);
        std::vector<MathNodeId> row_tags(rows, kInvalidMathNode);
        std::vector<Box> tag_boxes(rows);
        std::vector<Fx> widths(columns, 0);
        std::vector<Fx> ascents(rows, 0);
        std::vector<Fx> descents(rows, 0);
        for (std::size_t row = 0; row < rows; ++row) {
            for (std::size_t column = 0; column < columns; ++column) {
                Box& cell = cells[row * columns + column];
                const MathNodeId cell_id = child(node, row * columns + column);
                if (aligned_environment && cell_id < tree_.nodes.size() &&
                    tree_.nodes[cell_id].kind == MathNodeKind::Row) {
                    if (!layout_row_contents(tree_.nodes[cell_id], style, variant,
                                             depth + 1, true, row_tags[row],
                                             cell)) {
                        return false;
                    }
                } else if (!layout_node(cell_id, style, variant,
                                        depth + 1, cell)) {
                    return false;
                }
                widths[column] = std::max(widths[column], cell.metrics.width);
                ascents[row] = std::max(ascents[row], cell.metrics.ascent);
                descents[row] = std::max(descents[row], cell.metrics.descent);
            }
            if (row_tags[row] != kInvalidMathNode) {
                if (!layout_node(row_tags[row], style, variant, depth + 1,
                                 tag_boxes[row])) {
                    return false;
                }
                ascents[row] = std::max(ascents[row], tag_boxes[row].metrics.ascent);
                descents[row] = std::max(descents[row], tag_boxes[row].metrics.descent);
            }
        }
        const Fx pixel = style_size(base_pixel_size_, style);
        const Fx column_gap = pixel / 2;
        const Fx row_gap = pixel / 4;
        Fx equation_width = 0;
        for (Fx width : widths) equation_width += width;
        if (columns > 1) equation_width += column_gap * static_cast<Fx>(columns - 1);
        Fx tag_lane_width = 0;
        for (const Box& tag : tag_boxes) {
            tag_lane_width = std::max(tag_lane_width, tag.metrics.width);
        }
        const Fx tag_gap = tag_lane_width == 0
                               ? 0
                               : std::max<Fx>(fx_from_int(2), pixel / 2);
        const Fx tag_start = equation_width + tag_gap;
        const Fx content_width = tag_start + tag_lane_width;
        std::vector<Fx> column_starts(columns, 0);
        for (std::size_t column = 1; column < columns; ++column) {
            column_starts[column] = column_starts[column - 1] +
                                    widths[column - 1] + column_gap;
        }
        std::vector<char> column_alignment(columns, 'c');
        std::vector<bool> vertical_rules(columns + 1, false);
        std::size_t specified_column = 0;
        for (char value : preamble) {
            if (value == '|') {
                vertical_rules[std::min(specified_column, columns)] = true;
            } else if ((value == 'l' || value == 'c' || value == 'r') &&
                       specified_column < columns) {
                column_alignment[specified_column++] = value;
            }
        }
        Fx total_height = 0;
        for (std::size_t row = 0; row < rows; ++row) {
            total_height += ascents[row] + descents[row];
        }
        if (rows > 1) total_height += row_gap * static_cast<Fx>(rows - 1);
        const Fx axis = em_value(pixel, constants_.axis_height);
        const Fx center = -axis;
        const Fx top = center - total_height / 2;
        Box content;
        content.atom_class = AtomClass::Inner;
        Fx row_top = top;
        std::vector<Fx> row_tops(rows, top);
        for (std::size_t row = 0; row < rows; ++row) {
            row_tops[row] = row_top;
            const Fx baseline = row_top + ascents[row];
            for (std::size_t column = 0; column < columns; ++column) {
                Box& cell = cells[row * columns + column];
                const Fx x = column_starts[column];
                Fx cell_x = x + (widths[column] - cell.metrics.width) / 2;
                if (aligned_environment) {
                    cell_x = column % 2 == 0
                                 ? x + widths[column] - cell.metrics.width
                                 : x;
                } else if (environment == "cases") {
                    cell_x = x;
                } else if (environment == "array") {
                    cell_x = column_alignment[column] == 'l'
                                 ? x
                                 : column_alignment[column] == 'r'
                                       ? x + widths[column] - cell.metrics.width
                                       : cell_x;
                }
                append(content, std::move(cell), cell_x, baseline);
            }
            if (row_tags[row] != kInvalidMathNode) {
                Box& tag = tag_boxes[row];
                const Fx tag_width = tag.metrics.width;
                append(content, std::move(tag),
                       tag_start + tag_lane_width - tag_width, baseline);
            }
            row_top += ascents[row] + descents[row] + row_gap;
        }
        const Fx array_rule = fx_from_int(1);
        for (std::size_t row = 0; row < rows; ++row) {
            if ((node.metadata & (1U << static_cast<unsigned>(row))) == 0) continue;
            const Fx y = row == 0 ? top : row_tops[row] - row_gap / 2;
            content.rules.push_back({0, y, content_width, array_rule});
        }
        for (std::size_t boundary = 0; boundary <= columns; ++boundary) {
            if (!vertical_rules[boundary]) continue;
            const Fx x = boundary == 0
                             ? 0
                             : boundary == columns
                                   ? std::max<Fx>(0, content_width - array_rule)
                                   : column_starts[boundary] - column_gap / 2;
            content.rules.push_back({x, top, array_rule, total_height});
        }
        content.metrics = {content_width, std::max<Fx>(0, -top),
                           std::max<Fx>(0, total_height + top)};

        std::string opening;
        std::string closing;
        if (environment == "pmatrix") { opening = "("; closing = ")"; }
        if (environment == "bmatrix") { opening = "["; closing = "]"; }
        if (environment == "Bmatrix") { opening = "{"; closing = "}"; }
        if (environment == "vmatrix") { opening = "|"; closing = "|"; }
        if (environment == "Vmatrix") { opening = u8"‖"; closing = u8"‖"; }
        if (environment == "cases") { opening = "{"; }
        if (opening.empty() && closing.empty()) {
            output = std::move(content);
            return true;
        }

        Box left;
        Box right;
        Fx left_top = 0;
        Fx left_bottom = 0;
        Fx right_top = 0;
        Fx right_bottom = 0;
        if (!opening.empty() &&
            !shape_system_delimiter(opening, pixel, total_height,
                                AtomClass::Opening,
                                left, left_top, left_bottom) &&
            !fixed_stroke_delimiter(opening, pixel, total_height,
                                    AtomClass::Opening,
                                    left, left_top, left_bottom) &&
            !shape_stretchy(opening, pixel, total_height, variant,
                            AtomClass::Opening, left, left_top, left_bottom)) {
            return false;
        }
        if (!closing.empty() &&
            !shape_system_delimiter(closing, pixel, total_height,
                                AtomClass::Closing,
                                right, right_top, right_bottom) &&
            !fixed_stroke_delimiter(closing, pixel, total_height,
                                    AtomClass::Closing,
                                    right, right_top, right_bottom) &&
            !shape_stretchy(closing, pixel, total_height, variant,
                            AtomClass::Closing, right, right_top, right_bottom)) {
            return false;
        }
        const Fx gap = pixel / 8;
        output = {};
        output.atom_class = AtomClass::Inner;
        Fx x = 0;
        if (!opening.empty()) {
            const Fx baseline = center - (left_top + left_bottom) / 2;
            const Fx width = left.metrics.width;
            append(output, std::move(left), 0, baseline);
            output.metrics.ascent = std::max<Fx>(0, -(left_top + baseline));
            output.metrics.descent = std::max<Fx>(0, left_bottom + baseline);
            x = width + gap;
        }
        append(output, std::move(content), x, 0);
        output.metrics.ascent = std::max(output.metrics.ascent,
                                         content.metrics.ascent);
        output.metrics.descent = std::max(output.metrics.descent,
                                          content.metrics.descent);
        x += content.metrics.width;
        if (!closing.empty()) {
            x += gap;
            const Fx baseline = center - (right_top + right_bottom) / 2;
            const Fx width = right.metrics.width;
            append(output, std::move(right), x, baseline);
            output.metrics.ascent = std::max<Fx>(
                output.metrics.ascent, -(right_top + baseline));
            output.metrics.descent = std::max<Fx>(
                output.metrics.descent, right_bottom + baseline);
            x += width;
        }
        output.metrics.width = x;
        return true;
    }

    bool layout_node(MathNodeId id,
                     MathStyle style,
                     MathVariant variant,
                     unsigned depth,
                     Box& output) {
        if (id == kInvalidMathNode || id >= tree_.nodes.size() ||
            depth > kMaximumMathNesting) return false;
        const MathNode& node = tree_.nodes[id];
        const Fx pixel = style_size(base_pixel_size_, style);
        switch (node.kind) {
        case MathNodeKind::Row:
            return layout_row(node, style, variant, depth, output);
        case MathNodeKind::Symbol: {
            if ((node.flags & MathNodeFlagLargeOperator) != 0 &&
                style == MathStyle::Display) {
                if (shape_display_operator(tree_.text(node), pixel, variant,
                                           node.atom_class, output)) {
                    return true;
                }
                return shape(tree_.text(node), pixel * 5 / 4, variant,
                             node.atom_class, output);
            }
            return shape(tree_.text(node), pixel, variant,
                         node.atom_class, output);
        }
        case MathNodeKind::Text:
            return node.atom_class == AtomClass::Operator
                       ? shape(tree_.text(node), pixel, MathVariant::Roman,
                               node.atom_class, output)
                       : shape_text(tree_.text(node), pixel, node.atom_class, output);
        case MathNodeKind::Tag:
            return shape_text(tree_.text(node), pixel,
                              AtomClass::Ordinary, output);
        case MathNodeKind::Space:
            output = {};
            output.metrics.width = static_cast<Fx>(
                static_cast<std::int64_t>(pixel) * node.value / 18);
            return true;
        case MathNodeKind::Fraction:
            return layout_fraction(node, style, variant, depth, output);
        case MathNodeKind::Radical:
            return layout_radical(node, style, variant, depth, output);
        case MathNodeKind::Scripts:
            return layout_scripts(node, style, variant, depth, output);
        case MathNodeKind::Delimited:
            return layout_delimited(node, style, variant, depth, output);
        case MathNodeKind::Accent:
            return layout_accent(node, style, variant, depth, output);
        case MathNodeKind::Styled:
            if (!layout_node(child(node, 0), style,
                             static_cast<MathVariant>(node.aux), depth + 1,
                             output)) {
                return false;
            }
            output.atom_class = node.atom_class;
            return true;
        case MathNodeKind::StyleChange:
            output = {};
            return true;
        case MathNodeKind::Array:
            return layout_array(node, style, variant, depth, output);
        case MathNodeKind::Error:
            return shape("!", pixel, MathVariant::Bold,
                         AtomClass::Ordinary, output);
        }
        return false;
    }

    const MathTree& tree_;
    TextSystem& text_;
    Fx base_pixel_size_;
    const MathFontConstants& constants_;
};

}  // namespace

bool layout_math_tree(const MathTree& tree,
                      TextSystem& text,
                      MathStyle style,
                      Fx base_pixel_size,
                      Fx maximum_width,
                      const MathFontConstants& constants,
                      MathLayoutResult& result) {
    result = {};
    result.valid = !tree.recovered_error;
    result.diagnostic = tree.diagnostic;
    if (base_pixel_size <= 0 || tree.root == kInvalidMathNode) return false;
    Layouter layouter(tree, text, base_pixel_size, constants);
    Box box;
    if (!layouter.layout(tree.root, style, box)) return false;
    result.metrics = box.metrics;
    result.runs = std::move(box.runs);
    result.rules = std::move(box.rules);
    result.overflow = maximum_width > 0 && result.metrics.width > maximum_width;
    return true;
}

}  // namespace nmarkdown
