#include "nmarkdown/math/math_system.h"

#include <algorithm>
#include <utility>

#include "nmarkdown/generated/core_math_font.h"
#include "nmarkdown/math/math_parser.h"
#include "nmarkdown/render/primitives.h"
#include "nmarkdown/text/compositor.h"
#include "nmarkdown/text/text_system.h"

namespace nmarkdown {
namespace {

constexpr std::size_t kMaximumFormulaCacheEntries = 64;

}  // namespace

MathSystem::MathSystem(TextSystem& text)
    : text_(text), constants_(kCoreMathFontConstants) {}

bool MathSystem::layout(std::string_view latex,
                        MathStyle style,
                        Fx pixel_size,
                        Fx maximum_width,
                        std::shared_ptr<const MathLayoutResult>& result) {
    result.reset();
    for (CacheEntry& entry : cache_) {
        if (entry.source == latex && entry.style == style &&
            entry.pixel_size == pixel_size && entry.maximum_width == maximum_width) {
            entry.last_used = ++use_clock_;
            ++hits_;
            result = entry.result;
            return true;
        }
    }

    ++misses_;
    MathTree tree;
    const bool parsed = parse_math(latex, tree);
    std::shared_ptr<MathLayoutResult> laid_out(new MathLayoutResult());
    if (parsed && layout_math_tree(tree, text_, style, pixel_size,
                                   maximum_width, constants_, *laid_out) &&
        tree.recovered_error) {
        // Latin Modern Math has no U+26A0 warning glyph. Keep recovery
        // legible with the font's native exclamation mark.
        std::string literal = "! ";
        const std::size_t maximum = std::min<std::size_t>(latex.size(), 48);
        literal.append(latex.data(), maximum);
        if (maximum < latex.size()) literal += u8"…";
        std::replace(literal.begin(), literal.end(), '\n', ' ');
        GlyphRun run;
        if (!text_.shape(literal.data(), literal.size(), pixel_size, run,
                         FontRole::Math, TextSpacing::Natural)) return false;
        laid_out->runs.clear();
        laid_out->rules.clear();
        laid_out->metrics = {run.width, std::max<Fx>(0, run.ascent),
                             run.descent < 0 ? -run.descent : run.descent};
        laid_out->runs.push_back(
            {std::move(run), 0, 0, pixel_size, MathVariant::Roman});
        laid_out->valid = false;
        laid_out->overflow = maximum_width > 0 &&
                             laid_out->metrics.width > maximum_width;
        laid_out->diagnostic = tree.diagnostic;
    } else if (!parsed || tree.root == kInvalidMathNode ||
               (laid_out->runs.empty() && laid_out->rules.empty())) {
        MathTree fallback;
        parse_math("!", fallback);
        fallback.recovered_error = true;
        fallback.diagnostic = tree.diagnostic.empty() ? "formula could not be laid out"
                                                      : tree.diagnostic;
        if (!layout_math_tree(fallback, text_, style, pixel_size,
                              maximum_width, constants_, *laid_out)) {
            return false;
        }
    }
    CacheEntry entry;
    entry.source.assign(latex.data(), latex.size());
    entry.style = style;
    entry.pixel_size = pixel_size;
    entry.maximum_width = maximum_width;
    entry.last_used = ++use_clock_;
    entry.result = laid_out;
    cache_.push_back(std::move(entry));
    if (cache_.size() > kMaximumFormulaCacheEntries) {
        const auto victim = std::min_element(
            cache_.begin(), cache_.end(),
            [](const CacheEntry& left, const CacheEntry& right) {
                return left.last_used < right.last_used;
            });
        cache_.erase(victim);
        ++evictions_;
    }
    result = std::move(laid_out);
    return true;
}

bool MathSystem::draw(const Surface565& surface,
                      const MathLayoutResult& layout,
                      int origin_x,
                      int baseline_y,
                      int pan_x,
                      std::uint16_t foreground,
                      std::uint16_t background,
                      bool dark_theme,
    Rect clip) {
    const CoverageLut& coverage_lut = text_.coverage_lut(dark_theme);
    for (const MathRule& rule : layout.rules) {
        const Rect destination{
            origin_x + fx_floor(rule.x) - pan_x,
            baseline_y + fx_floor(rule.y),
            std::max(1, fx_ceil(rule.width)),
            std::max(1, fx_ceil(rule.height))};
        if (rule.coverage == 255) {
            fill_rect(surface, destination, foreground, clip);
            continue;
        }
        const Rect area = intersect(intersect(destination, clip), surface.bounds());
        for (int y = area.y; y < area.y + area.height; ++y) {
            std::uint16_t* row = surface.row(y);
            for (int x = area.x; x < area.x + area.width; ++x) {
                // Apply the same theme-specific coverage curve to authored
                // antialiased math strokes and FreeType glyphs so their edge
                // weight remains consistent after RGB565 quantization.
                const std::uint8_t coverage = coverage_lut[rule.coverage];
                row[x] = blend565(row[x], foreground, coverage);
            }
        }
    }
    for (const MathDrawRun& run : layout.runs) {
        const int x = origin_x + fx_floor(run.x) - pan_x;
        const int baseline = baseline_y + fx_floor(run.baseline_y);
        std::uint8_t synthesis = TextSynthesisNone;
        if (run.variant == MathVariant::Bold ||
            run.variant == MathVariant::BoldItalic ||
            run.variant == MathVariant::Blackboard) {
            synthesis |= TextSynthesisBold;
        }
        if (run.variant == MathVariant::Italic ||
            run.variant == MathVariant::Calligraphic) {
            synthesis |= TextSynthesisItalic;
        }
        if (!text_.draw_run(surface, run.glyphs, x, baseline, run.pixel_size,
                            foreground, background, dark_theme, true, clip,
                            synthesis)) {
            return false;
        }
    }
    return true;
}

void MathSystem::clear_cache() {
    cache_.clear();
}

FormulaCacheStats MathSystem::cache_stats() const {
    return {hits_, misses_, evictions_, cache_.size()};
}

}  // namespace nmarkdown
