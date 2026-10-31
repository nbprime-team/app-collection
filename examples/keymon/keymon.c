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


/* hp_input.h 的键名表（现场反查用；表不全时显示 "?"） */
struct keyname { int code; const char *name; };
static const struct keyname g_keys[] = {
    { 0x83, "ON" },        { 0x50, "EEX" },      { 0x30, "0" },
    { 0x54, "/" },         { 0x58, "*" },        { 0xB7, "-" },
    { 0xB9, "+" },         { 0x20, "SPACE" },    { 0x02, "LEFT" },
    { 0x04, "RIGHT" },     { 0x03, "UP" },       { 0x05, "DOWN" },
    { 0x0C, "BACKSPACE" }, { 0x0D, "ENTER" },    { 0x01, "ESC" },
    { 0xB6, "ALPHA" },     { 0x8B, "SHIFT" },    { 0x44, "xt-theta-n" },
    { 0x43, "TEMPLATES" }, { 0x45, "ab/c" },     { 0x41, "VARS" },
    { 0x42, "TOOLBOX" },   { 0x46, "x^y" },      { 0x47, "SIN" },
    { 0x48, "COS" },       { 0x49, "TAN" },      { 0x4A, "LN" },
    { 0x4B, "LOG" },       { 0x4C, "x^2" },      { 0x4D, "+/-" },
    { 0x4E, "()" },        { 0x4F, "," },        { 0x51, "7" },
    { 0x52, "8" },         { 0x53, "9" },        { 0x55, "4" },
    { 0x56, "5" },         { 0x57, "6" },        { 0x59, "1" },
    { 0x5A, "2" },         { 0x33, "3" },        { 0xB8, "." },
    { 0x91, "SYMB" },      { 0xB2, "PLOT" },     { 0xB3, "NUM" },
    { 0xB4, "VIEW" },      { 0xB5, "CAS" },      { 0x93, "MENU" },
    { 0x95, "HELP" },      { 0xB1, "APPS" },
};

/* 同一张映射表（来自真机实测 keymap.md）：
 *   正常层 / SHIFT 层 / ALPHA 层各一个字符，0 表示该层没有字符。 */
struct kmap_entry { unsigned scan; char norm; char shf; char alp; };
static const struct kmap_entry k_map[] = {
    { 0x59, '1', '1', 'Y' }, { 0x5A, '2', '2', 'Z' }, { 0x33, '3', '3', '#' },
    { 0x55, '4', '4', 'U' }, { 0x56, '5', '5', 'V' }, { 0x57, '6', '6', 'W' },
    { 0x51, '7', '7', 'Q' }, { 0x52, '8', '8', 'R' }, { 0x53, '9', '9', 'S' },
    { 0x30, '0', '"', '"' }, { 0xB8, '.', '=', 0 },   { 0x4F, ',', ',', 'O' },
    { 0x54, '/', '/', 'T' }, { 0x58, '*', '*', 'X' }, { 0xB7, '-', '-', ':' },
    { 0xB9, '+', '+', ';' }, { 0x20, ' ', '_', '_' },
    { 0x41, 0, 0, 'A' }, { 0x42, 0, 0, 'B' }, { 0x43, 0, 0, 'C' },
    { 0x44, 0, 0, 'D' }, { 0x45, 0, 0, 'E' }, { 0x46, 0, 0, 'F' },
    { 0x47, 0, 0, 'G' }, { 0x48, 0, 0, 'H' }, { 0x49, 0, 0, 'I' },
    { 0x4A, 0, 0, 'J' }, { 0x4B, 0, 0, 'K' }, { 0x4C, 0, 0, 'L' },
    { 0x4D, 0, 0, 'M' }, { 0x4E, 0, 0, 'N' }, { 0x50, 0, 0, 'P' },
};

static const struct kmap_entry *km(unsigned scan)
{
    unsigned i;
    for (i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++)
        if (k_map[i].scan == scan)
            return &k_map[i];
    return 0;
}

static char *ap_char(char *p, char c)
{
    *p++ = c ? c : '-';
    return p;
}


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
static volatile int      g_alpha, g_shift;

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
            if (scan == HP_KEY_ALPHA) g_alpha = (g_alpha + 1) % 3;
            if (scan == HP_KEY_SHIFT) g_shift = !g_shift;
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
            {
                const struct kmap_entry *e = km((unsigned)g_key);
                char *p = ap(buf, "n=");
                p = ap_char(p, e ? e->norm : '-');
                p = ap(p, " s="); p = ap_char(p, e ? e->shf : '-');
                p = ap(p, " a="); p = ap_char(p, e ? e->alp : '-');
                *p = 0;
            }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_FG);
            y += 18;
            {
                char *p = ap(buf, "alpha="); p = ap_dec(p, g_alpha);
                p = ap(p, " shift=");        p = ap_dec(p, g_shift);
                *p = 0;
            }
            unifont_draw_text(framebuf, LCD_W, LCD_H, 6, y, buf, 1, 1, C_FG);
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
