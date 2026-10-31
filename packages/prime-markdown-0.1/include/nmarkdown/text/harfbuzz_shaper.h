#ifndef NMARKDOWN_TEXT_HARFBUZZ_SHAPER_H
#define NMARKDOWN_TEXT_HARFBUZZ_SHAPER_H

#include <memory>

#include "nmarkdown/text/text_shaper.h"

namespace nmarkdown {

class HarfBuzzShaper final : public TextShaper {
public:
    explicit HarfBuzzShaper(const FontCollection& fonts);
    ~HarfBuzzShaper() override;

    HarfBuzzShaper(const HarfBuzzShaper&) = delete;
    HarfBuzzShaper& operator=(const HarfBuzzShaper&) = delete;

    bool shape(const std::uint8_t* utf8,
               std::size_t size,
               Fx pixel_size,
               GlyphRun& output,
               FontRole preferred_role = FontRole::BodySans,
               TextSpacing spacing = TextSpacing::Natural) const override;

    bool shape_math_stretchy(std::uint32_t codepoint,
                             Fx pixel_size,
                             Fx target_height,
                             GlyphRun& output,
                             Fx& render_pixel_size) const override;

private:
    class Impl;

    const FontCollection& fonts_;
    mutable std::unique_ptr<Impl> impl_;
};

}  // namespace nmarkdown

#endif
