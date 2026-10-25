/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * sh.c —— 极简终端（M1）：文本界面 + 行编辑 + 少量内建命令 + 装载并运行子 ELF。
 *
 * 输入策略（有意为之）：
 *   - 数字与常用符号直接按物理键（扫描码取自 hp_input.h）；
 *   - 字母用屏幕网格（方向键移动、Enter 选字），因为 G1 的 alpha 层扫描码
 *     没有可靠公开表，猜错会打出乱码——宁可慢，不可错。
 *   - 另有一排常用命令模板，选中即插入。
 */
#include <stdint.h>
#include "app_common.h"
#include "prime_hook.h"
#include "unifont_font.h"
#include "sh_api.h"
#include "loader.h"
#include "fs.h"
#include "pkg.h"

#define LCD_W   320
#define LCD_H   240
#define ROW_H   16
#define COLS    32
#define OUT_ROWS 12
#define LINE_MAX 64
#define HIST_MAX 8
#define ARGV_MAX 16
#define IN_COLS 30   /* 输入行可见字符数（去掉 "> "） */
#define OUT_MAX  48

#define C_BG     0xff0b1020u
#define C_FG     0xffe6edf3u
#define C_DIM    0xff8b98a8u
#define C_STATUS 0xff1b2333u
#define C_ACCENT 0xff5fd0a0u
#define C_SEL    0xff2f81f7u
#define C_CURSOR 0xffe8b04cu

/* 固件 SVC 包装（fw_svc.S） */
extern unsigned fw_get_attr(unsigned path16);

static uint32_t fb[LCD_W * LCD_H] __attribute__((aligned(32)));
static char out[OUT_MAX][COLS + 1];
static int out_count, out_start;
static char input[LINE_MAX + 1];
static int input_len;
static char cwd[96] = "C:\\DATA";
static char hist[HIST_MAX][LINE_MAX + 1];
static int hist_count, hist_pos;
static volatile int g_key = -1;
static int quit, dirty = 1;
static struct sh_api g_api;

static void dump_log(void);
static void dump_missing(void);

