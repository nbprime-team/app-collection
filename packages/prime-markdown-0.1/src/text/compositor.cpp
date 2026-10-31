#include "nmarkdown/text/compositor.h"

#include <algorithm>

namespace nmarkdown {
namespace {

constexpr int kMaximumCoverage = 255;

int coverage_boost(int alpha, int strength) {
    return alpha * (kMaximumCoverage - alpha) * strength /
           (kMaximumCoverage * kMaximumCoverage);
}

int coverage_contrast(int alpha, int strength) {
    // A smooth S-curve about half coverage.  Multiplying by alpha and
    // (255-alpha) keeps both endpoints exact; the signed center term quiets
    // faint fringe below the midpoint and strengthens ink above it.  The
    // largest intermediate remains within a signed 32-bit integer even when
    // strength is 255.
    const int centered = alpha * 2 - kMaximumCoverage;
    return centered * alpha * (kMaximumCoverage - alpha) * strength /
           (kMaximumCoverage * kMaximumCoverage * kMaximumCoverage);
}

}  // namespace

CoverageLut make_coverage_lut(std::uint8_t stem_darkening) {
    CoverageLut result{};
    for (int alpha = 0; alpha < 256; ++alpha) {
        const int boost = coverage_boost(alpha, stem_darkening);
        result[alpha] = static_cast<std::uint8_t>(
            std::min(kMaximumCoverage, alpha + boost));
    }
    result[0] = 0;
    result[255] = 255;
    return result;
}

CoverageLut make_light_coverage_lut(std::uint8_t stem_darkening,
                                    std::uint8_t edge_contrast) {
    CoverageLut result{};
    for (int alpha = 0; alpha < 256; ++alpha) {
        const int contrasted = std::max(
            0, std::min(kMaximumCoverage,
                        alpha + coverage_contrast(alpha, edge_contrast)));
        result[alpha] = static_cast<std::uint8_t>(std::min(
            kMaximumCoverage,
            contrasted + coverage_boost(contrasted, stem_darkening)));
    }
    result[0] = 0;
    result[255] = 255;
    return result;
}

CoverageLut make_dark_coverage_lut(std::uint8_t stem_darkening) {
    CoverageLut result{};
    // A direct A8 -> RGB565 blend under-represents the luminance of white
    // antialiasing pixels on dark paper.  A full power/gamma function would
    // require floating point (or a larger constant table) on the calculator.
    // This monotonic quadratic is a close, deliberately conservative fit over
    // the useful midrange.  Unlike a square-root approximation, it keeps the
    // near-zero toe quiet, avoiding a fuzzy halo around small glyphs.
    constexpr int kDarkLcdMidtoneCompensation = 192;
    for (int alpha = 0; alpha < 256; ++alpha) {
        const int compensated = std::min(
            kMaximumCoverage,
            alpha + coverage_boost(alpha, kDarkLcdMidtoneCompensation));
        result[alpha] = static_cast<std::uint8_t>(std::min(
            kMaximumCoverage,
            compensated + coverage_boost(compensated, stem_darkening)));
    }
    result[0] = 0;
    result[255] = 255;
    return result;
}

std::uint16_t blend565(std::uint16_t destination,
                       std::uint16_t source,
                       std::uint8_t alpha) {
    const int destination_red = (destination >> 11U) & 31U;
    const int destination_green = (destination >> 5U) & 63U;
    const int destination_blue = destination & 31U;
    const int source_red = (source >> 11U) & 31U;
    const int source_green = (source >> 5U) & 63U;
    const int source_blue = source & 31U;

    const int inverse = 255 - alpha;
    const int red = (destination_red * inverse + source_red * alpha + 127) / 255;
    const int green =
        (destination_green * inverse + source_green * alpha + 127) / 255;
    const int blue = (destination_blue * inverse + source_blue * alpha + 127) / 255;
    return static_cast<std::uint16_t>((red << 11U) | (green << 5U) | blue);
}

CoveragePalette565 make_coverage_palette565(
    std::uint16_t foreground,
    std::uint16_t background,
    const CoverageLut& coverage_lut) {
    CoveragePalette565 result{};
    for (int alpha = 0; alpha < 256; ++alpha) {
        result[alpha] = blend565(background, foreground, coverage_lut[alpha]);
    }
    return result;
}

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
                  Rect clip) {
    CoveragePalette565 palette{};
    const CoveragePalette565* uniform_palette = nullptr;
    if (uniform_background) {
        palette = make_coverage_palette565(foreground, background, coverage_lut);
        uniform_palette = &palette;
    }
    composite_a8_with_palette(surface, destination_x, destination_y, coverage,
                              width, height, coverage_stride, foreground,
                              coverage_lut, uniform_palette, clip);
}

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
                               Rect clip) {
    if (!surface.valid() || coverage == nullptr || width <= 0 || height <= 0 ||
        coverage_stride < width) {
        return;
    }
    const Rect destination{destination_x, destination_y, width, height};
    const Rect area = intersect(intersect(destination, clip), surface.bounds());
    if (area.empty()) {
        return;
    }

    const int source_x = area.x - destination_x;
    const int source_y = area.y - destination_y;
    for (int row = 0; row < area.height; ++row) {
        std::uint16_t* destination_row = surface.row(area.y + row) + area.x;
        const std::uint8_t* source_row = coverage +
                                         (source_y + row) * coverage_stride + source_x;
        for (int column = 0; column < area.width; ++column) {
            const std::uint8_t alpha = coverage_lut[source_row[column]];
            if (alpha == 0) {
                continue;
            }
            if (alpha == 255) {
                destination_row[column] = foreground;
            } else if (uniform_palette != nullptr) {
                destination_row[column] = (*uniform_palette)[source_row[column]];
            } else {
                destination_row[column] = blend565(destination_row[column],
                                                   foreground,
                                                   alpha);
            }
        }
    }
}

}  // namespace nmarkdown
