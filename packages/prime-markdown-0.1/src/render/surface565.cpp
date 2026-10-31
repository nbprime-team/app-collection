#include "nmarkdown/render/surface565.h"

#include <algorithm>
#include <cassert>

namespace nmarkdown {

Rect intersect(Rect a, Rect b) {
    const int left = std::max(a.x, b.x);
    const int top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

Surface565::Surface565(std::uint16_t* pixels,
                       int width,
                       int height,
                       int stride)
    : pixels_(pixels), width_(width), height_(height), stride_(stride) {}

bool Surface565::valid() const {
    return pixels_ != nullptr && width_ > 0 && height_ > 0 && stride_ >= width_;
}

std::uint16_t* Surface565::row(int y) const {
    assert(valid());
    assert(y >= 0 && y < height_);
    return pixels_ + y * stride_;
}

void Surface565::clear(std::uint16_t color) const {
    if (!valid()) {
        return;
    }

    for (int y = 0; y < height_; ++y) {
        std::fill_n(row(y), width_, color);
    }
}

void Surface565::put_pixel(int x, int y, std::uint16_t color) const {
    if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }
    row(y)[x] = color;
}

std::uint16_t Surface565::pixel(int x, int y) const {
    if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_) {
        return 0;
    }
    return row(y)[x];
}

namespace {

std::uint8_t expand_channel(std::uint16_t value, unsigned bits) {
    const std::uint16_t maximum = static_cast<std::uint16_t>((1U << bits) - 1U);
    return static_cast<std::uint8_t>((value * 255U + maximum / 2U) / maximum);
}

}  // namespace

std::uint8_t red8(std::uint16_t color) {
    return expand_channel(static_cast<std::uint16_t>((color >> 11U) & 31U), 5);
}

std::uint8_t green8(std::uint16_t color) {
    return expand_channel(static_cast<std::uint16_t>((color >> 5U) & 63U), 6);
}

std::uint8_t blue8(std::uint16_t color) {
    return expand_channel(static_cast<std::uint16_t>(color & 31U), 5);
}

}  // namespace nmarkdown

