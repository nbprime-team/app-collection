/*
 * unifont_draw.c —— 用 GNU Unifont（ASCII 子集）绘制文本（app-collection 共享资源）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 实现取自 suika_prime.c 的 draw_char/draw_text/text_width，泛化为"自带裁剪
 * 的帧缓冲绘制"，使各应用（suika、cube3d）共用同一份代码。
 */
#include "unifont_draw.h"

static void put_px(uint32_t *fb, int fb_w, int fb_h,
                   int x, int y, uint32_t color)
{
    if ((unsigned)x >= (unsigned)fb_w) return;
    if ((unsigned)y >= (unsigned)fb_h) return;
    fb[y * fb_w + x] = color;
}

static void draw_char(uint32_t *fb, int fb_w, int fb_h,
                      int x, int y, char c, int scale, uint32_t color)
{
    uint8_t width;
    const uint16_t *rows = unifont_glyph((unsigned char)c, &width);
    int row, col, sx, sy;

    if (scale < 1) scale = 1;

    for (row = 0; row < 16; ++row) {
        uint16_t bits = rows[row];

        for (col = 0; col < (int)width; ++col) {
            /* Unifont 位序：MSB 在左 */
            if (!(bits & (uint16_t)(1u << (width - 1 - col)))) continue;
            for (sy = 0; sy < scale; ++sy)
                for (sx = 0; sx < scale; ++sx)
                    put_px(fb, fb_w, fb_h,
                           x + col * scale + sx, y + row * scale + sy, color);
        }
    }
}

static int glyph_advance(char c, int scale, int gap)
{
    uint8_t width;

    (void)unifont_glyph((unsigned char)c, &width);
    return (int)width * (scale < 1 ? 1 : scale) + gap;
}

void unifont_draw_text(uint32_t *fb, int fb_w, int fb_h,
                       int x, int y, const char *s,
                       int scale, int gap, uint32_t color)
{
    while (*s) {
        char c = *s++;

        draw_char(fb, fb_w, fb_h, x, y, c, scale, color);
        x += glyph_advance(c, scale, gap);
    }
}

int unifont_text_width(const char *s, int scale, int gap)
{
    int width = 0;

    while (*s) width += glyph_advance(*s++, scale, gap);
    return width > 0 ? width - gap : 0;
}
