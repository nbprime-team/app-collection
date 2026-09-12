/*
 * cube3d —— HP Prime 上的 3D 线框演示（工程性示例）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 这个示例演示两件事：
 *   1) 不依赖 `hp_*` 运行时（其实现尚缺，见 prime-tcc/STATUS.md），
 *      直接调用固件接口自建渲染；
 *   2) 把 legacy 的 MicroPython 程序移植为 C —— 旋转与投影公式取自
 *      legacy/prime-mc/MC KMAT.hpappdir/KM3D.py 的 turn()/to3D()，
 *      在 C 中用软浮点重写（TCC-ARM 无原生浮点）。
 *
 * 固件接口（与 app-collection 内其他应用用法一致）：
 *   prime_sys_get_lcd()     → LCD 对象；其 vtable + 0x10 处是 320×240 ARGB 帧缓冲
 *   prime_sys_get_event(e)  → 非 0 表示有事件；e[1]=type，e[7]&0xffff=键 ID
 *   prime_sys_sleep(ms)     → 毫秒休眠（帧率控制）
 *
 * 操作：方向键旋转；ESC 退出。
 * 本文件只保证**编译通过**与结构正确，真机行为未验证。
 */

#include <stdint.h>

#define LCD_W 320
#define LCD_H 240
#define EVENT_WORDS 18

/* 事件类型 / 键 ID（与固件一致） */
#define EV_KEY     0x00100010u
#define EV_KEYDOWN 0x10u

#define HP_ESC   4
#define HP_UP    2
#define HP_DOWN  12
#define HP_LEFT  7
#define HP_RIGHT 8

/* ARGB8888 */
#define C_BG   0xff0a0f14u
#define C_EDGE 0xff39c5bbu
#define C_VERT 0xffffc857u
#define C_HUD  0xffb8c7d9u

extern void *prime_sys_get_lcd(void);
extern int   prime_sys_get_event(void *event);
extern void  prime_sys_sleep(uint32_t ms);

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

/* 绕 Y 再绕 X 旋转 —— 与 KM3D.turn(dtc=[cy,sy,cx,sx]) 等价 */
static vec3 turn(vec3 p, float cy, float sy, float cx, float sx)
{
    float x2 = p.x * cy - p.z * sy;
    float z2 = p.x * sy + p.z * cy;
    float y3 = p.y * cx - z2 * sx;
    float z3 = p.y * sx + z2 * cx;

    return (vec3){ x2, y3, z3 };
}

/* 透视投影 —— 与 KM3D 的取景一致（fov_N=130），相机沿 z 后退 */
#define FOV_F   130.0f
#define CAM_Z   4.0f

static void project(vec3 p, int *px, int *py)
{
    float z = p.z + CAM_Z;
    float s;

    if (z < 0.1f) z = 0.1f;                 /* 近平面保护，避免除零 */
    s = FOV_F / z;
    *px = LCD_W / 2 + (int)(p.x * s);
    *py = LCD_H / 2 - (int)(p.y * s);       /* 屏幕 Y 向下 */
}

/* ---- 绘图 ---- */

static uint32_t *lcd_framebuffer(void)
{
    uint32_t *lcd = (uint32_t *)prime_sys_get_lcd();
    uint32_t **table;

    if (!lcd) return 0;
    table = *(uint32_t ***)lcd;
    return table ? *(uint32_t **)((uint8_t *)table + 0x10) : 0;
}

static void clear(uint32_t *fb, uint32_t color)
{
    int i;
    for (i = 0; i < LCD_W * LCD_H; ++i) fb[i] = color;
}

static void put_px(uint32_t *fb, int x, int y, uint32_t color)
{
    if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
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

/* ---- 极简 8×8 字模（只含本示例用到的字符，无占位） ---- */

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
        if (glyph_chars[i] >= 'A' && glyph_chars[i] <= 'Z' &&
            c >= 'a' && c <= 'z' && glyph_chars[i] == c - 32) return i;
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
                if (g[row] & (0x80u >> col)) put_px(fb, x + col, y + row, color);
            }
        }
    }
}

/* ---- 场景 ---- */

#define STEP_ROT 0.12f

static void render(uint32_t *fb, float ax, float ay)
{
    int ix[8], iy[8];
    float cx = fcos(ax), sx = fsin(ax);
    float cy = fcos(ay), sy = fsin(ay);
    int i;

    clear(fb, C_BG);

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
    text(fb, 8, LCD_H - 16, "ARROWS ROTATE", C_HUD);
    text(fb, 8, LCD_H - 28, "ESC EXIT", C_HUD);
}

int main(void *config, void *reserved)
{
    uint32_t *lcd = lcd_framebuffer();
    uint32_t event[EVENT_WORDS];
    float ax = 0.35f, ay = 0.60f;
    int quit = 0;

    (void)config;
    (void)reserved;

    if (!lcd) return 0;

    while (!quit) {
        if (prime_sys_get_event(event)) {
            uint32_t type = event[1];
            if (type == EV_KEY || type == EV_KEYDOWN) {
                switch ((int)(event[7] & 0xffffu)) {
                case HP_ESC:   quit = 1;        break;
                case HP_UP:    ax -= STEP_ROT;  break;
                case HP_DOWN:  ax += STEP_ROT;  break;
                case HP_LEFT:  ay -= STEP_ROT;  break;
                case HP_RIGHT: ay += STEP_ROT;  break;
                default: break;
                }
            }
        }
        render(lcd, ax, ay);
        prime_sys_sleep(20);
    }
    return 0;
}
