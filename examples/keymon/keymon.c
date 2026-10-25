/*
 * keymon.c —— 键盘/触摸监视器（app-collection 实用示例，依赖按键输入）
 *
 * ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * 用途：在真机上核对「nbprime 原生按键处理」这条链路是否**正确**：
 *   1. 输入走 toolchain/sdk 的原生钩子（0x307FBFA0 + SVC 0x1003f）；
 *   2. 屏幕同时显示 **原始字段**（ty4 / w7 / w8）与**扫描码**；
 *   3. 用 toolchain/sdk/include/hp_input.h 的 HP_KEY_* 反查键名，便于逐键比对。
 *
 * 显示的字段（ABI 见 toolchain/sdk/docs/hook_abi.md）：
 *   ty4 = *(u32*)(ev+4)     事件大类：0x00100010 按键 / 15 触摸
 *   w7  = *(u32*)(ev+28)    按键：16 按下 / 0x100000 抬起；触摸：1/2/8
 *   w8  = *(u32*)(ev+32)    高 16 位 = 物理扫描码（按键）或 X（触摸）
 *   w9  = *(u32*)(ev+36)    触摸 Y
 *
 * 操作：任意键查看；ESC / ON 退出。
 */

#include <stdint.h>
#include "prime_hook.h"        /* nbprime SDK：原生钩子 */
#include "hp_input.h"          /* nbprime SDK：HP_KEY_* 键名常量 */
#include "unifont_draw.h"      /* app-collection 共享字体资源 */
#include "app_common.h"        /* 公共运行支撑 */

#define LCD_W 320
#define LCD_H 240

#define C_BG   0xff0d1117u
#define C_FG   0xffe6edf3u
#define C_ACC  0xff7ee787u
#define C_WARN 0xffffa657u
#define C_DIM  0xff8b949e

extern void prime_sys_sleep(uint32_t ms);

/* hp_input.h 的键名表（现场反查用；表不全时显示 "?"） */
struct keyname { int code; const char *name; };
static const struct keyname g_keys[] = {
    { HP_KEY_ESC,       "ESC" },
    { HP_KEY_LEFT,      "LEFT" },
    { HP_KEY_UP,        "UP" },
    { HP_KEY_RIGHT,     "RIGHT" },
    { HP_KEY_DOWN,      "DOWN" },
    { HP_KEY_BACKSPACE, "BACKSPACE" },
    { HP_KEY_ENTER,     "ENTER" },
    { HP_KEY_SPACE,     "SPACE" },
    { HP_KEY_ON,        "ON" },
    { HP_KEY_SHIFT,     "SHIFT" },
    { HP_KEY_Q,         "Q (7/Q)" },
    { HP_KEY_F1,        "SYMB" },
    { HP_KEY_F2,        "PLOT" },
    { HP_KEY_F3,        "NUM" },
    { HP_KEY_F4,        "VIEW" },
    { HP_KEY_F5,        "CAS" },
    { HP_KEY_F6,        "MENU" },
    { HP_KEY_APPS,      "APPS" },
    { HP_KEY_HELP,      "HELP" },
    { HP_KEY_ALPHA,     "ALPHA" },
    { HP_KEY_PLUSMINUS, "+/-" },
    { HP_KEY_X2,        "x^2" },
};

static const char *key_name(int code)
{
    unsigned i;
    for (i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); ++i)
        if (g_keys[i].code == code) return g_keys[i].name;
    return "?";
}

/* ---- 自带格式化（不依赖 libc / prime.h 的变参宏）---- */
static char *ap(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

static char *ap_dec(char *p, int v)          /* 十进制（含负号） */
{
    char t[12];
    int n = 0;
    unsigned u;

    if (v < 0) { *p++ = '-'; u = (unsigned)(-v); } else u = (unsigned)v;
    if (u == 0) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + u % 10u); u /= 10u; }
    while (n) *p++ = t[--n];
    return p;
}

static char *ap_hex8(char *p, unsigned v)    /* 固定 8 位十六进制 */
{
    static const char d[] = "0123456789abcdef";
    int i;

    for (i = 7; i >= 0; --i) *p++ = d[(v >> (i * 4)) & 15u];
    return p;
}

static char *ap_hex2(char *p, unsigned v)    /* 2 位十六进制 */
{
    static const char d[] = "0123456789abcdef";
    *p++ = d[(v >> 4) & 15u];
    *p++ = d[v & 15u];
    return p;
}

/* ---- 事件回调（固件上下文） ---- */
static volatile unsigned g_ty4, g_w7, g_w8, g_w9;
static volatile int      g_key = -1, g_touch = 0;
static volatile int      g_tx, g_ty;
static volatile unsigned g_n;
static volatile int      g_last[8];
static volatile int      g_quit;

