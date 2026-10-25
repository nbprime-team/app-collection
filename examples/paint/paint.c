/*
 * paint.c —— HP Prime 上的触摸绘图板（app-collection 工程示例）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 本文件是"新人实操项目"的参考实现，刻意把**常用用法**集中在一处：
 *   1) 事件：走 SDK 原生钩子（prime_hook_install），在回调里 prime_sys_get_event；
 *      按键与触摸分别用 APP_EV_KEY / APP_EV_TICK 判大类，触摸帧是**多触点列表**；
 *   2) 显示：离屏帧缓冲 + 每帧一次 app_blit_fb；只重绘**会变的部分**（工具条），
 *      画布内容就留在帧缓冲里 —— 最小内存做法，不额外开画布缓冲；
 *   3) 绘图：Bresenham 画线 + 方形笔头；矩形填充；工具条上的命中检测；
 *   4) 文本：共享字体资源 unifont_draw_text。
 *
 * 操作：
 *   画布区（上方 200 行）：触摸拖动 = 画
 *   工具条（底部 40 行）  ：8 色 / 3 档笔刷 / CLEAR
 *   ESC 或 ON             ：退出（退出前 prime_hook_remove 还原钩子）
 *
 * 公共运行支撑（事件读取、LCD、整屏拷贝、ELF 硬要求）来自
 * toolchain/examples/app-common；字体来自 resources/prime-unifont。
 */

#include <stdint.h>
#include "prime_hook.h"        /* 输入钩子 + prime_sys_sleep */
#include "hp_input.h"          /* HP_KEY_ESC / HP_KEY_ON 等扫描码 */
#include "unifont_draw.h"      /* 共享字体资源 */
#include "app_common.h"        /* 公共运行支撑 */

#define LCD_W 320
#define LCD_H 240

/* 画布在 y ∈ [0, CANVAS_H)，工具条在其下 */
#define CANVAS_H 200
#define TOOL_Y   CANVAS_H

/* ARGB8888 */
#define C_TOOLBG 0xff20242cu
#define C_TOOLFG 0xffe6edf3u
#define C_DIVIDE 0xff3b4252u
#define C_SEL    0xffffc857u

/* ---- 工具条布局（全部硬编码，便于对照） ---- */
#define SW       22                 /* 色块边长 */
#define SGAP     2                  /* 色块间距 */
#define SX0      2                  /* 首个色块 x */
#define SH       22                 /* 工具行高度 */
#define BRUSH_X  200                /* 笔刷按钮起始 x */
#define BRUSH_W  22                 /* 每个笔刷按钮宽 */
#define CLEAR_X  272                /* CLEAR 按钮 x */
#define CLEAR_W  46                 /* CLEAR 按钮宽 */
#define HINT_Y   224                /* 提示行 y */

static const uint32_t g_palette[8] = {
    0xff000000u, 0xffffffffu, 0xffe53935u, 0xff43a047u,
    0xff1e88e5u, 0xfffdd835u, 0xff00acc1u, 0xffd81b60u
};
static const int g_brush_sizes[3] = { 1, 3, 5 };

/* ---- 状态（回调只置标志/坐标，重活留给主循环） ---- */
static volatile int g_quit;
static volatile int g_drawing;          /* 是否正在画（触摸按下且未抬起） */
static volatile int g_last_x, g_last_y; /* 上一个触摸点 */
static volatile int g_pen_x, g_pen_y;   /* 当前笔位置（用于画光标） */
static volatile int g_have_pen;         /* 笔位置是否有效 */

static int g_color_idx = 2;             /* 默认红色 */
static int g_brush_idx = 1;             /* 默认 3px */
static int g_dirty_toolbar = 1;         /* 工具条需要重绘 */

/* ---------------------------------------------------------------- 绘图 ---- */

static void fill_rect(uint32_t *fb, int x, int y, int w, int h, uint32_t c)
{
    int i, j;

    for (j = 0; j < h; ++j)
        for (i = 0; i < w; ++i)
            app_put_px(fb, LCD_W, LCD_H, x + i, y + j, c);
}