/* ---------------- 渲染 ---------------- */
static void px(int x, int y, uint32_t c)
{
    if ((unsigned)x < LCD_W && (unsigned)y < LCD_H)
        fb[y * LCD_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            px(x + i, y + j, c);
}

static void glyph(int x, int y, char ch, uint32_t c)
{
    uint8_t w;
    const uint16_t *g = unifont_glyph((unsigned char)ch, &w);
    int r, k;
    for (r = 0; r < 16; r++)
        for (k = 0; k < w; k++)
            if (g[r] & (uint16_t)(1u << (w - 1 - k)))
                px(x + k, y + r, c);
}

static void text(int x, int y, const char *s, uint32_t c)
{
    while (*s) {
        if (*s != ' ')
            glyph(x, y, *s, c);
        x += 10;
        s++;
    }
}

static void lcd_blit(uint32_t *lcd)
{
    int i;
    for (i = 0; i < LCD_W * LCD_H; i++)
        lcd[i] = fb[i];
}

/* ---------------- 文本缓冲 ---------------- */
static void out_clear(void)
{
    out_count = 0;
    out_start = 0;
    dirty = 1;
}

static void out_line(const char *s)
{
    int slot, i;
    if (out_count < OUT_MAX) {
        slot = (out_start + out_count) % OUT_MAX;
        out_count++;
    } else {
        slot = out_start;
        out_start = (out_start + 1) % OUT_MAX;
    }
    for (i = 0; i < COLS && s[i]; i++)
        out[slot][i] = s[i];
    out[slot][i] = 0;
    dirty = 1;
}

static void out_str(const char *s)
{
    char line[COLS + 1];
    int n = 0;
    while (1) {
        if (*s == 0 || *s == '\n' || n >= COLS) {
            line[n] = 0;
            out_line(line);
            n = 0;
            if (*s == 0)
                return;
            if (*s == '\n') {
                s++;
                continue;
            }
        }
        line[n++] = *s++;
    }
}

static int puts_api(const char *s)
{
    out_str(s);
    return 0;
}

/* ---------------- 小工具 ---------------- */
static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void str_copy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

static unsigned short u16[256];
static unsigned short *to_u16(const char *s)
{
    int i;
    for (i = 0; i < 250 && s[i]; i++) {
        char ch = s[i];
        if (ch == '/')
            ch = '\\';
        u16[i] = (unsigned short)(unsigned char)ch;
    }
    u16[i] = 0;
    return u16;
}

static int is_dir(const char *path)
{
    unsigned a = fw_get_attr((unsigned)to_u16(path));
    if (a == 0xFFFFFFFFu)
        return 0;
    return (a & 0x10u) != 0;
}

static char path_buf[160];

/* 规范路径。唯一格式是反斜杠：
 *   ~ / ~\x  -> C:\DATA / C:\DATA\x
 *   C:\x     -> 原样
 *   相对      -> 当前目录下
 * 含 '/' 一律判为非法（不做兼容），返回 0。
 */
static const char *join_path(const char *name)
{
    int n;

    if (!name)
        name = "";
    if (name[0] == '/')
        return 0;
    if (name[0] == '~') {
        if (name[1] != 0 && name[1] != '\\')
            return 0;
        str_copy(path_buf, "C:\\DATA", sizeof(path_buf));
        name++;
        if (*name == '\\')
            name++;
    } else if (name[0] && name[1] == ':') {
        path_buf[0] = 0;
    } else {
        str_copy(path_buf, cwd, sizeof(path_buf));
    }

    if (*name) {
        if (name[0] == '\\' && path_buf[str_len(path_buf) - 1] == '\\')
            name++;
        if (path_buf[0] && path_buf[str_len(path_buf) - 1] != '\\') {
            n = str_len(path_buf);
            path_buf[n++] = '\\';
            path_buf[n] = 0;
        }
        str_copy(path_buf + str_len(path_buf), name,
                 (int)sizeof(path_buf) - str_len(path_buf));
    }
    for (n = 0; path_buf[n]; n++)
        if (path_buf[n] == '/')
            return 0;
    return path_buf;
}

/* ---------------- 内建命令 ---------------- */
static void cmd_help(void)
{
    out_str("builtins:");
    out_str("  help ls cd pwd clear");
    out_str("  pkg [list|info n]");
    out_str("  run <pkg|elf> runh <elf>");
    out_str("  cc <src.c> [-o out] echo exit");
    out_str("pkg cmds run by name");
    out_str("alpha: letters  (shift+/ = \\)");
}

static int ls_cb(const char *name, unsigned size, int is_dir, void *ud)
{
    char line[COLS + 1];
    int n = 0, k;
    for (k = 0; name[k] && n < 22; k++)
        line[n++] = name[k];
    if (is_dir) {
        line[n++] = '/';
    } else {
        unsigned v = size;
        char num[12];
        int d = 0;
        line[n++] = ' ';
        if (!v)
            num[d++] = '0';
        while (v) {
            num[d++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (d && n < COLS)
            line[n++] = num[--d];
    }
    line[n] = 0;
    out_line(line);
    (*(int *)ud)++;
    return 0;
}

static void cmd_ls(const char *arg)
{
    const char *p = join_path(arg && arg[0] ? arg : ".");
    int cnt = 0;
    if (!p) {
        out_str("bad path (use \\)");
        return;
    }
    if (fs_list(p, ls_cb, &cnt) <= 0)
        out_str("(empty)");
}

static void cmd_pkg(int argc, char **argv)
{
    int i;
    if (argc < 2 || str_eq(argv[1], "list")) {
        char line[COLS + 1];
        int n = 0, k;
        n = 0;
        line[n++] = 'p'; line[n++] = 'k'; line[n++] = 'g'; line[n++] = 's';
        line[n++] = ':'; line[n++] = ' ';
        if (pkg_count() >= 10)
            line[n++] = (char)('0' + pkg_count() / 10 % 10);
        line[n++] = (char)('0' + pkg_count() % 10);
        line[n] = 0;
        out_line(line);
        for (i = 0; i < pkg_count(); i++) {
            struct pkg *p = pkg_at(i);
            n = 0;
            for (k = 0; p->name[k] && n < 12; k++)
                line[n++] = p->name[k];
            line[n++] = ' ';
            for (k = 0; p->ver[k] && n < 18; k++)
                line[n++] = p->ver[k];
            line[n++] = ' ';
            for (k = 0; p->kind[k] && n < 24; k++)
                line[n++] = p->kind[k];
            if (p->provides[0]) {
                line[n++] = ' ';
                line[n++] = '=';
                line[n++] = '>';
                for (k = 0; p->provides[k] && n < COLS; k++)
                    line[n++] = p->provides[k];
            }
            line[n] = 0;
            out_line(line);
        }
    } else if (str_eq(argv[1], "scan")) {
        char line[COLS + 1];
        int n = 0;
        pkg_scan();
        line[n++] = 'p'; line[n++] = 'k'; line[n++] = 'g'; line[n++] = 's';
        line[n++] = ':'; line[n++] = ' ';
        line[n++] = (char)('0' + pkg_count() % 10);
        line[n] = 0;
        out_line(line);
    } else if (str_eq(argv[1], "info") && argc > 2) {
        struct pkg *p = pkg_find(argv[2]);
        if (!p) {
            out_str("no such pkg");
            return;
        }
        out_line(p->dir);
        out_line(p->entry[0] ? p->entry : p->lib);
        if (p->requires[0]) {
            out_str("requires:");
            out_line(p->requires);
        }
        if (p->exports[0]) {
            out_str("exports:");
            out_line(p->exports);
        }
    } else {
        out_str("usage: pkg list|info|scan");
    }
}

static void print_rc(int rc)
{
    char line[COLS + 1];
    int n = 0;
    if (rc >= 0) {
        line[n++] = 'e'; line[n++] = 'x'; line[n++] = 'i';
        line[n++] = 't'; line[n++] = ' ';
        if (rc >= 10)
            line[n++] = (char)('0' + (rc / 10) % 10);
        line[n++] = (char)('0' + rc % 10);
    } else {
        line[n++] = 'l'; line[n++] = 'o'; line[n++] = 'a'; line[n++] = 'd';
        line[n++] = ' '; line[n++] = 'f'; line[n++] = 'a'; line[n++] = 'i';
        line[n++] = 'l'; line[n++] = ' ';
        line[n++] = '-'; line[n++] = (char)('0' - rc);
    }
    line[n] = 0;
    out_line(line);
}

static void cmd_run(const char *arg)
{
    struct pkg *p;
    int rc;
    if (!arg || !arg[0]) {
        out_str("usage: run <pkg|elf>");
        return;
    }
    p = pkg_find(arg);
    if (p) {
        out_str("run pkg:");
        out_line(p->name);
        rc = sh_run_pkg(p, &g_api, 0, 0);
        dump_log();
    } else {
        const char *fp = join_path(arg);
        if (!fp) {
            out_str("bad path (use \\)");
            return;
        }
        out_str("run:");
        out_str(fp);
        rc = sh_run_elf(fp, &g_api, 0, 0);
    }
    print_rc(rc);
    dump_missing();
}
#if 0
{
}
#endif

static void cmd_mkdir(const char *arg)
{
    const char *p = join_path(arg);
    if (!arg || !p) { out_str("usage: mkdir <dir>"); return; }
    out_str(fs_mkdir(p) ? "ok" : "mkdir failed");
}

static void cmd_rm(const char *arg)
{
    const char *p = join_path(arg);
    if (!arg || !p) { out_str("usage: rm <path>"); return; }
    if (fs_is_dir(p))
        out_str(fs_rmdir(p) ? "ok" : "rmdir failed");
    else
        out_str(fs_remove(p) ? "ok" : "rm failed");
}

static void cmd_cp(const char *a, const char *b)
{
    static char src[160], dst[160];
    const char *p;
    if (!a || !b) { out_str("usage: cp <src> <dst>"); return; }
    p = join_path(a);
    if (!p) { out_str("bad path (use \\)"); return; }
    str_copy(src, p, sizeof(src));
    p = join_path(b);
    if (!p) { out_str("bad path (use \\)"); return; }
    str_copy(dst, p, sizeof(dst));
    out_str(fs_copy(src, dst) ? "ok" : "cp failed");
}

static void cmd_mv(const char *a, const char *b)
{
    static char src[160], dst[160];
    const char *p;
    if (!a || !b) { out_str("usage: mv <src> <dst>"); return; }
    p = join_path(a);
    if (!p) { out_str("bad path (use \\)"); return; }
    str_copy(src, p, sizeof(src));
    p = join_path(b);
    if (!p) { out_str("bad path (use \\)"); return; }
    str_copy(dst, p, sizeof(dst));
    out_str(fs_rename(src, dst) ? "ok" : "mv failed");
}

static void cmd_touch(const char *arg)
{
    const char *p = join_path(arg);
    if (!arg || !p) { out_str("usage: touch <file>"); return; }
    out_str(fs_touch(p) ? "ok" : "touch failed");
}

static void cmd_cat(const char *arg)
{
    unsigned char *buf = 0;
    unsigned len = 0, i;
    const char *p = join_path(arg);
    if (!arg || !p) { out_str("usage: cat <file>"); return; }
    if (fs_read(p, &buf, &len)) { out_str("cat: cannot read"); return; }
    {
        static char text[1024];
        unsigned n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
        for (i = 0; i < n; i++) {
            char c = (char)buf[i];
            if ((unsigned char)c < 0x20 && c != '\n')
                c = ' ';
            text[i] = c;
        }
        text[n] = 0;
        out_str(text);
    }
    fs_free(buf);
}

/* cc <src.c> [-o out.elf]：用 tcc 包编译；tcc 走 hp_config 旧 ABI *//* cc <src.c> [-o out.elf]：用 tcc 包编译；tcc 走 hp_config 旧 ABI */

static void dump_log(void)
{
    const char *lg = sh_last_log();
    if (lg && lg[0])
        out_str(lg);
}

static void dump_missing(void)
{
    const char *m = sh_last_missing();
    if (m && m[0]) {
        out_str("missing sym:");
        out_line(m);
    }
}

static void cmd_cc(int argc, char **argv)
{
    struct pkg *p = pkg_find("tcc");
    static char srcpath[160], outpath[160];
    char *targv[ARGV_MAX];
    int n = 0, rc, i;
    const char *src, *out;
    unsigned attr;

    if (!p) {
        out_str("cc: package 'tcc' not found");
        return;
    }
    if (argc < 2) {
        out_str("usage: cc <src.c> [-o out.elf]");
        return;
    }
    src = argv[1];
    out = 0;
    if (argc >= 4 && str_eq(argv[2], "-o"))
        out = argv[3];

    {
        const char *q = join_path(src);
        if (!q) { out_str("bad path (use \\)"); return; }
        str_copy(srcpath, q, sizeof(srcpath));
    }
    if (!out) {
        str_copy(outpath, srcpath, sizeof(outpath));
        for (i = str_len(outpath) - 1; i >= 0; i--) {
            if (outpath[i] == '.') {
                str_copy(outpath + i, ".elf", (int)sizeof(outpath) - i);
                break;
            }
        }
    } else {
        const char *q = join_path(out);
        if (!q) { out_str("bad path (use \\)"); return; }
        str_copy(outpath, q, sizeof(outpath));
    }

    /* 源文件找不到时，回退到 tcc 包里同名文件（demo.c 就住在那儿） */
    if (fw_get_attr((unsigned)to_u16(srcpath)) == 0xFFFFFFFFu) {
        int k = str_len(srcpath) - 1;
        static char alt[160];
        while (k > 0 && srcpath[k] != '\\')
            k--;
        str_copy(alt, p->dir, sizeof(alt));
        str_copy(alt + str_len(alt), "\\", (int)sizeof(alt) - str_len(alt));
        str_copy(alt + str_len(alt), srcpath + k + (srcpath[k] == '\\' ? 1 : 0),
                 (int)sizeof(alt) - str_len(alt));
        if (fw_get_attr((unsigned)to_u16(alt)) != 0xFFFFFFFFu)
            str_copy(srcpath, alt, sizeof(srcpath));
        else {
            out_str("cc: source not found:");
            out_line(srcpath);
            return;
        }
    }

    targv[n++] = "tcc";
    targv[n++] = "-nostdlib";
    targv[n++] = "-shared";
    targv[n++] = "-Wl,-e,hp_entry";
    targv[n++] = "-I";
    targv[n++] = p->dir;
    targv[n++] = "-o";
    targv[n++] = outpath;
    targv[n++] = srcpath;
    {
        static char objs[4][160];
        static const char *names[4] = {
            "\\rt_core.o", "\\rt_svc.o", "\\rt_aeabi.o", "\\rt_math.o"
        };
        for (i = 0; i < 4; i++) {
            str_copy(objs[i], p->dir, sizeof(objs[i]));
            str_copy(objs[i] + str_len(objs[i]), names[i],
                     (int)sizeof(objs[i]) - str_len(objs[i]));
            targv[n++] = objs[i];
        }
    }

    out_str("cc:");
    out_line(srcpath);
    rc = sh_run_pkg(p, &g_api, n, targv);
    dump_log();

    attr = fw_get_attr((unsigned)to_u16(outpath));
    if (attr != 0xFFFFFFFFu) {
        out_str("out:");
        out_line(outpath);
        out_str("compile ok, run with: runh <out.elf>");
    } else {
        char line[COLS + 1];
        int k = 0;
        const char *msg = "compile failed, tcc rc=";
        while (msg[k]) {
            line[k] = msg[k];
            k++;
        }
        if (rc >= 0)
            line[k++] = (char)('0' + rc % 10);
        line[k] = 0;
        out_line(line);
    }
}

/* runh <elf>：用旧 hp_config ABI 运行（TCC/Lua 产物） */
static void cmd_runh(const char *arg)
{
    if (!arg || !arg[0]) {
        out_str("usage: runh <elf>");
        return;
    }
    {
        const char *hp = join_path(arg);
        if (!hp) { out_str("bad path (use \\)"); return; }
        out_str("runh:");
        out_str(hp);
        print_rc(sh_run_elf_abi(hp, 1, &g_api, 0, 0));
        dump_missing();
        dump_log();
    }
}

static void exec_line(char *line)
{
    char *argv[6];
    int argc = 0, i = 0;
    char cmd[LINE_MAX + 1];

    while (line[i] == ' ')
        i++;
    if (!line[i])
        return;

    str_copy(cmd, line + i, sizeof(cmd));
    {
        char *p = cmd;
        while (*p && argc < 6) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            argv[argc++] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = 0;
        }
    }

    out_str("> ");
    out_line(cmd);

    if (str_eq(argv[0], "help")) {
        cmd_help();
    } else if (str_eq(argv[0], "ls")) {
        cmd_ls(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "mkdir")) {
        cmd_mkdir(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "rmdir") || str_eq(argv[0], "rm")) {
        cmd_rm(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "cp")) {
        cmd_cp(argc > 2 ? argv[1] : 0, argc > 2 ? argv[2] : 0);
    } else if (str_eq(argv[0], "mv")) {
        cmd_mv(argc > 2 ? argv[1] : 0, argc > 2 ? argv[2] : 0);
    } else if (str_eq(argv[0], "touch")) {
        cmd_touch(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "cat")) {
        cmd_cat(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "pwd")) {
        out_line(cwd);
    } else if (str_eq(argv[0], "clear")) {
        out_clear();
    } else if (str_eq(argv[0], "cd")) {
        const char *t = (argc > 1) ? argv[1] : "~";
        const char *p = join_path(t);
        if (!p) {
            out_str("bad path (use \\)");
            return;
        } else if (is_dir(p)) {
            str_copy(cwd, p, sizeof(cwd));
        } else {
            out_str("no such dir");
        }
    } else if (str_eq(argv[0], "run")) {
        cmd_run(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "runh")) {
        cmd_runh(argc > 1 ? argv[1] : 0);
    } else if (str_eq(argv[0], "cc")) {
        cmd_cc(argc, argv);
    } else if (str_eq(argv[0], "pkg")) {
        cmd_pkg(argc, argv);
    } else if (str_eq(argv[0], "echo")) {
        if (argc > 1)
            out_line(argv[1]);
    } else if (str_eq(argv[0], "exit") || str_eq(argv[0], "quit")) {
        quit = 1;
    } else {
        struct pkg *p = pkg_find_cmd(argv[0]);
        if (p) {
            print_rc(sh_run_pkg(p, &g_api, argc, argv));
            dump_missing();
            dump_log();
        } else {
            out_str("not found:");
            out_line(argv[0]);
        }
    }
}

/* ---------------- 输入 ----------------
 *
 * 键位表来自真机实测映射（keymap.md）：
 *   字母用 ALPHA 层（Vars=A … 2=Z），数字/符号用正常层，SHIFT 层给 = _ " ; 等。
 *   ALPHA 一次 = 下一个字母，连按两次 = 锁定；SHIFT 同理（一次性）。
 */
struct keymap {
    unsigned scan;
    char norm;
    char shf;
    char alp;
};

static const struct keymap kmap[] = {
    /* 数字行 */
    { 0x59, '1', '1', 'Y' }, { 0x5A, '2', '2', 'Z' }, { 0x33, '3', '3', '#' },
    { 0x55, '4', '4', 'U' }, { 0x56, '5', '5', 'V' }, { 0x57, '6', '6', 'W' },
    { 0x51, '7', '7', 'Q' }, { 0x52, '8', '8', 'R' }, { 0x53, '9', '9', 'S' },
    { 0x30, '0', '"', '"' },
    /* 符号行 */
    { 0xB8, '.', '=', 0   }, { 0x4F, ',', ',', 'O' }, { 0x54, '/', '\\', 'T' },
    { 0x58, '*', '*', 'X' }, { 0xB7, '-', '-', ':' }, { 0xB9, '+', '+', ';' },
    { 0x20, ' ', '_', '_' },
    /* 字母键（正常层是数学函数，终端不用） */
    { 0x41, 0, 0, 'A' }, { 0x42, 0, 0, 'B' }, { 0x43, 0, 0, 'C' },
    { 0x44, 0, 0, 'D' }, { 0x45, 0, 0, 'E' }, { 0x46, 0, 0, 'F' },
    { 0x47, 0, 0, 'G' }, { 0x48, 0, 0, 'H' }, { 0x49, 0, 0, 'I' },
    { 0x4A, 0, 0, 'J' }, { 0x4B, 0, 0, 'K' }, { 0x4C, 0, '~', 'L' },
    { 0x4D, 0, 0, 'M' }, { 0x4E, 0, 0, 'N' }, { 0x50, 0, 0, 'P' }
};

static int alpha_mode;     /* 0=off 1=one-shot 2=lock */
static int shift_mode;
static int cursor;

static void insert_ch(char c)
{
    int i;
    if (input_len >= LINE_MAX - 1)
        return;
    for (i = input_len; i > cursor; i--)
        input[i] = input[i - 1];
    input[cursor] = c;
    input_len++;
    input[input_len] = 0;
    cursor++;
    dirty = 1;
}

static void backspace_ch(void)
{
    int i;
    if (cursor <= 0)
        return;
    for (i = cursor - 1; i < input_len - 1; i++)
        input[i] = input[i + 1];
    input_len--;
    input[input_len] = 0;
    cursor--;
    dirty = 1;
}

static const struct keymap *lookup(unsigned scan)
{
    unsigned i;
    for (i = 0; i < sizeof(kmap) / sizeof(kmap[0]); i++)
        if (kmap[i].scan == scan)
            return &kmap[i];
    return 0;
}

static void input_accept(void)
{
    if (input_len == 0) {
        dirty = 1;
        return;
    }
    input[input_len] = 0;
    if (hist_count < HIST_MAX) {
        hist_count++;
    } else {
        int i;
        for (i = 1; i < HIST_MAX; i++)
            str_copy(hist[i - 1], hist[i], LINE_MAX + 1);
    }
    str_copy(hist[hist_count - 1], input, LINE_MAX + 1);
    hist_pos = hist_count;
    exec_line(input);
    input_len = 0;
    cursor = 0;
    input[0] = 0;
    dirty = 1;
}

static void handle_key(int k)
{
    unsigned scan = (unsigned)k;

    if (scan == 0xB6) {                 /* ALPHA */
        alpha_mode = (alpha_mode + 1) % 3;
        dirty = 1;
        return;
    }
    if (scan == 0x8B) {                 /* SHIFT */
        shift_mode = 1;
        return;
    }
    if (scan == 0x0D) {                 /* ENTER */
        input_accept();
        return;
    }
    if (scan == 0x0C) {                 /* BACKSPACE */
        backspace_ch();
        return;
    }
    if (scan == 0x01) {                 /* ESC：清行 */
        input_len = 0;
        cursor = 0;
        input[0] = 0;
        dirty = 1;
        return;
    }
    if (scan == 0x02) {                 /* LEFT */
        if (cursor > 0)
            cursor--;
        dirty = 1;
        return;
    }
    if (scan == 0x04) {                 /* RIGHT */
        if (cursor < input_len)
            cursor++;
        dirty = 1;
        return;
    }
    if (scan == 0x03) {                 /* UP：历史 */
        if (hist_count && hist_pos > 0) {
            hist_pos--;
            str_copy(input, hist[hist_pos], LINE_MAX + 1);
            input_len = str_len(input);
            cursor = input_len;
            dirty = 1;
        }
        return;
    }
    if (scan == 0x05) {                 /* DOWN */
        if (hist_pos + 1 < hist_count) {
            hist_pos++;
            str_copy(input, hist[hist_pos], LINE_MAX + 1);
            input_len = str_len(input);
        } else {
            input_len = 0;
            input[0] = 0;
        }
        cursor = input_len;
        dirty = 1;
        return;
    }

    {
        const struct keymap *m = lookup(scan);
        char c = 0;
        if (!m)
            return;
        if (alpha_mode && m->alp) {
            c = m->alp;
            if (shift_mode) {
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
            } else if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
        } else if (shift_mode && m->shf) {
            c = m->shf;
        } else if (!alpha_mode && m->norm) {
            c = m->norm;
        }
        if (c) {
            insert_ch(c);
            shift_mode = 0;
            if (alpha_mode == 1)
                alpha_mode = 0;
        }
    }
}

static void on_event(void *ev)
{
    uint8_t *p = (uint8_t *)ev;
    prime_sys_get_event(ev);
    if (app_rd32(p + 4) != 0x00100010u)
        return;
    if (app_rd16(p + 28) != 16)
        return;
    g_key = (int)(app_rd32(p + 32) >> 16);
}

/* ---------------- 绘制 ---------------- */
static void draw(void)
{
    int i, y;
    fill_rect(0, 0, LCD_W, LCD_H, C_BG);

    fill_rect(0, 0, LCD_W, ROW_H, C_STATUS);
    text(4, 1, "sh 0.2", C_ACCENT);
    text(70, 1, cwd, C_DIM);

    for (i = 0; i < OUT_ROWS; i++) {
        int idx = out_count - OUT_ROWS + i;
        y = ROW_H + i * ROW_H;
        if (idx < 0)
            continue;
        idx = (out_start + idx) % OUT_MAX;
        text(4, y, out[idx], C_FG);
    }


        y = 240 - ROW_H;
        fill_rect(0, y - 2, LCD_W, ROW_H + 2, C_STATUS);
        text(4, y, ">", C_ACCENT);
        {
            /* 输入超过可视宽度时，让窗口跟着光标走 */
            int view = 0;
            if (cursor >= IN_COLS)
                view = cursor - IN_COLS + 1;
            text(18, y, input + view, C_FG);
            fill_rect(18 + (cursor - view) * 10, y, 8, ROW_H, C_CURSOR);
        }
}

int main(void *config, void *reserved)
{
    uint32_t *lcd;
    (void)config;
    (void)reserved;

    app_elf_requirements();
    lcd = app_lcd_framebuffer();
    if (!lcd)
        return 1;

    g_api.magic = SH_API_MAGIC;
    g_api.abi = SH_API_ABI;
    g_api.size = sizeof(g_api);
    g_api.puts = puts_api;

    out_str("sh 0.2 ready");
    out_str("alpha=letters shift=upper");
    pkg_scan();
    {
        char line[COLS + 1];
        int n = 0;
        n = 0;
        line[n++] = 'p'; line[n++] = 'k'; line[n++] = 'g'; line[n++] = 's';
        line[n++] = ':'; line[n++] = ' ';
        if (pkg_count() >= 10)
            line[n++] = (char)('0' + pkg_count() / 10 % 10);
        line[n++] = (char)('0' + pkg_count() % 10);
        line[n] = 0;
        out_line(line);
    }

    if (!prime_hook_install(on_event))
        out_str("!! input hook failed");

#ifdef PRIME_HOST_TEST
    int host_frames = 0;
#endif
    while (!quit) {
        int k = g_key;
#ifdef PRIME_HOST_TEST
        if (++host_frames > 400)
            break;
#endif
        if (k >= 0) {
            g_key = -1;
            handle_key(k);
        }
        if (dirty) {
            draw();
            lcd_blit(lcd);
            dirty = 0;
        }
        prime_sys_sleep(20);
    }

    prime_hook_remove();
    return 0;
}
