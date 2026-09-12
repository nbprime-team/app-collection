/*
 * unifont_draw.h —— 用 GNU Unifont（ASCII 子集）绘制文本（app-collection 共享资源）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 数据由 fontgen.py 从 font/unifont-*.hex 生成（unifont_font.c/.h）；
 * 本文件只提供绘制与度量，供各应用共用（suika、cube3d）。
 */
#ifndef UNIFONT_DRAW_H
#define UNIFONT_DRAW_H

#include <stdint.h>
#include "unifont_font.h"      /* 生成物：unifont_rows / unifont_widths / unifont_glyph */

/* 把字符串绘制到 32bpp、行优先的帧缓冲（ARGB8888）。
 * 字形 8x16 或 16x16（见 unifont_widths），行高 16；scale 为放大倍数（>=1），
 * gap 为字符间距（像素）。超出 fb_w/fb_h 的像素丢弃。 */
void unifont_draw_text(uint32_t *fb, int fb_w, int fb_h,
                       int x, int y, const char *s,
                       int scale, int gap, uint32_t color);

/* 文本像素宽度（不含末尾 gap；空串返回 0） */
int unifont_text_width(const char *s, int scale, int gap);

#endif /* UNIFONT_DRAW_H */
