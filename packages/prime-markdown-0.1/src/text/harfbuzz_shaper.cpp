#include "nmarkdown/text/harfbuzz_shaper.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "nmarkdown/document/unicode.h"
#include "nmarkdown/document/utf8.h"

extern "C" {
#include "hb-ot.h"
#include "hb.h"
}

namespace nmarkdown {
namespace {

constexpr Fx kTrackedClusterGap = 64;

bool variation_selector(std::uint32_t codepoint) {
    return (codepoint >= 0xFE00U && codepoint <= 0xFE0FU) ||
           (codepoint >= 0xE0100U && codepoint <= 0xE01EFU);
}

bool combining_mark(std::uint32_t codepoint) {
    return unicode_canonical_combining_class(codepoint) != 0;
}

struct InputCodepoint {
    std::uint32_t original = 0;
    std::uint32_t rendered = 0;
    std::uint32_t cluster = 0;
    const FontFace* face = nullptr;
    FontRole role = FontRole::BodySans;
    hb_script_t script = HB_SCRIPT_UNKNOWN;
    bool substituted = false;
    bool combining = false;
};

bool strong_script(hb_script_t script) {
    return script != HB_SCRIPT_COMMON && script != HB_SCRIPT_INHERITED &&
           script != HB_SCRIPT_UNKNOWN && script != HB_SCRIPT_INVALID;
}

}  // namespace

class HarfBuzzShaper::Impl {
public:
    ~Impl() {
        for (FontEntry& entry : fonts) {
            hb_font_destroy(entry.font);
            hb_face_destroy(entry.face);
            hb_blob_destroy(entry.blob);
        }
    }

    hb_font_t* font_for(const FontFace& face) {
        for (FontEntry& entry : fonts) {
            if (entry.id == face.id()) return entry.font;
        }
        const std::uint8_t* data = face.font_data();
        const std::size_t size = face.font_data_size();
        if (size == 0 ||
            size > static_cast<std::size_t>(
                       std::numeric_limits<unsigned>::max())) {
            return nullptr;
        }

        FontEntry entry;
        entry.id = face.id();
        if (data != nullptr) {
            entry.blob = hb_blob_create(reinterpret_cast<const char*>(data),
                                        static_cast<unsigned>(size),
                                        HB_MEMORY_MODE_READONLY,
                                        nullptr,
                                        nullptr);
            entry.face = hb_face_create(entry.blob, 0);
        } else {
            entry.face = hb_face_create_for_tables(
                &Impl::reference_table,
                const_cast<FontFace*>(&face), nullptr);
        }
        entry.font = hb_font_create(entry.face);
        // Table-provider faces intentionally have no whole-font blob. Only
        // memory-backed faces require one; both paths require a usable face,
        // font, and units-per-em value.
        if ((data != nullptr && entry.blob == nullptr) ||
            entry.face == nullptr || entry.font == nullptr ||
            hb_face_get_upem(entry.face) == 0) {
            hb_font_destroy(entry.font);
            hb_face_destroy(entry.face);
            hb_blob_destroy(entry.blob);
            return nullptr;
        }
        hb_ot_font_set_funcs(entry.font);
        fonts.push_back(entry);
        return fonts.back().font;
    }

private:
    static void destroy_table(void* data) {
        delete static_cast<std::vector<std::uint8_t>*>(data);
    }

    static hb_blob_t* reference_table(hb_face_t*,
                                      hb_tag_t tag,
                                      void* context) {
        FontFace* face = static_cast<FontFace*>(context);
        std::vector<std::uint8_t>* table =
            new (std::nothrow) std::vector<std::uint8_t>();
        if (face == nullptr || table == nullptr) {
            delete table;
            return hb_blob_get_empty();
        }
        try {
            if (!face->load_sfnt_table(tag, *table) || table->empty() ||
                table->size() > static_cast<std::size_t>(
                                    std::numeric_limits<unsigned>::max())) {
                delete table;
                return hb_blob_get_empty();
            }
        } catch (...) {
            // Never let a C++ allocation exception cross HarfBuzz's C ABI.
            delete table;
            return hb_blob_get_empty();
        }
        return hb_blob_create(
            reinterpret_cast<const char*>(table->data()),
            static_cast<unsigned>(table->size()), HB_MEMORY_MODE_READONLY,
            table, &Impl::destroy_table);
    }

