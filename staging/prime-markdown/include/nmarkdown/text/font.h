#ifndef NMARKDOWN_TEXT_FONT_H
#define NMARKDOWN_TEXT_FONT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nmarkdown/io/random_access.h"
#include "nmarkdown/layout/fixed.h"
#include "nmarkdown/text/font_pack.h"

namespace nmarkdown {

using FontFaceId = std::uint32_t;
using GlyphId = std::uint32_t;

constexpr std::size_t kExternalFontRoleCount = 7;
using FontRoleBindings =
    std::array<FontFaceId, kExternalFontRoleCount>;

int external_font_role_index(FontRole role);
FontRole external_font_role(std::size_t index);

struct FontLineMetrics {
    Fx ascent = 0;
    Fx descent = 0;
    Fx line_gap = 0;
};

struct GlyphMetrics {
    Fx advance = 0;
    int bearing_x = 0;
    int y_offset = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct GlyphBitmap {
    GlyphMetrics metrics;
    std::vector<std::uint8_t> coverage;
};

enum GlyphRenderFlags : std::uint8_t {
    GlyphRenderNone = 0,
    GlyphRenderOblique = 1U << 0U,
};

struct MemoryFontFace {
    FontFaceId id = 0;
    FontRole role = FontRole::BodySans;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::shared_ptr<RandomAccessData> source;
};

class FontFace {
public:
    virtual ~FontFace() = default;
    virtual FontFaceId id() const = 0;
    virtual FontRole role() const = 0;
    virtual bool italic_design() const = 0;
    virtual bool bold_design() const = 0;
    virtual const std::uint8_t* font_data() const = 0;
    virtual std::size_t font_data_size() const = 0;
    virtual bool load_sfnt_table(std::uint32_t tag,
                                 std::vector<std::uint8_t>& data) const = 0;
    virtual bool glyph_for_codepoint(std::uint32_t codepoint,
                                     GlyphId& glyph) const = 0;
    virtual bool line_metrics(Fx pixel_size, FontLineMetrics& metrics) const = 0;
    virtual bool glyph_metrics(GlyphId glyph,
                               Fx pixel_size,
                               GlyphMetrics& metrics) const = 0;
    virtual Fx kerning(GlyphId left, GlyphId right, Fx pixel_size) const = 0;
    virtual bool rasterize(GlyphId glyph,
                           Fx pixel_size,
                           std::uint8_t subpixel_phase,
                           std::uint8_t render_flags,
                           GlyphBitmap& bitmap) const = 0;
};

class FreeTypeFontFace final : public FontFace {
public:
    FreeTypeFontFace() = default;
    ~FreeTypeFontFace() override;

    FreeTypeFontFace(const FreeTypeFontFace&) = delete;
    FreeTypeFontFace& operator=(const FreeTypeFontFace&) = delete;

    bool initialize(void* library,
                    const FontPackFace& packed_face,
                    std::string& error);
    bool initialize(void* library,
                    const MemoryFontFace& memory_face,
                    std::string& error);
    FontFaceId id() const override { return id_; }
    FontRole role() const override { return role_; }
    bool italic_design() const override { return italic_design_; }
    bool bold_design() const override { return bold_design_; }
    const std::uint8_t* font_data() const override { return font_data_; }
    std::size_t font_data_size() const override { return font_size_; }
    bool load_sfnt_table(std::uint32_t tag,
                         std::vector<std::uint8_t>& data) const override;
    bool glyph_for_codepoint(std::uint32_t codepoint,
                             GlyphId& glyph) const override;
    bool line_metrics(Fx pixel_size, FontLineMetrics& metrics) const override;
    bool glyph_metrics(GlyphId glyph,
                       Fx pixel_size,
                       GlyphMetrics& metrics) const override;
    Fx kerning(GlyphId left, GlyphId right, Fx pixel_size) const override;
    bool rasterize(GlyphId glyph,
                   Fx pixel_size,
                   std::uint8_t subpixel_phase,
                   std::uint8_t render_flags,
                   GlyphBitmap& bitmap) const override;

private:
    class StreamState;

    bool initialize_memory(void* library,
                           FontFaceId id,
                           FontRole role,
                           const std::uint8_t* data,
                           std::size_t size,
                           const std::vector<CodepointRange>& ranges,
                           std::string& error);
    void* font_ = nullptr;
    FontFaceId id_ = 0;
    FontRole role_ = FontRole::BodySans;
    const std::uint8_t* font_data_ = nullptr;
    std::size_t font_size_ = 0;
    bool italic_design_ = false;
    bool bold_design_ = false;
    std::vector<CodepointRange> ranges_;
    std::unique_ptr<StreamState> stream_;
};

struct ResolvedGlyph {
    const FontFace* face = nullptr;
    GlyphId glyph = 0;
    std::uint32_t rendered_codepoint = 0;
    FontRole resolved_role = FontRole::BodySans;
    bool substituted = false;
};

class FontCollection {
public:
    ~FontCollection();

    bool initialize(const FontPack& pack, std::string& error);
    bool initialize(const FontPack& pack,
                    const std::vector<MemoryFontFace>& memory_faces,
                    std::string& error);
    bool initialize(const FontPack& pack,
                    const std::vector<MemoryFontFace>& memory_faces,
                    const FontRoleBindings& bindings,
                    std::string& error);
    void reset();

    const FontFace* face(FontFaceId id) const;
    const FontFace* primary_face(FontRole preferred_role = FontRole::BodySans) const;
    FontFaceId assigned_face(FontRole role) const;
    bool resolve(std::uint32_t codepoint,
                 ResolvedGlyph& result,
                 FontRole preferred_role = FontRole::BodySans) const;
    std::size_t size() const { return faces_.size(); }

private:
    void* library_ = nullptr;
    std::vector<std::unique_ptr<FreeTypeFontFace>> faces_;
    FontRoleBindings bindings_{};
};

}  // namespace nmarkdown

#endif
