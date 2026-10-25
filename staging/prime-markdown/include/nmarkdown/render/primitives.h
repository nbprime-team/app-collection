#ifndef NMARKDOWN_RENDER_PRIMITIVES_H
#define NMARKDOWN_RENDER_PRIMITIVES_H

#include <cstdint>

#include "nmarkdown/render/surface565.h"

namespace nmarkdown {

void fill_rect(const Surface565& surface, Rect rect, std::uint16_t color);
void fill_rect(const Surface565& surface,
               Rect rect,
               std::uint16_t color,
               Rect clip);
void stroke_rect(const Surface565& surface, Rect rect, std::uint16_t color);
void stroke_rect(const Surface565& surface,
                 Rect rect,
                 std::uint16_t color,
                 Rect clip);
void draw_line(const Surface565& surface,
               int x0,
               int y0,
               int x1,
               int y1,
               std::uint16_t color);

}  // namespace nmarkdown

#endif
