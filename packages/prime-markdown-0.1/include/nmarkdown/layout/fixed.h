#ifndef NMARKDOWN_LAYOUT_FIXED_H
#define NMARKDOWN_LAYOUT_FIXED_H

#include <cstdint>

namespace nmarkdown {

using Fx = std::int32_t;

constexpr Fx fx_from_int(int value) {
    return static_cast<Fx>(static_cast<std::int64_t>(value) * 64);
}
constexpr int fx_floor(Fx value) { return value >> 6; }
constexpr int fx_ceil(Fx value) { return (value + 63) >> 6; }
constexpr Fx fx_from_double(double value) {
    return static_cast<Fx>(value >= 0.0 ? value * 64.0 + 0.5
                                        : value * 64.0 - 0.5);
}

}  // namespace nmarkdown

#endif