    struct FontEntry {
        FontFaceId id = 0;
        hb_blob_t* blob = nullptr;
        hb_face_t* face = nullptr;
        hb_font_t* font = nullptr;
    };

    std::vector<FontEntry> fonts;
};

HarfBuzzShaper::HarfBuzzShaper(const FontCollection& fonts)
    : fonts_(fonts), impl_(new Impl()) {}

HarfBuzzShaper::~HarfBuzzShaper() = default;

bool HarfBuzzShaper::shape_math_stretchy(std::uint32_t codepoint,
                                         Fx pixel_size,
                                         Fx target_height,
                                         GlyphRun& output,
                                         Fx& render_pixel_size) const {
    output = {};
    render_pixel_size = pixel_size;
    const FontFace* face = fonts_.primary_face(FontRole::Math);
    if (face == nullptr || face->role() != FontRole::Math ||
        pixel_size <= 0 || target_height <= 0) {
        return false;
    }

    GlyphId base_glyph = 0;
    if (!face->glyph_for_codepoint(codepoint, base_glyph)) return false;
    hb_font_t* font = impl_->font_for(*face);
    if (font == nullptr || !hb_ot_math_has_data(hb_font_get_face(font))) {
        return false;
    }

    const auto set_size = [font](Fx size) {
        hb_font_set_scale(font, size, size);
        const unsigned ppem = static_cast<unsigned>(
            std::max(1, static_cast<int>((size + 32) / 64)));
        hb_font_set_ppem(font, ppem, ppem);
    };
    set_size(pixel_size);

    unsigned variant_count = 0;
    const unsigned total = hb_ot_math_get_glyph_variants(
        font, base_glyph, HB_DIRECTION_TTB, 0, &variant_count, nullptr);
    if (total == 0 || total > 64) return false;
    std::vector<hb_ot_math_glyph_variant_t> variants(total);
    variant_count = total;
    hb_ot_math_get_glyph_variants(font, base_glyph, HB_DIRECTION_TTB, 0,
                                  &variant_count, variants.data());
    if (variant_count == 0) return false;

    const hb_ot_math_glyph_variant_t* selected = &variants[variant_count - 1];
    for (unsigned index = 0; index < variant_count; ++index) {
        if (variants[index].advance >= target_height) {
            selected = &variants[index];
            break;
        }
    }
    if (selected->advance <= 0) return false;
    if (selected->advance < target_height) {
        render_pixel_size = static_cast<Fx>(std::min<std::int64_t>(
            static_cast<std::int64_t>(pixel_size) * 4,
            (static_cast<std::int64_t>(pixel_size) * target_height +
             selected->advance - 1) /
                selected->advance));
        set_size(render_pixel_size);
    }

    FontLineMetrics metrics;
    if (!face->line_metrics(render_pixel_size, metrics)) return false;
    PositionedGlyph glyph;
    glyph.face = face->id();
    glyph.glyph = selected->glyph;
    glyph.x_advance = hb_font_get_glyph_h_advance(font, selected->glyph);
    glyph.codepoint = codepoint;
    glyph.resolved_role = FontRole::Math;
    output.glyphs.push_back(glyph);
    output.width = glyph.x_advance;
    output.ascent = metrics.ascent;
    output.descent = metrics.descent;
    return true;
}

bool HarfBuzzShaper::shape(const std::uint8_t* utf8,
                           std::size_t size,
                           Fx pixel_size,
                           GlyphRun& output,
                           FontRole preferred_role,
                           TextSpacing spacing) const {
    output = {};
    const FontFace* primary = fonts_.primary_face(preferred_role);
    if (primary == nullptr || pixel_size <= 0 || (utf8 == nullptr && size != 0)) {
        return false;
    }

    FontLineMetrics primary_metrics;
    if (!primary->line_metrics(pixel_size, primary_metrics)) return false;
    output.ascent = primary_metrics.ascent;
    output.descent = primary_metrics.descent;

    std::vector<InputCodepoint> input;
    input.reserve(size);
    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    const FontFace* preceding_face = nullptr;
    FontRole preceding_role = preferred_role;
    std::size_t offset = 0;
    while (offset < size) {
        const DecodedCodepoint decoded =
            utf8_next(utf8, size, static_cast<std::uint32_t>(offset));
        offset += decoded.byte_length == 0 ? 1 : decoded.byte_length;
        if (!decoded.valid) ++output.invalid_sequence_count;

        const bool is_variation = variation_selector(decoded.value);
        const bool is_combining = combining_mark(decoded.value);
        if (is_variation && preceding_face == nullptr) continue;

        InputCodepoint item;
        item.original = decoded.value;
        item.rendered = decoded.value;
        item.cluster = decoded.byte_offset;
        item.combining = is_combining;

        GlyphId same_face_glyph = 0;
        if ((is_combining || is_variation) && preceding_face != nullptr &&
            (is_variation || preceding_face->glyph_for_codepoint(decoded.value,
                                                                  same_face_glyph))) {
            item.face = preceding_face;
            item.role = preceding_role;
        } else {
            ResolvedGlyph resolved;
            if (!fonts_.resolve(decoded.value, resolved, preferred_role) ||
                resolved.face == nullptr) {
                return false;
            }
            item.face = resolved.face;
            item.role = resolved.resolved_role;
            item.rendered = resolved.rendered_codepoint;
            item.substituted = resolved.substituted;
        }
        if (item.substituted) ++output.substitution_count;
        item.script = hb_unicode_script(unicode, item.rendered);
        input.push_back(item);
        preceding_face = item.face;
        preceding_role = item.role;

        FontLineMetrics metrics;
        if (item.face->line_metrics(pixel_size, metrics)) {
            output.ascent = std::max(output.ascent, metrics.ascent);
            output.descent = std::min(output.descent, metrics.descent);
        }
    }

    // Common and inherited characters adopt the surrounding run's script so
    // punctuation and marks remain attached to their neighboring text.
    hb_script_t preceding_script = HB_SCRIPT_UNKNOWN;
    for (InputCodepoint& item : input) {
        if (strong_script(item.script)) {
            preceding_script = item.script;
        } else if (strong_script(preceding_script)) {
            item.script = preceding_script;
        }
    }
    hb_script_t following_script = HB_SCRIPT_UNKNOWN;
    for (std::size_t index = input.size(); index-- > 0;) {
        InputCodepoint& item = input[index];
        if (strong_script(item.script)) {
            following_script = item.script;
        } else if (strong_script(following_script)) {
            item.script = following_script;
        } else {
            item.script = HB_SCRIPT_LATIN;
        }
    }

    std::size_t begin = 0;
    while (begin < input.size()) {
        const FontFace* face = input[begin].face;
        const hb_script_t script = input[begin].script;
        std::size_t end = begin + 1;
        while (end < input.size() && input[end].face == face &&
               input[end].script == script) {
            ++end;
        }

        hb_font_t* font = impl_->font_for(*face);
        hb_buffer_t* buffer = hb_buffer_create();
        if (font == nullptr || buffer == nullptr) {
            hb_buffer_destroy(buffer);
            return false;
        }
        hb_font_set_scale(font, pixel_size, pixel_size);
        const unsigned ppem = static_cast<unsigned>(
            std::max(1, static_cast<int>((pixel_size + 32) / 64)));
        hb_font_set_ppem(font, ppem, ppem);
        hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
        hb_buffer_set_script(buffer, script);
        hb_direction_t direction = hb_script_get_horizontal_direction(script);
        if (!HB_DIRECTION_IS_HORIZONTAL(direction)) direction = HB_DIRECTION_LTR;
        hb_buffer_set_direction(buffer, direction);
        hb_buffer_set_language(buffer, hb_language_from_string("und", -1));
        for (std::size_t index = begin; index < end; ++index) {
            hb_buffer_add(buffer, input[index].rendered, input[index].cluster);
        }

        hb_feature_t features[4];
        unsigned feature_count = 0;
        if (spacing == TextSpacing::Tracked) {
            static const char* const kDisabledLigatures[] = {
                "liga=0", "clig=0", "dlig=0", "hlig=0",
            };
            for (const char* feature : kDisabledLigatures) {
                if (hb_feature_from_string(feature, -1,
                                           &features[feature_count])) {
                    ++feature_count;
                }
            }
        }
        hb_shape(font, buffer, features, feature_count);

        unsigned glyph_count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer,
                                                                  &glyph_count);
        const hb_glyph_position_t* positions =
            hb_buffer_get_glyph_positions(buffer, &glyph_count);
        if ((glyph_count != 0 && (infos == nullptr || positions == nullptr)) ||
            !hb_buffer_allocation_successful(buffer)) {
            hb_buffer_destroy(buffer);
            return false;
        }

        struct OutputClusterMetadata {
            std::uint32_t cluster = 0;
            std::size_t source_index = 0;
            std::size_t next_mark_index = 0;
            std::size_t input_end = 0;
            std::size_t output_glyph_count = 0;
            bool substituted = false;
            bool has_combining = false;
        };
        const auto input_first = input.begin() +
                                 static_cast<std::ptrdiff_t>(begin);
        const auto input_last = input.begin() +
                                static_cast<std::ptrdiff_t>(end);
        const auto lower_input_cluster = [input_first, input_last](
                                             std::uint32_t cluster) {
            return std::lower_bound(
                input_first, input_last, cluster,
                [](const InputCodepoint& item, std::uint32_t value) {
                    return item.cluster < value;
                });
        };
        const auto make_metadata = [&](std::uint32_t cluster,
                                       decltype(input_first) range_end,
                                       std::size_t output_glyph_count) {
            const auto after_source = std::upper_bound(
                input_first, input_last, cluster,
                [](std::uint32_t value, const InputCodepoint& item) {
                    return value < item.cluster;
                });
            const auto range_begin = lower_input_cluster(cluster);
            OutputClusterMetadata metadata;
            metadata.cluster = cluster;
            metadata.source_index = after_source == input_first
                                        ? begin
                                        : static_cast<std::size_t>(
                                              std::distance(input.begin(),
                                                            after_source - 1));
            metadata.next_mark_index = static_cast<std::size_t>(
                std::distance(input.begin(), range_begin));
            metadata.input_end = static_cast<std::size_t>(
                std::distance(input.begin(), range_end));
            metadata.output_glyph_count = output_glyph_count;
            metadata.substituted = std::any_of(
                range_begin, range_end,
                [](const InputCodepoint& item) { return item.substituted; });
            metadata.has_combining = std::any_of(
                range_begin, range_end,
                [](const InputCodepoint& item) { return item.combining; });
            return metadata;
        };

        // Math layout shapes most atoms one at a time. Keep the overwhelmingly
        // common one-cluster path on the stack instead of allocating and
        // sorting two temporary vectors for every symbol.
        bool single_cluster = glyph_count != 0;
        for (unsigned index = 1; index < glyph_count; ++index) {
            single_cluster = single_cluster &&
                             infos[index].cluster == infos[0].cluster;
        }
        OutputClusterMetadata single_metadata;
        std::vector<std::uint32_t> output_clusters;
        std::vector<OutputClusterMetadata> cluster_metadata;
        if (single_cluster) {
            single_metadata = make_metadata(infos[0].cluster, input_last,
                                            glyph_count);
        } else if (glyph_count != 0) {
            output_clusters.reserve(glyph_count);
            for (unsigned index = 0; index < glyph_count; ++index) {
                output_clusters.push_back(infos[index].cluster);
            }
            std::sort(output_clusters.begin(), output_clusters.end());
            output_clusters.erase(
                std::unique(output_clusters.begin(), output_clusters.end()),
                output_clusters.end());
            cluster_metadata.reserve(output_clusters.size());
            for (std::size_t index = 0; index < output_clusters.size(); ++index) {
                const auto range_end = index + 1 < output_clusters.size()
                                           ? lower_input_cluster(
                                                 output_clusters[index + 1])
                                           : input_last;
                cluster_metadata.push_back(
                    make_metadata(output_clusters[index], range_end, 0));
            }
            for (unsigned index = 0; index < glyph_count; ++index) {
                auto metadata = std::lower_bound(
                    cluster_metadata.begin(), cluster_metadata.end(),
                    infos[index].cluster,
                    [](const OutputClusterMetadata& item, std::uint32_t value) {
                        return item.cluster < value;
                    });
                if (metadata != cluster_metadata.end() &&
                    metadata->cluster == infos[index].cluster) {
                    ++metadata->output_glyph_count;
                }
            }
        }

        output.glyphs.reserve(output.glyphs.size() + glyph_count);
        for (unsigned index = 0; index < glyph_count; ++index) {
            OutputClusterMetadata* metadata = nullptr;
            if (single_cluster) {
                metadata = &single_metadata;
            } else {
                auto found = std::lower_bound(
                    cluster_metadata.begin(), cluster_metadata.end(),
                    infos[index].cluster,
                    [](const OutputClusterMetadata& item, std::uint32_t value) {
                        return item.cluster < value;
                    });
                if (found != cluster_metadata.end() &&
                    found->cluster == infos[index].cluster) {
                    metadata = &*found;
                }
            }
            if (metadata == nullptr ||
                metadata->source_index >= input.size()) {
                hb_buffer_destroy(buffer);
                return false;
            }
            const InputCodepoint& source = input[metadata->source_index];
            const InputCodepoint* attached_mark = nullptr;
            if (positions[index].x_advance == 0) {
                while (metadata->next_mark_index < metadata->input_end &&
                       !input[metadata->next_mark_index].combining) {
                    ++metadata->next_mark_index;
                }
                if (metadata->next_mark_index < metadata->input_end) {
                    attached_mark = &input[metadata->next_mark_index++];
                }
            }
            PositionedGlyph glyph;
            glyph.face = face->id();
            glyph.glyph = infos[index].codepoint;
            glyph.x_advance = positions[index].x_advance;
            glyph.y_advance = -positions[index].y_advance;
            glyph.x_offset = positions[index].x_offset;
            glyph.y_offset = -positions[index].y_offset;
            glyph.source_cluster = infos[index].cluster;
            glyph.codepoint = attached_mark == nullptr
                                  ? source.rendered
                                  : attached_mark->rendered;
            glyph.resolved_role = attached_mark == nullptr
                                      ? source.role
                                      : attached_mark->role;
            glyph.substituted = metadata->substituted;
            // If HarfBuzz composes a base and mark into one output glyph, that
            // glyph still represents a combining-mark source even though it
            // has a normal advance. For decomposed clusters, only the mark
            // output is labelled as combining.
            glyph.combining_mark = source.combining || attached_mark != nullptr ||
                                   (metadata->has_combining &&
                                    metadata->output_glyph_count == 1);
            output.glyphs.push_back(glyph);
        }
        hb_buffer_destroy(buffer);
        begin = end;
    }

    if (spacing == TextSpacing::Tracked) {
        for (std::size_t index = 0; index + 1 < output.glyphs.size(); ++index) {
            if (output.glyphs[index].source_cluster !=
                output.glyphs[index + 1].source_cluster) {
                output.glyphs[index].x_advance += kTrackedClusterGap;
            }
        }
    }
    for (const PositionedGlyph& glyph : output.glyphs) {
        output.width += glyph.x_advance;
    }
    return true;
}

}  // namespace nmarkdown
