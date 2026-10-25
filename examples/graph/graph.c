/*
 * graph.c —— HP Prime 上的函数绘图器（app-collection 工程示例）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 与 paint.c 互补：paint 演示**触摸**，这里演示**键盘** + 数学 + 坐标变换。
 *
 * 覆盖的常用用法：
 *   1) 键盘：APP_EV_KEY 事件 → APP_KEY_DOWN 时取扫描码 (w8 >> 16)，用
 *      hp_input.h 的 HP_KEY_* 判定；
 *   2) 数学：目标是软浮点且无 libm，sin/cos 用泰勒展开自己实现
 *      （与 cube3d 同一份），其余是多项式/除法；
 *   3) 坐标：世界坐标 <-> 屏幕像素的正交映射，便于平移/缩放；
 *   4) 绘图：逐像素列采样，相邻列之间做竖直填充；越界或跳变处断开；
 *   5) 局部刷新：只有视图变化（g_dirty）时才重画并上屏。
 *
 * 操作：
 *   LEFT / RIGHT   左右平移
 *   UP / DOWN      缩放（UP 放大，DOWN 缩小）
 *   x² 键          切换函数（sin / cos / x²/4 / 1/x）
 *   PLOT 键        复位视图
 *   ESC 或 ON      退出
 *
 * 公共运行支撑来自 toolchain/examples/app-common；字体来自 resources/prime-unifont。
 */

#include <stdint.h>
#include "prime_hook.h"
#include "hp_input.h"          /* HP_KEY_* */
#include "unifont_draw.h"
#include "app_common.h"

#define LCD_W 320
#define LCD_H 240
#define HUD_H 16                            /* 顶部状态条高度 */
#define PLOT_TOP HUD_H
#define MID_X (LCD_W / 2)                   /* 画面中心 x = 160 */
#define MID_Y (PLOT_TOP + (LCD_H - PLOT_TOP) / 2)   /* 画面中心 y = 128 */

#define C_BG    0xff0d1117u
#define C_GRID  0xff1b222cu
#define C_AXIS  0xff3b4252u
#define C_CURVE 0xff7ee787u
#define C_TEXT  0xffe6edf3u
#define C_ACC   0xffffc857u

/* ---- 视图状态（回调里改，主循环读取） ---- */
static volatile int   g_quit;
static volatile int   g_fn;                 /* 当前函数下标 */
static volatile int   g_dirty = 1;          /* 需要重画 */
static volatile float g_scale = 24.0f;      /* 每世界单位多少像素 */
static volatile float g_wx0;                /* 视图中心的世界 x */
static volatile float g_wy0;                /* 视图中心的世界 y */

static const char *const g_fn_names[] = { "sin(x)", "cos(x)", "x*x/4", "1/x" };
#define FN_COUNT ((int)(sizeof(g_fn_names) / sizeof(g_fn_names[0])))

/* ------------------------------------------------------------ 软浮点数学 ---- */

#define PI_F     3.14159265f
#define TWO_PI_F 6.28318531f

static float fsin(float x)
{
    float x2, x3, x5, x7;

    while (x >  PI_F) x -= TWO_PI_F;
    while (x < -PI_F) x += TWO_PI_F;
    x2 = x * x;
    x3 = x2 * x;
    x5 = x3 * x2;
    x7 = x5 * x2;
    return x - x3 / 6.0f + x5 / 120.0f - x7 / 5040.0f;
}

static float fcos(float x) { return fsin(x + PI_F / 2.0f); }

/* 返回 1 表示该点可用；0 表示奇点/无定义（曲线在此断开） */
static int eval(int idx, float x, float *out)
{
    switch (idx) {
    case 0: *out = fsin(x); break;
    case 1: *out = fcos(x); break;
    case 2: *out = x * x * 0.25f; break;             /* 缩小 4 倍，视野内更好看 */
    case 3:
        if (x > -0.02f && x < 0.02f) return 0;       /* 1/x 的奇点附近 */
        *out = 1.0f / x;
        break;
    default: *out = 0.0f; break;
    }
    /* 放大后数值可能很大，先挡住，避免 (int) 转换溢出 */
    if (*out > 1.0e6f || *out < -1.0e6f) return 0;
    return 1;
}

/* ---------------------------------------------------------------- 绘图 ---- */

static int world_to_sy(float wy)
{
    return MID_Y - (int)((wy - g_wy0) * g_scale);
}

static void hline(uint32_t *fb, int y, uint32_t c)
{
    int x;

    if (y < PLOT_TOP || y >= LCD_H) return;
    for (x = 0; x < LCD_W; ++x)
        app_put_px(fb, LCD_W, LCD_H, x, y, c);
}

static void vline(uint32_t *fb, int x, uint32_t c)
{
    int y;

    if (x < 0 || x >= LCD_W) return;
    for (y = PLOT_TOP; y < LCD_H; ++y)
        app_put_px(fb, LCD_W, LCD_H, x, y, c);
}

