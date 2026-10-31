#ifndef NMARKDOWN_TEXT_RENDER_STYLE_H
#define NMARKDOWN_TEXT_RENDER_STYLE_H

#include <cstdint>

namespace nmarkdown {

using RenderSharpness = std::uint8_t;

constexpr RenderSharpness kMinimumRenderSharpness = 0;
constexpr RenderSharpness kBalancedRenderSharpness = 5;
constexpr RenderSharpness kMaximumRenderSharpness = 10;
constexpr RenderSharpness kDefaultRenderSharpness =
    kBalancedRenderSharpness;

constexpr RenderSharpness clamp_render_sharpness(unsigned value) {
    return value > kMaximumRenderSharpness
               ? kMaximumRenderSharpness
               : static_cast<RenderSharpness>(value);
}

}  // namespace nmarkdown

#endif
