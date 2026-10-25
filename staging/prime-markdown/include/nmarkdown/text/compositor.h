#ifndef NMARKDOWN_TEXT_COMPOSITOR_H
#define NMARKDOWN_TEXT_COMPOSITOR_H

#include <array>
#include <cstdint>

#include "nmarkdown/render/surface565.h"

namespace nmarkdown {

using CoverageLut = std::array<std::uint8_t, 256>;
using CoveragePalette565 = std::array<std::uint16_t, 256>;

CoverageLut make_coverage_lut(std::uint8_t stem_darkening);

// Dark text on light RGB565 paper benefits from a steeper coverage transition:
// quiet outline fringe becomes less visible while high-coverage stem pixels
// become darker.  The curve is fixed-point, monotonic, and preserves the
// transparent/opaque endpoints.
CoverageLut make_light_coverage_lut(std::uint8_t stem_darkening,
                                    std::uint8_t edge_contrast);

// FreeType's A8 values describe geometric coverage, but blending them directly
// into RGB565 makes light glyph edges much dimmer on a dark LCD background than
// dark glyph edges on light paper.  This fixed-point correction lifts only the
// dark-theme midtones while preserving transparent/opaque endpoints.
CoverageLut make_dark_coverage_lut(std::uint8_t stem_darkening);

CoveragePalette565 make_coverage_palette565(std::uint16_t foreground,
                                            std::uint16_t background,
                                            const CoverageLut& coverage_lut);

std::uint16_t blend565(std::uint16_t destination,
                       std::uint16_t source,
                       std::uint8_t alpha);

void composite_a8(const Surface565& surface,
                  int destination_x,
                  int destination_y,
                  const std::uint8_t* coverage,
                  int width,
                  int height,
                  int coverage_stride,
                  std::uint16_t foreground,
                  std::uint16_t background,
                  const CoverageLut& coverage_lut,
                  bool uniform_background,
                  Rect clip);

void composite_a8_with_palette(const Surface565& surface,
                               int destination_x,
                               int destination_y,
                               const std::uint8_t* coverage,
                               int width,
                               int height,
                               int coverage_stride,
                               std::uint16_t foreground,
                               const CoverageLut& coverage_lut,
                               const CoveragePalette565* uniform_palette,
                               Rect clip);

}  // namespace nmarkdown

#endif
