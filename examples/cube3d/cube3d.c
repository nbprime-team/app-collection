/*
 * cube3d —— HP Prime 上的 3D 线框演示（工程性示例）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 3D 管线移植自 legacy/prime-mc 的 KM3D.py（CubeGame 的渲染核心），
 * 在 C 里用软浮点重写：
 *   turn()      绕 Y 再绕 X 的旋转（KM3D 的矩阵等效算法）
 *   to3D()      相机平移 + 旋转
 *   to2D()      透视投影（fov=130，屏幕中心 (160,120)）
 *   back()      近平面（z=1）裁剪
 *   line3D()    带裁剪的 3D 线段
 *   show_floor() 地面网格（40 单位格）
 *   cube()      方块（12 条边，边长 40）
 *
 * 世界为 5x4x5 个 40 单位方块（y=0 为地面层），相机在外面俯视。
 * 交互：触摸拖动旋转视角；任意键退出。
 *
 * 未移植（见 README.txt）：面剔除（KM3D 的 del_line/del_y，需相邻方块查询）、
 * 物理（drop/Jump）、放置/破坏（cube_set/cube_break）、菜单（Mvar）。
 *
 * 公共运行支撑（事件读取、LCD、整屏拷贝、ELF 硬要求）来自
 * toolchain/examples/app-common；字体来自 resources/prime-unifont。
 */

#include <stdint.h>
#include "prime_hook.h"        /* 输入钩子：取事件必须用它，不能轮询 */
#include "unifont_draw.h"      /* 共享字体资源（GNU Unifont ASCII 子集） */
#include "app_common.h"        /* 公共运行支撑 */

#define LCD_W 320
#define LCD_H 240

/* ARGB8888 */
#define C_BG    0xff0a0f14u
#define C_EDGE  0xff39c5bbu
#define C_VERT  0xffffc857u
#define C_FLOOR 0xff2a3a4au
#define C_HUD   0xffb8c7d9u

extern void prime_sys_sleep(uint32_t ms);

/* ---- KM3D 的软浮点三角函数（无 libm） ---- */

#define PI_F      3.14159265f
#define TWO_PI_F  6.28318531f
#define DEG2RAD   0.01745329252f

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

/* ---- KM3D 的 3D 基础 ---- */

typedef struct { float x, y, z; } vec3;

#define V(x_, y_, z_) ((vec3){ (x_), (y_), (z_) })

/* KM3D 常量：方块边长 40，投影 fov 130，屏幕中心 (160,120) */
#define KM_UNIT 40.0f
#define KM_FOV  130.0f
#define KM_CX   160.0f
#define KM_CY   120.0f

/* 相机：位置 + 朝向（dt = 水平/竖直角度，单位度） */
static vec3  g_cam = { 100.0f, 150.0f, 300.0f };
static float g_dt_h = 0.0f;
static float g_dt_v = 20.0f;

/* turn()：绕 Y 再绕 X（KM3D 的 turn） */
static vec3 km_turn(vec3 p, float cy, float sy, float cx, float sx)
{
    float x2 = p.x * cy - p.z * sy;
    float z2 = p.x * sy + p.z * cy;
    float y3 = p.y * cx - z2 * sx;
    float z3 = p.y * sx + z2 * cx;

    return V(x2, y3, z3);
}

/* dtcs(dt)：KM3D 把角度换成 cos/sin（取负角） */
static void km_dtcs(float dh, float dv, float *cy, float *sy, float *cx, float *sx)
{
    *cy = fcos(-dh * DEG2RAD);
    *sy = fsin(-dh * DEG2RAD);
    *cx = fcos(-dv * DEG2RAD);
    *sx = fsin(-dv * DEG2RAD);
}

/* to3D()：先减去相机位置，再旋转 */
static vec3 km_to3d(vec3 p, float cy, float sy, float cx, float sx)
{
    p.x -= g_cam.x;
    p.y -= g_cam.y;
    p.z -= g_cam.z;
    return km_turn(p, cy, sy, cx, sx);
}

/* to2D()：透视投影 */
static void km_to2d(vec3 p, int *px, int *py)
{
    float fac = KM_FOV / p.z;

    *px = (int)(KM_CX + fac * p.x);
    *py = (int)(KM_CY - fac * p.y);
}

