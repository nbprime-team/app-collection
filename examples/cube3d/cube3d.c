/*
 * cube3d —— HP Prime 上的 3D 线框演示（工程性示例）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 迭代记录（真机反馈）：
 *   v1：文字左右翻转、屏闪、按键无响应。
 *   v2（本版）修正：
 *     - 文字镜像：字模数据是 **LSB 在左**，渲染位序改为 `(bits >> col) & 1`
 *       （此前用 `0x80 >> col` 即 MSB 在左，导致每个字符镜像）；
 *     - 屏闪：改为**离屏 framebuf 渲染 + 每帧一次 blit**（与 suika 一致），
 *       不再直接写 LCD；
 *     - 输入：改为 suika 的解析方式（仅触屏版）——触摸拖动旋转，
 *       **任意键退出**。注意 prime_sys_get_event 的返回值不可靠，
 *       由 SDK 的 hook 统一处理（见 toolchain/sdk/prime_hook.c）。
 *
 * 输入方式为**仅触屏**（同 suika）：不依赖键码映射表。
 */

#include <stdint.h>
#include "prime_hook.h"        /* 输入钩子：取事件必须用它，不能轮询 */

#define LCD_W 320
#define LCD_H 240

/* 事件常量（取值与 suika 一致；prime-code 的实现有误，未参考） */
#define EV_TICK       15u
#define EV_KEY        0x00100010u
#define KEY_DOWN      16u
#define KEY_UP        0x00100000u
#define TOUCH_BEGIN   1u
#define TOUCH_MOVE    2u
#define TOUCH_END     8u

/* ARGB8888 */
#define C_BG   0xff0a0f14u
#define C_EDGE 0xff39c5bbu
#define C_VERT 0xffffc857u
#define C_HUD  0xffb8c7d9u

extern void *prime_sys_get_lcd(void);
extern void  prime_sys_sleep(uint32_t ms);

