#ifndef NMARKDOWN_TEXT_TEXT_SYSTEM_H
#define NMARKDOWN_TEXT_TEXT_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "nmarkdown/render/surface565.h"
#include "nmarkdown/text/compositor.h"
#include "nmarkdown/text/font.h"
#include "nmarkdown/text/font_pack.h"
#include "nmarkdown/text/glyph_cache.h"
#include "nmarkdown/text/render_style.h"
#include "nmarkdown/text/text_shaper.h"
#include "nmarkdown/text/text_renderer.h"

namespace nmarkdown {

struct ExternalFontUpdate {
    FontRole role = FontRole::BodySans;
    std::vector<std::uint8_t> data;
};

struct LoadedExternalFont {
    FontFaceId id = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> data;
    std::shared_ptr<RandomAccessData> source;
    std::uint32_t signature = 0;

    std::size_t byte_size() const {
        if (data != nullptr) return data->size();
        if (source == nullptr ||
            source->size() > static_cast<std::uint64_t>(SIZE_MAX)) {
            return 0;
        }
        return static_cast<std::size_t>(source->size());
    }
};

// Font payload ownership is independent from role assignment. Several role
// slots may reference one loaded font ID without duplicating bytes, FT_Face,
// HarfBuzz state, or glyph-cache identity.
struct FontRegistryState {
    std::vector<LoadedExternalFont> fonts;
    FontRoleBindings roles{};
};

class TextSystem {
public:
    TextSystem();

    bool initialize(std::string& error);
    bool set_external_font(FontRole role,
                           const std::uint8_t* data,
                           std::size_t size,
                           std::string& error);
    bool replace_external_font(FontRole role,
                               std::vector<std::uint8_t> data,
                               std::vector<std::uint8_t>& previous,
                               std::string& error);
    bool replace_external_fonts(std::vector<ExternalFontUpdate> updates,
                                std::vector<ExternalFontUpdate>& previous,
                                std::string& error);
    bool replace_font_registry(FontRegistryState registry,
                               FontRegistryState& previous,
                               std::string& error);
    FontRegistryState font_registry() const { return registry_; }
    bool ready() const { return shaper_ != nullptr && renderer_ != nullptr; }

    bool shape(const char* utf8,
               std::size_t size,
               Fx pixel_size,
               GlyphRun& output,
               FontRole preferred_role = FontRole::BodySans,
               TextSpacing spacing = TextSpacing::Natural,
               FontStyle style = FontStyle::Regular) const;
    bool shape_math_stretchy(std::uint32_t codepoint,
                             Fx pixel_size,
                             Fx target_height,
                             GlyphRun& output,
                             Fx& render_pixel_size) const;
    bool ink_bounds(const GlyphRun& run,
                    Fx pixel_size,
                    Fx& top,
                    Fx& bottom) const;
    bool draw_run(const Surface565& surface,
                  const GlyphRun& run,
                  int origin_x,
                  int baseline_y,
                  Fx pixel_size,
                  std::uint16_t foreground,
                  std::uint16_t background,
                  bool dark_theme,
                  bool uniform_background,
                  Rect clip,
                  std::uint8_t synthesis = TextSynthesisNone);
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

    GlyphCacheStats cache_stats() const { return cache_.stats(); }
    std::uint64_t glyph_cache_clear_generation() const {
        return cache_.clear_generation();
    }
    bool has_streamed_external_fonts() const {
        for (const LoadedExternalFont& resource : registry_.fonts) {
            if (resource.source != nullptr) return true;
        }
        return false;
    }
    const std::vector<std::uint8_t>& external_font_data(FontRole role) const;
    std::size_t external_font_bytes() const;
    std::size_t external_font_bytes(FontRole role) const;
    std::size_t external_font_count() const { return registry_.fonts.size(); }
    FontFaceId external_font_id(FontRole role) const;
    bool requires_synthetic_bold(const GlyphRun& run) const;
    std::uint32_t font_signature() const { return font_signature_; }
    const CoverageLut& coverage_lut(bool dark_theme) const {
        return dark_theme ? dark_lut_ : light_lut_;
    }
    RenderSharpness render_sharpness() const { return render_sharpness_; }
    void set_render_sharpness(RenderSharpness sharpness);
    void clear_cache() { cache_.clear(); }

private:
    void release_runtime();
    bool rebuild(std::string& error);

    FontPack pack_;
    FontRegistryState registry_;
    FontCollection fonts_;
    GlyphCache cache_;
    std::unique_ptr<TextShaper> shaper_;
    std::unique_ptr<TextRenderer> renderer_;
    CoverageLut light_lut_;
    CoverageLut dark_lut_;
    RenderSharpness render_sharpness_ = kDefaultRenderSharpness;
    std::uint32_t font_signature_ = 1;
};

}  // namespace nmarkdown

#endif