/* back()：把线段裁到近平面 z=1（KM3D 的 back） */
static vec3 km_back(vec3 a, vec3 b)
{
    float t = (1.0f - a.z) / (b.z - a.z);

    return V(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), 1.0f);
}

/* Bresenham（2D 帧缓冲，带裁剪） */
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
        app_put_px(fb, LCD_W, LCD_H, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* line3D()：相机变换 + 近平面裁剪 + 投影后画线 */
static void line3d(uint32_t *fb, vec3 p1, vec3 p2, uint32_t color,
                   float cy, float sy, float cx, float sx)
{
    vec3 a = km_to3d(p1, cy, sy, cx, sx);
    vec3 b = km_to3d(p2, cy, sy, cx, sx);
    int x1, y1, x2, y2;
    vec3 t;

    if (b.z < a.z) { t = a; a = b; b = t; }     /* 近端在前 */
    if (b.z <= 1.0f) return;                    /* 整段在近平面之后 */
    if (a.z < 1.0f) a = km_back(a, b);          /* 裁剪近端 */

    km_to2d(a, &x1, &y1);
    km_to2d(b, &x2, &y2);
    line(fb, x1, y1, x2, y2, color);
}

/* ---- 世界（KM3D 的 MAP：g_map[y][z][x]；y=0 为地面层，不放方块） ---- */

#define WX 5
#define WY 4
#define WZ 5

static const uint8_t g_map[WY][WZ][WX] = {
    /* y=0：地面层（地面网格单独绘制） */
    { {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0} },
    /* y=1：一圈围墙 */
    { {1,1,1,1,1}, {1,0,0,0,1}, {1,0,0,0,1}, {1,0,0,0,1}, {1,1,1,1,1} },
    /* y=2：中心柱 */
    { {0,0,0,0,0}, {0,0,0,0,0}, {0,0,1,0,0}, {0,0,0,0,0}, {0,0,0,0,0} },
    /* y=3：空 */
    { {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0} },
};

/* show_floor()：KM3D 的地面网格（y = 一个方块高，格距 40） */
static void show_floor(uint32_t *fb, float cy, float sy, float cx, float sx)
{
    int i;
    float y = KM_UNIT;

    for (i = 0; i <= WX; ++i) {
        float x = i * KM_UNIT;
        line3d(fb, V(x, y, 0.0f), V(x, y, WZ * KM_UNIT), C_FLOOR, cy, sy, cx, sx);
    }
    for (i = 0; i <= WZ; ++i) {
        float z = i * KM_UNIT;
        line3d(fb, V(0.0f, y, z), V(WX * KM_UNIT, y, z), C_FLOOR, cy, sy, cx, sx);
    }
}

/* cube()：KM3D 的方块（顶点 (x,y,z)，向下延伸 40，共 12 条边） */
static void draw_cube(uint32_t *fb, float x, float y, float z,
                      uint32_t color, float cy, float sy, float cx, float sx)
{
    float s = KM_UNIT;

    /* 顶面 */
    line3d(fb, V(x, y, z),        V(x, y, z + s),        color, cy, sy, cx, sx);
    line3d(fb, V(x, y, z + s),    V(x + s, y, z + s),    color, cy, sy, cx, sx);
    line3d(fb, V(x + s, y, z + s),V(x + s, y, z),        color, cy, sy, cx, sx);
    line3d(fb, V(x + s, y, z),    V(x, y, z),            color, cy, sy, cx, sx);
    /* 竖边 */
    line3d(fb, V(x, y, z),        V(x, y - s, z),        color, cy, sy, cx, sx);
    line3d(fb, V(x, y, z + s),    V(x, y - s, z + s),    color, cy, sy, cx, sx);
    line3d(fb, V(x + s, y, z + s),V(x + s, y - s, z + s),color, cy, sy, cx, sx);
    line3d(fb, V(x + s, y, z),    V(x + s, y - s, z),    color, cy, sy, cx, sx);
    /* 底面 */
    line3d(fb, V(x, y - s, z),        V(x, y - s, z + s),        color, cy, sy, cx, sx);
    line3d(fb, V(x, y - s, z + s),    V(x + s, y - s, z + s),    color, cy, sy, cx, sx);
    line3d(fb, V(x + s, y - s, z + s),V(x + s, y - s, z),        color, cy, sy, cx, sx);
    line3d(fb, V(x + s, y - s, z),    V(x, y - s, z),            color, cy, sy, cx, sx);
}

/* 场景：遍历 MAP 画方块（KM3D 的 cube([x*40, y*40+40, z*40])） */
static void render(uint32_t *fb)
{
    float cy, sy, cx, sx;
    int x, y, z;

    km_dtcs(g_dt_h, g_dt_v, &cy, &sy, &cx, &sx);

    app_clear_fb(fb, LCD_W * LCD_H, C_BG);
    show_floor(fb, cy, sy, cx, sx);

    for (y = 1; y < WY; ++y)
        for (z = 0; z < WZ; ++z)
            for (x = 0; x < WX; ++x)
                if (g_map[y][z][x])
                    draw_cube(fb, x * KM_UNIT, y * KM_UNIT + KM_UNIT,
                              z * KM_UNIT, C_EDGE, cy, sy, cx, sx);

    unifont_draw_text(fb, LCD_W, LCD_H, 8, 8, "CUBE3D", 1, 1, C_HUD);
    unifont_draw_text(fb, LCD_W, LCD_H, 8, LCD_H - 32, "DRAG TO ROTATE", 1, 1, C_HUD);
    unifont_draw_text(fb, LCD_W, LCD_H, 8, LCD_H - 16, "ANY KEY EXIT", 1, 1, C_HUD);
}

/* ---- 输入：固件钩子（仅触屏版，同 suika） ---- */

static volatile int   g_quit;
static volatile float g_dx, g_dy;             /* 待消费的拖动增量 */
static volatile int   g_last_x, g_last_y, g_dragging;

/* 在固件分发上下文中执行：只置标志/累积角度，重活留给主循环 */
static void on_event(void *event)
{
    uint8_t *p = (uint8_t *)event;
    uint32_t type;
    int count, i;

    prime_sys_get_event(event);                 /* 取事件（SVC #0x1003f）；trampoline 直达本回调 */
    type = app_rd32(p + 4);

    if (type == APP_EV_KEY) {                   /* 任意键 -> 退出 */
        int action = app_rd16(p + 28);
        if (action == (int)APP_KEY_DOWN || action == (int)APP_KEY_UP) g_quit = 1;
        return;
    }

    if (type != APP_EV_TICK) return;            /* 15：触摸帧 */

    count = app_rd16(p + 24);
    if (count > 8) count = 8;
    if (count < 0) count = 0;

    for (i = 0; i < count; ++i) {
        uint8_t *m = p + 28 + i * 12;
        int action = app_rd16(m + 0);
        int valid  = app_rd16(m + 4);
        int x      = app_rd16(m + 6);
        int y      = app_rd16(m + 8);

        if (valid != 0) continue;               /* 与 suika 相同的有效性判断 */

        if (action == (int)APP_TOUCH_BEGIN) {
            g_last_x = x; g_last_y = y; g_dragging = 1;
        } else if (action == (int)APP_TOUCH_MOVE && g_dragging) {
            g_dx += (float)(x - g_last_x);
            g_dy += (float)(y - g_last_y);
            g_last_x = x; g_last_y = y;
        } else if (action == (int)APP_TOUCH_END) {
            g_dragging = 0;
        }
    }
}

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

    g_quit = 0;
    if (!prime_hook_install(on_event)) {
        unifont_draw_text(framebuf, LCD_W, LCD_H, 8, 8, "HOOK FAILED", 1, 1, C_HUD);
        app_blit_fb(lcd, framebuf, LCD_W * LCD_H);
        return 1;
    }

    while (!g_quit) {
        /* 消费拖动增量 -> 视角角度（KM3D 的 dt；竖直钳制到 +-85 度） */
        if (g_dx != 0.0f || g_dy != 0.0f) {
            g_dt_h += g_dx * 0.3f;
            g_dt_v -= g_dy * 0.3f;
            if (g_dt_v >  85.0f) g_dt_v =  85.0f;
            if (g_dt_v < -85.0f) g_dt_v = -85.0f;
            g_dx = 0.0f;
            g_dy = 0.0f;
        }

        render(framebuf);
        app_blit_fb(lcd, framebuf, LCD_W * LCD_H);   /* 每帧一次整屏拷贝 */
        prime_sys_sleep(20);
    }

    prime_hook_remove();
    return 0;
}
