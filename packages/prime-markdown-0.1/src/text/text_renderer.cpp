#include "nmarkdown/text/text_renderer.h"

#include <algorithm>

namespace nmarkdown {
namespace {

struct QuarterPixelOrigin {
    int pixel = 0;
    std::uint8_t phase = 0;
};

QuarterPixelOrigin quantize_origin(Fx value) {
    QuarterPixelOrigin result;
    result.pixel = fx_floor(value);
    const Fx fraction = value - fx_from_int(result.pixel);
    int quarter = static_cast<int>((fraction + 8) / 16);
    if (quarter >= 4) {
        ++result.pixel;
        quarter = 0;
    }
    result.phase = static_cast<std::uint8_t>(quarter);
    return result;
}

std::uint8_t glyph_render_flags(const FontFace& face,
                                FontRole resolved_role,
                                std::uint8_t synthesis) {
    if ((synthesis & TextSynthesisItalic) == 0 || face.italic_design() ||
        resolved_role == FontRole::Cjk) {
        return GlyphRenderNone;
    }
    return GlyphRenderOblique;
}

}  // namespace

bool TextRenderer::draw_run(const Surface565& surface,
                            const GlyphRun& run,
                            int origin_x,
                            int baseline_y,
                            Fx pixel_size,
                            std::uint16_t foreground,
                            std::uint16_t background,
                            const CoverageLut& coverage_lut,
                            bool uniform_background,
                            Rect clip,
                            std::uint8_t synthesis) {
    CoveragePalette565 palette{};
    const CoveragePalette565* uniform_palette = nullptr;
    if (uniform_background) {
        palette = make_coverage_palette565(foreground, background, coverage_lut);
        uniform_palette = &palette;
    }

    Fx pen = 0;
    for (const PositionedGlyph& positioned : run.glyphs) {
        const FontFace* face = fonts_.face(positioned.face);
        if (face == nullptr) {
            return false;
        }
        const QuarterPixelOrigin origin =
            quantize_origin(pen + positioned.x_offset);
        const std::uint8_t render_flags =
            glyph_render_flags(*face, positioned.resolved_role, synthesis);
        GlyphCacheHandle cached;
        if (!cache_.get(*face, positioned.glyph, pixel_size,
                        origin.phase, render_flags, cached)) {
            return false;
        }
        if (cached.drawable()) {
            const std::uint8_t* coverage = cache_.coverage(cached);
            if (coverage == nullptr) {
                return false;
            }
            const int x = origin_x + origin.pixel +
                          cached.metrics.bearing_x;
            const int y = baseline_y + cached.metrics.y_offset +
                          fx_floor(positioned.y_offset);
            const int passes =
                (synthesis & TextSynthesisBold) != 0 && !face->bold_design()
                    ? 2 : 1;
            for (int pass = 0; pass < passes; ++pass) {
                composite_a8_with_palette(surface,
                                 x + pass,
                                 y,
                                 coverage,
                                 cached.metrics.width,
                                 cached.metrics.height,
                                 cache_.coverage_stride(),
                                 foreground,
                                 coverage_lut,
                                 pass == 0 ? uniform_palette : nullptr,
                                 clip);
            }
        }
        pen += positioned.x_advance;
    }
    return true;
}

std::size_t TextRenderer::cache_run(const GlyphRun& run,
                                    Fx pixel_size,
                                    std::size_t maximum_glyphs,
                                    std::uint8_t synthesis) {
    return cache_run_range(
        run, pixel_size, 0, maximum_glyphs, synthesis);
}

std::size_t TextRenderer::cache_run_range(
    const GlyphRun& run,
    Fx pixel_size,
    std::size_t first_glyph,
    std::size_t maximum_glyphs,
    std::uint8_t synthesis) {
    Fx pen = 0;
    const std::size_t begin =
        std::min(first_glyph, run.glyphs.size());
    for (std::size_t index = 0; index < begin; ++index) {
        pen += run.glyphs[index].x_advance;
    }
    std::size_t cached_count = 0;
    for (std::size_t index = begin; index < run.glyphs.size(); ++index) {
        if (cached_count >= maximum_glyphs) break;
        const PositionedGlyph& positioned = run.glyphs[index];
        const FontFace* face = fonts_.face(positioned.face);
        if (face == nullptr) break;
        const QuarterPixelOrigin origin =
            quantize_origin(pen + positioned.x_offset);
        GlyphCacheHandle cached;
        if (!cache_.get(*face, positioned.glyph, pixel_size,
                        origin.phase,
                        glyph_render_flags(*face, positioned.resolved_role,
                                           synthesis),
                        cached)) {
            break;
        }
        ++cached_count;
        pen += positioned.x_advance;
    }
    return cached_count;
}

bool TextRenderer::run_cached(const GlyphRun& run,
                              Fx pixel_size,
                              std::uint8_t synthesis) const {
    Fx pen = 0;
    for (const PositionedGlyph& positioned : run.glyphs) {
        const FontFace* face = fonts_.face(positioned.face);
        if (face == nullptr) return false;
        const QuarterPixelOrigin origin =
            quantize_origin(pen + positioned.x_offset);
        if (!cache_.contains(
                *face, positioned.glyph, pixel_size, origin.phase,
                glyph_render_flags(*face, positioned.resolved_role,
                                   synthesis))) {
            return false;
        }
        pen += positioned.x_advance;
    }
    return true;
}

}  // namespace nmarkdown
