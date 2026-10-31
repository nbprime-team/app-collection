#ifndef NMARKDOWN_RENDER_SURFACE565_H
#define NMARKDOWN_RENDER_SURFACE565_H

#include <cstdint>

namespace nmarkdown {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }
};

Rect intersect(Rect a, Rect b);

class Surface565 {
public:
    Surface565() = default;
    Surface565(std::uint16_t* pixels, int width, int height, int stride);

    bool valid() const;
    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    Rect bounds() const { return {0, 0, width_, height_}; }

    std::uint16_t* row(int y) const;
    void clear(std::uint16_t color) const;
    void put_pixel(int x, int y, std::uint16_t color) const;
    std::uint16_t pixel(int x, int y) const;

private:
    std::uint16_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
};

constexpr std::uint16_t rgb565(std::uint8_t red,
                               std::uint8_t green,
                               std::uint8_t blue) {
    return static_cast<std::uint16_t>(((red >> 3U) << 11U) |
                                      ((green >> 2U) << 5U) |
                                      (blue >> 3U));
}

std::uint8_t red8(std::uint16_t color);
std::uint8_t green8(std::uint16_t color);
std::uint8_t blue8(std::uint16_t color);

}  // namespace nmarkdown

#endif