/* ---- 小端读取（事件缓冲按字节偏移访问；suika 同样如此） ---- */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int rd16(const uint8_t *p)
{
    return (int)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* ---- 自带的三角函数（避免链接 libm；软浮点下够用） ---- */

#define PI_F      3.14159265f
#define TWO_PI_F  6.28318531f

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

static float fcos(float x)
{
    return fsin(x + PI_F / 2.0f);
}

/* ---- 3D ---- */

typedef struct { float x, y, z; } vec3;

static const vec3 cube_verts[8] = {
    { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
    { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};
static const int cube_edges[12][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
};

/* 绕 Y 再绕 X 旋转 —— 与 legacy/prime-mc 的 KM3D.turn() 等价 */
static vec3 turn(vec3 p, float cy, float sy, float cx, float sx)
{
    float x2 = p.x * cy - p.z * sy;
    float z2 = p.x * sy + p.z * cy;
    float y3 = p.y * cx - z2 * sx;
    float z3 = p.y * sx + z2 * cx;

    return (vec3){ x2, y3, z3 };
}

#define FOV_F   130.0f
#define CAM_Z   4.0f

static void project(vec3 p, int *px, int *py)
{
    float z = p.z + CAM_Z;
    float s;

    if (z < 0.1f) z = 0.1f;                 /* 近平面保护 */
    s = FOV_F / z;
    *px = LCD_W / 2 + (int)(p.x * s);
    *py = LCD_H / 2 - (int)(p.y * s);       /* 屏幕 Y 向下 */
}

/* ---- 离屏帧缓冲（消除屏闪：渲染完成后整屏拷贝，同 suika） ---- */

static uint32_t framebuf[LCD_W * LCD_H] __attribute__((aligned(32)));

static uint32_t *lcd_framebuffer(void)
{
    uint32_t *lcd = (uint32_t *)prime_sys_get_lcd();
    uint32_t **table;

    if (!lcd) return 0;
    table = *(uint32_t ***)lcd;
    return table ? *(uint32_t **)((uint8_t *)table + 0x10) : 0;
}

static void blit_fb(uint32_t *dst, const uint32_t *src)
{
    int i;
    for (i = 0; i < LCD_W * LCD_H; ++i) dst[i] = src[i];
}

static void clear_fb(uint32_t *fb, uint32_t color)
{
    int i;
    for (i = 0; i < LCD_W * LCD_H; ++i) fb[i] = color;
}

static void put_px(uint32_t *fb, int x, int y, uint32_t color)
{
    if ((unsigned)x >= LCD_W || (unsigned)y >= LCD_H) return;
    fb[y * LCD_W + x] = color;
}

static void line(uint32_t *fb, int x0, int y0, int x1, int y1, uint32_t color)
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
        put_px(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* ---- 极简 8×8 字模（只含本示例用到的字符；数据为 LSB 在左） ---- */

static const char glyph_chars[] = " 3ABCDEIORSTUWX";
static const uint8_t glyph_bits[][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* ' ' */
    { 0x3c,0x66,0x60,0x38,0x60,0x66,0x3c,0x00 }, /* 3 */
    { 0x3c,0x66,0x66,0x7e,0x66,0x66,0x66,0x00 }, /* A */
    { 0x3e,0x66,0x66,0x3e,0x66,0x66,0x3e,0x00 }, /* B */
    { 0x3c,0x66,0x06,0x06,0x06,0x66,0x3c,0x00 }, /* C */
    { 0x3e,0x66,0x66,0x66,0x66,0x66,0x3e,0x00 }, /* D */
    { 0x7e,0x06,0x06,0x3e,0x06,0x06,0x7e,0x00 }, /* E */
    { 0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00 }, /* I */
    { 0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00 }, /* O */
    { 0x3e,0x66,0x66,0x3e,0x66,0x66,0x66,0x00 }, /* R */
    { 0x3c,0x66,0x06,0x1c,0x60,0x66,0x3c,0x00 }, /* S */
    { 0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00 }, /* T */
    { 0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00 }, /* U */
    { 0x63,0x63,0x63,0x6b,0x7f,0x77,0x63,0x00 }, /* W */
    { 0x66,0x66,0x3c,0x18,0x3c,0x66,0x66,0x00 }, /* X */
};

static int glyph_index(char c)
{
    int i;
    for (i = 0; glyph_chars[i]; ++i) {
        if (glyph_chars[i] == c) return i;
        if (c >= 'a' && c <= 'z' && glyph_chars[i] == c - 32) return i;
    }
    return 0;                                /* 未收录 -> 空格 */
}

static void text(uint32_t *fb, int x, int y, const char *s, uint32_t color)
{
    for (; *s; ++s, x += 9) {
        const uint8_t *g = glyph_bits[glyph_index(*s)];
        int row, col;
        for (row = 0; row < 8; ++row) {
            for (col = 0; col < 8; ++col) {
                /* 字模为 LSB 在左：第 col 列对应第 col 位 */
                if ((g[row] >> col) & 1u) put_px(fb, x + col, y + row, color);
            }
        }
    }
}

/* ---- 输入：固件钩子（仅触屏版，同 suika） ---- */

static volatile int   g_quit;
static volatile float g_ax = 0.35f;      /* 绕 X 倾角 */
static volatile float g_ay = 0.60f;      /* 绕 Y 偏角 */
static volatile int   g_last_x, g_last_y, g_dragging;

/* 在固件分发上下文中执行：只置标志/累积角度，重活留给主循环 */
static void on_event(void *event)
{
    uint8_t *p = (uint8_t *)event;
    uint32_t type = rd32(p + 4);
    int count, i;

    if (type == EV_KEY) {                       /* 任意键 -> 退出 */
        int action = rd16(p + 28);
        if (action == (int)KEY_DOWN || action == (int)KEY_UP) g_quit = 1;
        return;
    }

    if (type != EV_TICK) return;                /* 15：触摸帧 */

    count = rd16(p + 24);
    if (count > 8) count = 8;
    if (count < 0) count = 0;

    for (i = 0; i < count; ++i) {
        uint8_t *m = p + 28 + i * 12;
        int action = rd16(m + 0);
        int valid  = rd16(m + 4);
        int x      = rd16(m + 6);
        int y      = rd16(m + 8);

        if (valid != 0) continue;               /* 与 suika 相同的有效性判断 */

        if (action == (int)TOUCH_BEGIN) {
            g_last_x = x; g_last_y = y; g_dragging = 1;
        } else if (action == (int)TOUCH_MOVE && g_dragging) {
            g_ay += (float)(x - g_last_x) * 0.010f;   /* 横向拖动 -> 绕 Y */
            g_ax += (float)(y - g_last_y) * 0.010f;   /* 纵向拖动 -> 绕 X */
            g_last_x = x; g_last_y = y;
        } else if (action == (int)TOUCH_END) {
            g_dragging = 0;
        }
    }
}

/* ---- 场景 ---- */

static void render(uint32_t *fb, float ax, float ay)
{
    int ix[8], iy[8];
    float cx = fcos(ax), sx = fsin(ax);
    float cy = fcos(ay), sy = fsin(ay);
    int i;

    clear_fb(fb, C_BG);

    for (i = 0; i < 8; ++i) {
        vec3 r = turn(cube_verts[i], cy, sy, cx, sx);
        project(r, &ix[i], &iy[i]);
    }
    for (i = 0; i < 12; ++i) {
        int a = cube_edges[i][0], b = cube_edges[i][1];
        line(fb, ix[a], iy[a], ix[b], iy[b], C_EDGE);
    }
    for (i = 0; i < 8; ++i) {
        put_px(fb, ix[i], iy[i], C_VERT);
    }

    text(fb, 8, 8, "CUBE3D", C_HUD);
    text(fb, 8, LCD_H - 28, "DRAG TO ROTATE", C_HUD);
    text(fb, 8, LCD_H - 16, "ANY KEY EXIT", C_HUD);
}

int main(void *config, void *reserved)
{
    uint32_t *lcd = lcd_framebuffer();

    (void)config;
    (void)reserved;
    if (!lcd) return 0;

    if (!prime_hook_install(on_event)) {
        text(framebuf, 8, 8, "HOOK FAILED", C_HUD);
        blit_fb(lcd, framebuf);
        return 1;
    }

    while (!g_quit) {
        render(framebuf, (float)g_ax, (float)g_ay);
        blit_fb(lcd, framebuf);            /* 每帧一次整屏拷贝，避免撕裂/屏闪 */
        prime_sys_sleep(20);
    }

    prime_hook_remove();
    return 0;
}
