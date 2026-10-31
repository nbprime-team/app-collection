#include "nmarkdown/text/text_system.h"

#include <algorithm>
#include <exception>

#include "nmarkdown/generated/core_font_pack.h"
#include "nmarkdown/text/harfbuzz_shaper.h"

namespace nmarkdown {
namespace {

constexpr std::uint8_t kLightStemDarkening = 36;
constexpr std::uint8_t kLightEdgeContrast = 160;
constexpr std::uint8_t kDarkSharpStemDarkening = 20;
constexpr std::uint8_t kDarkSharpEdgeContrast = 112;
constexpr std::uint8_t kSmoothStemDarkening = 20;
constexpr std::uint8_t kExtraSmoothEdgeSoftness = 96;

CoverageLut interpolate_coverage_lut(const CoverageLut& smooth,
                                     const CoverageLut& sharp,
                                     unsigned numerator,
                                     unsigned denominator) {
    CoverageLut result{};
    numerator = std::min(numerator, denominator);
    const unsigned inverse = denominator - numerator;
    for (std::size_t alpha = 0; alpha < result.size(); ++alpha) {
        result[alpha] = static_cast<std::uint8_t>(
            (static_cast<unsigned>(smooth[alpha]) * inverse +
             static_cast<unsigned>(sharp[alpha]) * numerator +
             denominator / 2U) /
            denominator);
    }
    result[0] = 0;
    result[255] = 255;
    return result;
}

CoverageLut make_near_binary_coverage_lut() {
    CoverageLut result{};
    constexpr unsigned kLowerEdge = 88;
    constexpr unsigned kUpperEdge = 168;
    for (unsigned alpha = 0; alpha < result.size(); ++alpha) {
        if (alpha <= kLowerEdge) result[alpha] = 0;
        else if (alpha >= kUpperEdge) result[alpha] = 255;
        else {
            result[alpha] = static_cast<std::uint8_t>(
                (alpha - kLowerEdge) * 255U /
                (kUpperEdge - kLowerEdge));
        }
    }
    return result;
}

CoverageLut make_extra_smooth_coverage_lut(const CoverageLut& base) {
    CoverageLut result{};
    for (unsigned alpha = 0; alpha < result.size(); ++alpha) {
        const int centered = static_cast<int>(alpha) * 2 - 255;
        const int contrast =
            centered * static_cast<int>(alpha) *
            (255 - static_cast<int>(alpha)) *
            kExtraSmoothEdgeSoftness /
            (255 * 255 * 255);
        const int softened = std::max(
            0, std::min(255, static_cast<int>(alpha) - contrast));
        result[alpha] = base[static_cast<std::size_t>(softened)];
    }
    result[0] = 0;
    result[255] = 255;
    return result;
}

std::uint32_t hash_font_bytes(std::uint32_t hash,
                              const std::vector<std::uint8_t>& bytes) {
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

}  // namespace

TextSystem::TextSystem() : cache_() {
    set_render_sharpness(kDefaultRenderSharpness);
}

void TextSystem::set_render_sharpness(RenderSharpness sharpness) {
    sharpness = clamp_render_sharpness(sharpness);
    // The complete 0-10 range stays antialiased. Level 10 is exactly the old
    // level 7 curve, while level 0 pulls A8 edge coverage farther toward the
    // midpoint than the old neutral endpoint. Linear interpolation makes 5
    // the balanced default without the former near-binary/1-bit tail.
    const CoverageLut smooth_light = make_extra_smooth_coverage_lut(
        make_coverage_lut(0));
    const CoverageLut smooth_dark = make_extra_smooth_coverage_lut(
        make_dark_coverage_lut(0));
    const CoverageLut previous_smooth_light =
        make_coverage_lut(kSmoothStemDarkening);
    const CoverageLut sharp_light = make_light_coverage_lut(
        kLightStemDarkening, kLightEdgeContrast);
    const CoverageLut previous_smooth_dark =
        make_dark_coverage_lut(kSmoothStemDarkening);
    const CoverageLut sharp_dark = make_light_coverage_lut(
        kDarkSharpStemDarkening, kDarkSharpEdgeContrast);
    const CoverageLut balanced_light = interpolate_coverage_lut(
        previous_smooth_light, sharp_light, 1, 2);
    const CoverageLut balanced_dark = interpolate_coverage_lut(
        previous_smooth_dark, sharp_dark, 1, 2);
    const CoverageLut near_binary = make_near_binary_coverage_lut();
    const CoverageLut previous_level_seven_light =
        interpolate_coverage_lut(balanced_light, near_binary, 2, 4);
    const CoverageLut previous_level_seven_dark =
        interpolate_coverage_lut(balanced_dark, near_binary, 2, 4);
    render_sharpness_ = sharpness;
    light_lut_ = interpolate_coverage_lut(
        smooth_light, previous_level_seven_light, sharpness,
        kMaximumRenderSharpness);
    dark_lut_ = interpolate_coverage_lut(
        smooth_dark, previous_level_seven_dark, sharpness,
        kMaximumRenderSharpness);
}

bool TextSystem::initialize(std::string& error) {
    release_runtime();
    pack_.reset();
    registry_ = {};
    return rebuild(error);
}

void TextSystem::release_runtime() {
    shaper_.reset();
    renderer_.reset();
    fonts_.reset();
    cache_.clear();
}

bool TextSystem::rebuild(std::string& error) {
    error.clear();
    if (!pack_.load_from_memory(kCoreFontPack, kCoreFontPackSize, error)) {
        return false;
    }
    std::vector<MemoryFontFace> memory_faces;
    memory_faces.reserve(registry_.fonts.size());
    for (const LoadedExternalFont& resource : registry_.fonts) {
        if (resource.data != nullptr && !resource.data->empty()) {
            memory_faces.push_back({resource.id, FontRole::External,
                                    resource.data->data(),
                                    resource.data->size(), {}});
        } else if (resource.source != nullptr &&
                   resource.source->size() != 0) {
            memory_faces.push_back({resource.id, FontRole::External,
                                    nullptr, 0, resource.source});
        }
    }
    if (!fonts_.initialize(pack_, memory_faces, registry_.roles, error)) {
        return false;
    }
    shaper_.reset(new HarfBuzzShaper(fonts_));
    renderer_.reset(new TextRenderer(fonts_, cache_));
    font_signature_ = pack_.checksum();
    for (const LoadedExternalFont& resource : registry_.fonts) {
        if (resource.data != nullptr) {
            font_signature_ = hash_font_bytes(font_signature_, *resource.data);
        } else {
            font_signature_ ^= resource.signature;
            font_signature_ *= 16777619U;
            const std::uint64_t size = resource.source == nullptr
                                           ? 0
                                           : resource.source->size();
            for (int byte = 0; byte < 8; ++byte) {
                font_signature_ ^=
                    static_cast<std::uint8_t>(size >> (byte * 8));
                font_signature_ *= 16777619U;
            }
        }
    }
    for (FontFaceId id : registry_.roles) {
        for (int byte = 0; byte < 4; ++byte) {
            font_signature_ ^= static_cast<std::uint8_t>(id >> (byte * 8));
            font_signature_ *= 16777619U;
        }
    }
    if (font_signature_ == 0) font_signature_ = 1;
    return true;
}

bool TextSystem::set_external_font(FontRole role,
                                   const std::uint8_t* data,
                                   std::size_t size,
                                   std::string& error) {
    if (data == nullptr && size != 0) {
        error = "unsupported external font role";
        return false;
    }
    std::vector<std::uint8_t> replacement;
    if (size != 0) replacement.assign(data, data + size);
    std::vector<std::uint8_t> previous;
    return replace_external_font(role, std::move(replacement), previous, error);
}

bool TextSystem::replace_external_font(FontRole role,
                                       std::vector<std::uint8_t> data,
                                       std::vector<std::uint8_t>& previous,
                                       std::string& error) {
    std::vector<ExternalFontUpdate> updates;
    updates.push_back({role, std::move(data)});
    std::vector<ExternalFontUpdate> previous_updates;
    if (!replace_external_fonts(std::move(updates), previous_updates, error)) {
        return false;
    }
    previous.clear();
    if (!previous_updates.empty()) {
        previous = std::move(previous_updates.front().data);
    }
    return true;
}

bool TextSystem::replace_external_fonts(
    std::vector<ExternalFontUpdate> updates,
    std::vector<ExternalFontUpdate>& previous,
    std::string& error) {
    previous.clear();
    std::array<bool, kExternalFontRoleCount> changed{};
    for (const ExternalFontUpdate& update : updates) {
        const int index = external_font_role_index(update.role);
        if (index < 0 || changed[static_cast<std::size_t>(index)]) {
            error = index < 0 ? "unsupported external font role"
                              : "external font role was supplied twice";
            return false;
        }
        changed[static_cast<std::size_t>(index)] = true;
    }

    FontRegistryState next = registry_;
    previous.reserve(updates.size());
    for (ExternalFontUpdate& update : updates) {
        const int index = external_font_role_index(update.role);
        previous.push_back({update.role, external_font_data(update.role)});
        const FontFaceId compatibility_id =
            static_cast<FontFaceId>(2001 + index);
        next.roles[static_cast<std::size_t>(index)] =
            update.data.empty() ? 0 : compatibility_id;
        auto resource = std::find_if(
            next.fonts.begin(), next.fonts.end(),
            [compatibility_id](const LoadedExternalFont& item) {
                return item.id == compatibility_id;
            });
        if (!update.data.empty()) {
            std::shared_ptr<const std::vector<std::uint8_t>> bytes =
                std::make_shared<const std::vector<std::uint8_t>>(
                    std::move(update.data));
            if (resource == next.fonts.end()) {
                next.fonts.push_back(
                    {compatibility_id, std::move(bytes), {}, 0});
            } else {
                resource->data = std::move(bytes);
            }
        }
    }
    next.fonts.erase(
        std::remove_if(next.fonts.begin(), next.fonts.end(),
                       [&next](const LoadedExternalFont& resource) {
                           return std::find(next.roles.begin(), next.roles.end(),
                                            resource.id) == next.roles.end();
                       }),
        next.fonts.end());
    FontRegistryState snapshot;
    if (replace_font_registry(std::move(next), snapshot, error)) return true;
    previous.clear();
    return false;
}

bool TextSystem::replace_font_registry(FontRegistryState registry,
                                       FontRegistryState& previous,
                                       std::string& error) {
    error.clear();
    for (std::size_t index = 0; index < registry.fonts.size(); ++index) {
        const LoadedExternalFont& resource = registry.fonts[index];
        const bool has_memory =
            resource.data != nullptr && !resource.data->empty();
        const bool has_stream =
            resource.source != nullptr && resource.source->size() != 0;
        if (resource.id < 1000 || has_memory == has_stream) {
            error = "font registry contains an invalid resource";
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (registry.fonts[other].id == resource.id) {
                error = "font registry contains a duplicate resource ID";
                return false;
            }
        }
    }
    for (FontFaceId id : registry.roles) {
        if (id == 0) continue;
        const bool found = std::any_of(
            registry.fonts.begin(), registry.fonts.end(),
            [id](const LoadedExternalFont& resource) {
                return resource.id == id;
            });
        if (!found) {
            error = "font role references an unloaded resource";
            return false;
        }
    }

    previous = registry_;
    release_runtime();
    registry_ = std::move(registry);
    try {
        if (rebuild(error)) return true;
    } catch (...) {
        // Keep the old registry usable when allocation fails anywhere inside
        // FreeType, HarfBuzz, or the registry rebuild. The caller will present
        // the original exception as a recoverable font-application error.
        const std::exception_ptr failure = std::current_exception();
        release_runtime();
        registry_ = std::move(previous);
        previous = {};
        std::string restore_error;
        try {
            rebuild(restore_error);
        } catch (...) {
            // The original allocation failure remains the useful reason. At
            // extreme global pressure the restored runtime may remain empty,
            // but no half-installed new registry survives.
        }
        std::rethrow_exception(failure);
    }

    const std::string load_error = error;
    release_runtime();
    registry_ = std::move(previous);
    previous = {};
    std::string restore_error;
    rebuild(restore_error);
    error = load_error;
    return false;
}

const std::vector<std::uint8_t>& TextSystem::external_font_data(
    FontRole role) const {
    static const std::vector<std::uint8_t> empty;
    const FontFaceId id = external_font_id(role);
    if (id == 0) return empty;
    for (const LoadedExternalFont& resource : registry_.fonts) {
        if (resource.id == id && resource.data != nullptr) {
            return *resource.data;
        }
    }
    return empty;
}

std::size_t TextSystem::external_font_bytes() const {
    std::size_t total = 0;
    for (const LoadedExternalFont& font : registry_.fonts) {
        total += font.byte_size();
    }
    return total;
}

std::size_t TextSystem::external_font_bytes(FontRole role) const {
    const FontFaceId id = external_font_id(role);
    for (const LoadedExternalFont& resource : registry_.fonts) {
        if (resource.id == id) return resource.byte_size();
    }
    return 0;
}

FontFaceId TextSystem::external_font_id(FontRole role) const {
    const int index = external_font_role_index(role);
    return index < 0 ? 0 : registry_.roles[static_cast<std::size_t>(index)];
}

bool TextSystem::shape(const char* utf8,
                       std::size_t size,
                       Fx pixel_size,
                       GlyphRun& output,
                       FontRole preferred_role,
                       TextSpacing spacing,
                       FontStyle style) const {
    // An explicitly selected italic companion supplies its own family design.
    // Without one, keep the regular Body face and apply FreeType's outline
    // transform during rasterization.
    if (preferred_role == FontRole::BodySans) {
        if (style == FontStyle::BoldItalic &&
            external_font_id(FontRole::BodySansBoldItalic) != 0) {
            preferred_role = FontRole::BodySansBoldItalic;
        } else if ((style == FontStyle::Bold ||
                    style == FontStyle::BoldItalic) &&
                   external_font_id(FontRole::BodySansBold) != 0) {
            preferred_role = FontRole::BodySansBold;
        } else if ((style == FontStyle::Italic ||
                    style == FontStyle::BoldItalic) &&
                   external_font_id(FontRole::BodySansItalic) != 0) {
            preferred_role = FontRole::BodySansItalic;
        }
    } else if (preferred_role == FontRole::Monospace &&
               (style == FontStyle::Italic ||
                style == FontStyle::BoldItalic) &&
               external_font_id(FontRole::MonospaceItalic) != 0) {
        preferred_role = FontRole::MonospaceItalic;
    }
    return shaper_ != nullptr &&
           shaper_->shape(reinterpret_cast<const std::uint8_t*>(utf8),
                          size,
                          pixel_size,
                          output,
                          preferred_role,
                          spacing);
}

bool TextSystem::requires_synthetic_bold(const GlyphRun& run) const {
    for (const PositionedGlyph& positioned : run.glyphs) {
        const FontFace* face = fonts_.face(positioned.face);
        if (face != nullptr && !face->bold_design()) return true;
    }
    return false;
}

bool TextSystem::shape_math_stretchy(std::uint32_t codepoint,
                                     Fx pixel_size,
                                     Fx target_height,
                                     GlyphRun& output,
                                     Fx& render_pixel_size) const {
    return shaper_ != nullptr &&
           shaper_->shape_math_stretchy(codepoint, pixel_size, target_height,
                                        output, render_pixel_size);
}

bool TextSystem::ink_bounds(const GlyphRun& run,
                            Fx pixel_size,
                            Fx& top,
                            Fx& bottom) const {
    bool found = false;
    top = 0;
    bottom = 0;
    for (const PositionedGlyph& positioned : run.glyphs) {
        const FontFace* face = fonts_.face(positioned.face);
        GlyphMetrics metrics;
        if (face == nullptr ||
            !face->glyph_metrics(positioned.glyph, pixel_size, metrics)) {
            return false;
        }
        if (metrics.height == 0) continue;
        const Fx glyph_top = positioned.y_offset + fx_from_int(metrics.y_offset);
        const Fx glyph_bottom = glyph_top + fx_from_int(metrics.height);
        if (!found) {
            top = glyph_top;
            bottom = glyph_bottom;
            found = true;
        } else {
            top = std::min(top, glyph_top);
            bottom = std::max(bottom, glyph_bottom);
        }
    }
    return found;
}

bool TextSystem::draw_run(const Surface565& surface,
                          const GlyphRun& run,
                          int origin_x,
                          int baseline_y,
                          Fx pixel_size,
                          std::uint16_t foreground,
                          std::uint16_t background,
                          bool dark_theme,
                          bool uniform_background,
                          Rect clip,
                          std::uint8_t synthesis) {
    return renderer_ != nullptr &&
           renderer_->draw_run(surface,
                               run,
                               origin_x,
                               baseline_y,
                               pixel_size,
                               foreground,
                               background,
                               coverage_lut(dark_theme),
                               uniform_background,
                               clip,
                               synthesis);
}

std::size_t TextSystem::cache_run(const GlyphRun& run,
                                  Fx pixel_size,
                                  std::size_t maximum_glyphs,
                                  std::uint8_t synthesis) {
    return renderer_ == nullptr
               ? 0
               : renderer_->cache_run(run, pixel_size, maximum_glyphs,
                                      synthesis);
}

std::size_t TextSystem::cache_run_range(
    const GlyphRun& run,
    Fx pixel_size,
    std::size_t first_glyph,
    std::size_t maximum_glyphs,
    std::uint8_t synthesis) {
    return renderer_ == nullptr
               ? 0
               : renderer_->cache_run_range(
                     run, pixel_size, first_glyph, maximum_glyphs,
                     synthesis);
}

bool TextSystem::run_cached(const GlyphRun& run,
                            Fx pixel_size,
                            std::uint8_t synthesis) const {
    return renderer_ != nullptr &&
           renderer_->run_cached(run, pixel_size, synthesis);
}

}  // namespace nmarkdown
