#include "nmarkdown/render/primitives.h"

#include <algorithm>
#include <cstdlib>

namespace nmarkdown {

void fill_rect(const Surface565& surface, Rect rect, std::uint16_t color) {
    fill_rect(surface, rect, color, surface.bounds());
}

void fill_rect(const Surface565& surface,
               Rect rect,
               std::uint16_t color,
               Rect clip) {
    if (!surface.valid()) {
        return;
    }

    const Rect area = intersect(intersect(rect, clip), surface.bounds());
    if (area.empty()) {
        return;
    }

    for (int y = area.y; y < area.y + area.height; ++y) {
        std::fill_n(surface.row(y) + area.x, area.width, color);
    }
}

void stroke_rect(const Surface565& surface, Rect rect, std::uint16_t color) {
    stroke_rect(surface, rect, color, surface.bounds());
}

void stroke_rect(const Surface565& surface,
                 Rect rect,
                 std::uint16_t color,
                 Rect clip) {
    if (rect.empty()) {
        return;
    }
    fill_rect(surface, {rect.x, rect.y, rect.width, 1}, color, clip);
    fill_rect(surface,
              {rect.x, rect.y + rect.height - 1, rect.width, 1},
              color,
              clip);
    fill_rect(surface, {rect.x, rect.y, 1, rect.height}, color, clip);
    fill_rect(surface,
              {rect.x + rect.width - 1, rect.y, 1, rect.height},
              color,
              clip);
}

void draw_line(const Surface565& surface,
               int x0,
               int y0,
               int x1,
               int y1,
               std::uint16_t color) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        surface.put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

}  // namespace nmarkdown