static void on_event(void *event)
{
    uint8_t *p = (uint8_t *)event;
    unsigned ty4;

    prime_sys_get_event(event);              /* SDK 契约：回调内取事件 */

    ty4 = app_rd32(p + 4);
    g_ty4 = ty4;

    /* 判据以 words[7] 为准（实测：16 按下 / 0x100000 抬起；触摸 1/2/8） */
    {
        unsigned w7 = app_rd32(p + 28);
        unsigned w8 = app_rd32(p + 32);

    if (w7 == 16u || w7 == 0x100000u) {      /* 按键 */
        int scan = (int)(w8 >> 16);

        g_w7 = w7; g_w8 = w8;
        g_key = scan;
        g_touch = 0;
        if (w7 == 16u) {
            int i;
            for (i = 7; i > 0; --i) g_last[i] = g_last[i - 1];
            g_last[0] = scan;
            if (scan == HP_KEY_ESC || scan == HP_KEY_ON) g_quit = 1;
        }
        if (g_n < 0xFFFFFFFFu) g_n++;
    } else if (w7 == 1u || w7 == 2u || w7 == 8u) {   /* 触摸 */
        g_w7 = w7; g_w8 = w8; g_w9 = app_rd32(p + 36);
        g_tx = (int)(w8 >> 16);
        g_ty = (int)g_w9;
        g_touch = 1;
        if (g_n < 0xFFFFFFFFu) g_n++;
    }
    }
}

static uint32_t framebuf[LCD_W * LCD_H] __attribute__((aligned(32)));

__attribute__((section(".text.main"), noinline))
int main(void *config, void *reserved)
{
    uint32_t *lcd;
    char buf[64];

    (void)config;
    (void)reserved;
    app_elf_requirements();
    lcd = app_lcd_framebuffer();
    if (!lcd) return 0;
    if (!prime_hook_install(on_event)) return 1;

    while (!g_quit) {
        int y = 6;
        app_clear_fb(framebuf, LCD_W * LCD_H, C_BG);

        unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, "KEY MONITOR", 1, 1, C_ACC);
        y += 18;

        if (g_touch) {
            /* 触摸：显示动作 + 坐标 */
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, "TOUCH", 1, 1, C_WARN);
            y += 18;
            {
                char *p = ap(buf, "x=");  p = ap_dec(p, g_tx);
                p = ap(p, " y=");         p = ap_dec(p, g_ty);
                p = ap(p, " act=");       p = ap_dec(p, (int)g_w7);
                *p = 0;
            }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_FG);
            y += 18;
        } else if (g_key >= 0) {
            /* 按键：扫描码（十进制/十六进制）+ 键名反查 */
            {
                char *p = ap(buf, "scan = "); p = ap_dec(p, g_key);
                p = ap(p, "  (0x");           p = ap_hex2(p, (unsigned)g_key);
                p = ap(p, ")");               *p = 0;
            }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_FG);
            y += 18;
            { char *p = ap(buf, "name = "); p = ap(p, key_name(g_key)); *p = 0; }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1,
                              key_name(g_key)[0] == '?' ? C_WARN : C_ACC);
            y += 18;
        } else {
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y,
                              "press any key / touch screen", 1, 1, C_DIM);
            y += 18;
        }

        /* 原始字段（对照 hook_abi.md） */
        {
            { char *p = ap(buf, "ty4="); p = ap_hex8(p, g_ty4); *p = 0; }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_DIM);
            y += 14;
            { char *p = ap(buf, "w7 ="); p = ap_hex8(p, g_w7); *p = 0; }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_DIM);
            y += 14;
            { char *p = ap(buf, "w8 ="); p = ap_hex8(p, g_w8); *p = 0; }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_DIM);
            y += 14;
            { char *p = ap(buf, "w9 ="); p = ap_hex8(p, g_w9); *p = 0; }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_DIM);
            y += 16;
        }

        /* 最近 8 个扫描码 + 事件计数 */
        {
            int i, x = 6;
            unifont_draw_text(framebuf, LCD_W, LCD_H, x, y, "last:", 1, 1, C_DIM);
            x += 6 * 6;
            for (i = 0; i < 8; ++i) {
                if (g_last[i] < 0) break;
                { char *p = ap_hex2(buf, (unsigned)g_last[i]); *p++ = ' '; *p = 0; }
                unifont_draw_text(framebuf, LCD_W, LCD_H, x, y, buf, 1, 1, C_FG);
                x += 3 * 6;
            }
            y += 16;
            { char *p = ap(buf, "events="); p = ap_dec(p, (int)g_n); *p = 0; }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_DIM);
        }

        unifont_draw_text(framebuf, LCD_W, LCD_H, 6, LCD_H - 12,
                          "ESC / ON to exit", 1, 1, C_DIM);

        app_blit_fb(lcd, framebuf, LCD_W * LCD_H);
        prime_sys_sleep(30);
    }

    prime_hook_remove();
    return 0;
}