static void rect_outline(uint32_t *fb, int x, int y, int w, int h, uint32_t c)
{
    int i;

    for (i = 0; i < w; ++i) {
        app_put_px(fb, LCD_W, LCD_H, x + i, y, c);
        app_put_px(fb, LCD_W, LCD_H, x + i, y + h - 1, c);
    }
    for (i = 0; i < h; ++i) {
        app_put_px(fb, LCD_W, LCD_H, x, y + i, c);
        app_put_px(fb, LCD_W, LCD_H, x + w - 1, y + i, c);
    }
}

/* 方形笔头：以 (x,y) 为中心画 size×size 的实心块，只画在画布区内 */
static void stamp(uint32_t *fb, int x, int y, int size, uint32_t c)
{
    int half = size / 2;
    int i, j;

    for (j = -half; j <= half; ++j) {
        for (i = -half; i <= half; ++i) {
            int py = y + j;

            if (py < 0 || py >= CANVAS_H) continue;   /* 不越到工具条上 */
            app_put_px(fb, LCD_W, LCD_H, x + i, py, c);
        }
    }
}

/* Bresenham 连线，沿途用方形笔头盖章（触屏采样点稀疏，必须连线） */
static void stroke(uint32_t *fb, int x0, int y0, int x1, int y1,
                   int size, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    err = dx - dy;

    for (;;) {
        int e2;

        stamp(fb, x0, y0, size, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ---- 工具条 ---- */

static void draw_toolbar(uint32_t *fb)
{
    int i;

    fill_rect(fb, 0, TOOL_Y, LCD_W, LCD_H - TOOL_Y, C_TOOLBG);
    /* 与画布的分隔线 */
    for (i = 0; i < LCD_W; ++i)
        app_put_px(fb, LCD_W, LCD_H, i, TOOL_Y, C_DIVIDE);

    /* 调色板 */
    for (i = 0; i < 8; ++i) {
        int x = SX0 + i * (SW + SGAP);

        fill_rect(fb, x, TOOL_Y + 2, SW, SH, g_palette[i]);
        if (i == g_color_idx)
            rect_outline(fb, x - 1, TOOL_Y + 1, SW + 2, SH + 2, C_SEL);
    }

    /* 笔刷档位（用实心方块大小表示） */
    for (i = 0; i < 3; ++i) {
        int x = BRUSH_X + i * BRUSH_W, s = g_brush_sizes[i];
        int cx = x + BRUSH_W / 2, cy = TOOL_Y + 2 + SH / 2;

        fill_rect(fb, x, TOOL_Y + 2, BRUSH_W - 2, SH, 0xff2b3038u);
        fill_rect(fb, cx - s / 2, cy - s / 2, s, s, C_TOOLFG);
        if (i == g_brush_idx)
            rect_outline(fb, x, TOOL_Y + 1, BRUSH_W - 1, SH + 2, C_SEL);
    }

    /* CLEAR 按钮 */
    fill_rect(fb, CLEAR_X, TOOL_Y + 2, CLEAR_W, SH, 0xff55202au);
    unifont_draw_text(fb, LCD_W, LCD_H, CLEAR_X + 6, TOOL_Y + 5,
                      "CLEAR", 1, 1, C_TOOLFG);

    unifont_draw_text(fb, LCD_W, LCD_H, 4, HINT_Y,
                      "touch=drag to draw   ESC/ON=quit", 1, 1, C_TOOLFG);
}

/* 工具条命中检测：返回 1 表示已处理（不要当作画画） */
static int hit_toolbar(uint32_t *fb, int x, int y)
{
    int i;

    for (i = 0; i < 8; ++i) {
        int bx = SX0 + i * (SW + SGAP);

        if (x >= bx && x < bx + SW && y >= TOOL_Y + 2 && y < TOOL_Y + 2 + SH) {
            g_color_idx = i;
            g_dirty_toolbar = 1;
            return 1;
        }
    }
    for (i = 0; i < 3; ++i) {
        int bx = BRUSH_X + i * BRUSH_W;

        if (x >= bx && x < bx + BRUSH_W && y >= TOOL_Y + 2 && y < TOOL_Y + 2 + SH) {
            g_brush_idx = i;
            g_dirty_toolbar = 1;
            return 1;
        }
    }
    if (x >= CLEAR_X && x < CLEAR_X + CLEAR_W &&
        y >= TOOL_Y + 2 && y < TOOL_Y + 2 + SH) {
        (void)fb;
        fill_rect(fb, 0, 0, LCD_W, CANVAS_H, 0xffffffffu);   /* 画布清成白纸 */
        g_dirty_toolbar = 1;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 事件 ---- */

static void on_event(void *event)
{
    uint8_t *p = (uint8_t *)event;
    uint32_t type;

    prime_sys_get_event(event);                 /* SDK 契约：回调内取事件 */
    type = app_rd32(p + 4);

    if (type == APP_EV_KEY) {                   /* 按键：ESC / ON 退出 */
        int action = app_rd16(p + 28);
        unsigned w8 = (unsigned)app_rd32(p + 32);

        if (action == (int)APP_KEY_DOWN) {
            int scan = (int)(w8 >> 16);

            if (scan == HP_KEY_ESC || scan == HP_KEY_ON) g_quit = 1;
        }
        return;
    }

    if (type != APP_EV_TICK) return;            /* 15：触摸帧 */

    {
        int count = app_rd16(p + 24);
        int i;

        if (count > 8) count = 8;
        if (count < 0) count = 0;

        for (i = 0; i < count; ++i) {
            uint8_t *m = p + 28 + i * 12;
            int action = app_rd16(m + 0);
            int valid  = app_rd16(m + 4);
            int x      = app_rd16(m + 6);
            int y      = app_rd16(m + 8);

            if (valid != 0) continue;           /* 与 suika/cube3d 同一判据 */

            if (action == (int)APP_TOUCH_BEGIN) {
                g_pen_x = x; g_pen_y = y; g_have_pen = 1;
                if (y >= TOOL_Y) {              /* 点工具条 */
                    g_drawing = 0;
                } else {
                    g_drawing = 1;
                    g_last_x = x; g_last_y = y;
                }
            } else if (action == (int)APP_TOUCH_MOVE) {
                g_pen_x = x; g_pen_y = y; g_have_pen = 1;
            } else if (action == (int)APP_TOUCH_END) {
                g_drawing = 0;
            }
        }
    }
}

/* ---------------------------------------------------------------- 入口 ---- */

static uint32_t framebuf[LCD_W * LCD_H] __attribute__((aligned(32)));

__attribute__((section(".text.main"), noinline))
int main(void *config, void *reserved)
{
    uint32_t *lcd;

    (void)config;
    (void)reserved;
    app_elf_requirements();
    lcd = app_lcd_framebuffer();
    if (!lcd) return 0;

    /* 初始化：白纸画布 + 工具条 */
    app_clear_fb(framebuf, LCD_W * LCD_H, 0xffffffffu);
    draw_toolbar(framebuf);
    app_blit_fb(lcd, framebuf, LCD_W * LCD_H);

    if (!prime_hook_install(on_event)) return 1;

    while (!g_quit) {
        /* 1) 工具条命中（触摸按下落在工具条上时） */
        if (g_have_pen && g_pen_y >= TOOL_Y && !g_drawing) {
            g_have_pen = 0;
            hit_toolbar(framebuf, g_pen_x, g_pen_y);
        }

        /* 2) 画线：把回调记录的新位置接到上一位置 */
        if (g_drawing) {
            int x = g_pen_x, y = g_pen_y;

            if (y >= CANVAS_H) y = CANVAS_H - 1;
            if (g_have_pen) {
                stroke(framebuf, g_last_x, g_last_y, x, y,
                       g_brush_sizes[g_brush_idx], g_palette[g_color_idx]);
                g_last_x = x; g_last_y = y;
            }
        }

        /* 3) 只重绘工具条（画布内容留在帧缓冲里） */
        if (g_dirty_toolbar) {
            g_dirty_toolbar = 0;
            draw_toolbar(framebuf);
        }

        app_blit_fb(lcd, framebuf, LCD_W * LCD_H);
        prime_sys_sleep(20);                     /* 约 50 fps 上限 */
    }

    prime_hook_remove();
    return 0;
}
