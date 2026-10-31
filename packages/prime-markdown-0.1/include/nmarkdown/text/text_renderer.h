#ifndef NMARKDOWN_TEXT_TEXT_RENDERER_H
#define NMARKDOWN_TEXT_TEXT_RENDERER_H

#include <cstddef>
#include <cstdint>

#include "nmarkdown/render/surface565.h"
#include "nmarkdown/text/compositor.h"
#include "nmarkdown/text/font.h"
#include "nmarkdown/text/glyph_cache.h"
#include "nmarkdown/text/text_shaper.h"

namespace nmarkdown {

enum TextSynthesisFlags : std::uint8_t {
    TextSynthesisNone = 0,
    TextSynthesisBold = 1U << 0U,
    TextSynthesisItalic = 1U << 1U,
};

class TextRenderer {
public:
    TextRenderer(const FontCollection& fonts, GlyphCache& cache)
        : fonts_(fonts), cache_(cache) {}

    bool draw_run(const Surface565& surface,
                  const GlyphRun& run,
                  int origin_x,
                  int baseline_y,
                  Fx pixel_size,
                  std::uint16_t foreground,
                  std::uint16_t background,
                  const CoverageLut& coverage_lut,
                  bool uniform_background,
                  Rect clip,
                  std::uint8_t synthesis = TextSynthesisNone);

    // Rasterize a bounded number of positioned glyphs into the atlas without
    // touching the framebuffer. Idle look-ahead must include this FreeType
    // work; shaping and line layout alone do not make the next paint cheap.
    std::size_t cache_run(const GlyphRun& run,
                          Fx pixel_size,
                          std::size_t maximum_glyphs,
                          std::uint8_t synthesis = TextSynthesisNone);
    std::size_t cache_run_range(
        const GlyphRun& run,
        Fx pixel_size,
        std::size_t first_glyph,
        std::size_t maximum_glyphs,
        std::uint8_t synthesis = TextSynthesisNone);
    bool run_cached(
        const GlyphRun& run,
        Fx pixel_size,
        std::uint8_t synthesis = TextSynthesisNone) const;

private:
    const FontCollection& fonts_;
    GlyphCache& cache_;
};

}  // namespace nmarkdown

#endif