/* 世界整数格的网格线（太密就不画） */
static void draw_grid(uint32_t *fb)
{
    int i;

    if (g_scale < 12.0f) return;
    for (i = -64; i <= 64; ++i) {
        int gx = MID_X + (int)(((float)i - g_wx0) * g_scale);
        int gy = MID_Y - (int)(((float)i - g_wy0) * g_scale);

        if (i != 0) {                       /* 0 留给坐标轴 */
            vline(fb, gx, C_GRID);
            hline(fb, gy, C_GRID);
        }
    }
}

static void draw_axes(uint32_t *fb)
{
    vline(fb, MID_X + (int)((0.0f - g_wx0) * g_scale), C_AXIS);
    hline(fb, MID_Y - (int)((0.0f - g_wy0) * g_scale), C_AXIS);
}

static void draw_curve(uint32_t *fb)
{
    int sx, prev_sy = 0, prev_ok = 0;

    for (sx = 0; sx < LCD_W; ++sx) {
        float wx = g_wx0 + (float)(sx - MID_X) / g_scale;
        float wy;
        int sy;

        if (!eval(g_fn, wx, &wy)) { prev_ok = 0; continue; }

        sy = world_to_sy(wy);
        if (sy < PLOT_TOP || sy >= LCD_H) { prev_ok = 0; continue; }

        if (prev_ok && sy - prev_sy < 60 && prev_sy - sy < 60) {
            int a = sy < prev_sy ? sy : prev_sy;
            int b = sy < prev_sy ? prev_sy : sy;
            int y;

            for (y = a; y <= b; ++y)          /* 相邻列差 1，竖直填充即可 */
                app_put_px(fb, LCD_W, LCD_H, sx, y, C_CURVE);
        } else {
            app_put_px(fb, LCD_W, LCD_H, sx, sy, C_CURVE);
        }
        prev_sy = sy;
        prev_ok = 1;
    }
}

/* 把浮点缩放值写成 "NN.N"（不依赖 libc） */
static char *fmt_scale(char *p, float s)
{
    int iv = (int)s;
    int fv = (int)((s - (float)iv) * 10.0f + 0.5f);
    char tmp[12];
    int n = 0;

    if (fv >= 10) { iv += 1; fv = 0; }
    if (iv == 0) {
        *p++ = '0';
    } else {
        while (iv > 0) { tmp[n++] = (char)('0' + iv % 10); iv /= 10; }
        while (n > 0) *p++ = tmp[--n];
    }
    *p++ = '.';
    *p++ = (char)('0' + fv);
    return p;
}

static void draw_hud(uint32_t *fb)
{
    char buf[40];
    char *p = buf;
    const char *s = g_fn_names[g_fn];

    /* 顶部窄条背景 */
    {
        int x, y;

        for (y = 0; y < HUD_H; ++y)
            for (x = 0; x < LCD_W; ++x)
                app_put_px(fb, LCD_W, LCD_H, x, y, C_BG);
    }

    { const char *t = "f(x)="; while (*t) *p++ = *t++; }
    while (*s) *p++ = *s++;
    { const char *t = "  zoom="; while (*t) *p++ = *t++; }
    p = fmt_scale(p, g_scale);
    *p = 0;

    unifont_draw_text(fb, LCD_W, LCD_H, 4, 0, buf, 1, 1, C_TEXT);
    unifont_draw_text(fb, LCD_W, LCD_H, LCD_W - 7 * 6, 0, "ESC=out", 1, 1, C_ACC);
}

/* ---------------------------------------------------------------- 事件 ---- */

static void on_event(void *event)
{
    uint8_t *p = (uint8_t *)event;
    uint32_t type;

    prime_sys_get_event(event);
    type = app_rd32(p + 4);

    if (type != APP_EV_KEY) return;

    {
        int action = app_rd16(p + 28);
        unsigned w8 = (unsigned)app_rd32(p + 32);

        if (action != (int)APP_KEY_DOWN) return;

        switch ((int)(w8 >> 16)) {
        case HP_KEY_ESC:
        case HP_KEY_ON:      g_quit = 1; break;
        case HP_KEY_LEFT:    g_wx0 -= 12.0f / g_scale; g_dirty = 1; break;
        case HP_KEY_RIGHT:   g_wx0 += 12.0f / g_scale; g_dirty = 1; break;
        case HP_KEY_UP:      if (g_scale < 400.0f) g_scale *= 1.25f; g_dirty = 1; break;
        case HP_KEY_DOWN:    if (g_scale > 1.5f)   g_scale /= 1.25f; g_dirty = 1; break;
        case HP_KEY_X2:      g_fn = (g_fn + 1) % FN_COUNT;            g_dirty = 1; break;
        case HP_KEY_F2:      g_scale = 24.0f; g_wx0 = 0.0f; g_wy0 = 0.0f; g_dirty = 1; break;
        default: break;
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

    if (!prime_hook_install(on_event)) return 1;

    while (!g_quit) {
        if (g_dirty) {
            g_dirty = 0;
            app_clear_fb(framebuf, LCD_W * LCD_H, C_BG);
            draw_grid(framebuf);
            draw_axes(framebuf);
            draw_curve(framebuf);
            draw_hud(framebuf);
            app_blit_fb(lcd, framebuf, LCD_W * LCD_H);
        }
        prime_sys_sleep(20);
    }

    prime_hook_remove();
    return 0;
}
