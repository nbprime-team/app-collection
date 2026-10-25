/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * pkg.c —— 包索引：扫描 pkg\ 下的 name-ver 目录，解析 pkg.cfg。
 *
 * 包目录：C:\DATA\pkg\（与终端 sh.hpappdir/ 分开存放）
 */
#include <stdint.h>
#include "pkg.h"
#include "fs.h"

static struct pkg pkgs[PKG_MAX];
static int n_pkgs;
static char pathbuf[160];

static const char *ROOTS[1] = {
    "C:\\DATA\\pkg"          /* 包与终端分开存放 */
};

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static void str_copy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

static void trim(char *s)
{
    int i = 0, j;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    if (i) {
        for (j = 0; s[i]; j++, i++)
            s[j] = s[i];
        s[j] = 0;
    }
    j = 0;
    while (s[j])
        j++;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' ||
                     s[j - 1] == '\r' || s[j - 1] == '\n'))
        s[--j] = 0;
}

static void set_field(char *dst, int cap, const char *val)
{
    trim((char *)val);
    str_copy(dst, val, cap);
}

/* "name-1.2" -> name / 1.2 */
static void split_nv(const char *dir, char *name, int ncap, char *ver, int vcap)
{
    int i, dash = -1;
    for (i = 0; dir[i]; i++)
        if (dir[i] == '-')
            dash = i;
    if (dash < 0) {
        str_copy(name, dir, ncap);
        ver[0] = 0;
        return;
    }
    {
        int k;
        for (k = 0; k < dash && k < ncap - 1; k++)
            name[k] = dir[k];
        name[k] = 0;
        str_copy(ver, dir + dash + 1, vcap);
    }
}

static int parse_cfg(struct pkg *p, const char *file)
{
    unsigned char *buf = 0;
    unsigned len = 0, i = 0;

    if (fs_read(file, &buf, &len))
        return -1;

    while (i < len) {
        char line[120];
        int n = 0;
        while (i < len && buf[i] != '\n' && n < 118)
            line[n++] = (char)buf[i++];
        if (i < len && buf[i] == '\n')
            i++;
        line[n] = 0;
        trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        {
            char *eq = line;
            while (*eq && *eq != '=')
                eq++;
            if (!*eq)
                continue;
            *eq++ = 0;
            trim(line);
            trim(eq);
            if (str_eq(line, "name"))
                set_field(p->name, sizeof(p->name), eq);
            else if (str_eq(line, "version"))
                set_field(p->ver, sizeof(p->ver), eq);
            else if (str_eq(line, "kind"))
                set_field(p->kind, sizeof(p->kind), eq);
            else if (str_eq(line, "abi"))
                set_field(p->abi, sizeof(p->abi), eq);
            else if (str_eq(line, "entry"))
                set_field(p->entry, sizeof(p->entry), eq);
            else if (str_eq(line, "lib"))
                set_field(p->lib, sizeof(p->lib), eq);
            else if (str_eq(line, "exports"))
                set_field(p->exports, sizeof(p->exports), eq);
            else if (str_eq(line, "provides"))
                set_field(p->provides, sizeof(p->provides), eq);
            else if (str_eq(line, "requires"))
                set_field(p->requires, sizeof(p->requires), eq);
        }
    }
    fs_free(buf);
    return 0;
}

struct scan_ctx {
    const char *root;
};

static int on_dir(const char *name, unsigned size, int is_dir, void *ud)
{
    struct scan_ctx *sc = (struct scan_ctx *)ud;
    struct pkg *p;
    char cfg[160];
    int i, n = 0;

    (void)size;
    if (!is_dir || n_pkgs >= PKG_MAX)
        return 0;

    p = &pkgs[n_pkgs];
    for (i = 0; i < (int)sizeof(*p); i++)
        ((char *)p)[i] = 0;

    str_copy(p->dir, sc->root, sizeof(p->dir));
    {
        int len = 0;
        while (p->dir[len])
            len++;
        if (len < (int)sizeof(p->dir) - 2) {
            p->dir[len++] = '\\';
            for (i = 0; name[i] && len < (int)sizeof(p->dir) - 1; i++)
                p->dir[len++] = name[i];
            p->dir[len] = 0;
        }
    }

    str_copy(cfg, p->dir, sizeof(cfg));
    {
        int len = 0;
        while (cfg[len])
            len++;
        str_copy(cfg + len, "\\pkg.cfg", (int)sizeof(cfg) - len);
    }

    if (parse_cfg(p, cfg))
        return 0;                       /* 没有清单：不是包 */

    if (!p->name[0])
        split_nv(name, p->name, sizeof(p->name), p->ver, sizeof(p->ver));
    if (!p->kind[0])
        str_copy(p->kind, "app", sizeof(p->kind));
    if (!p->abi[0])
        str_copy(p->abi, "shell", sizeof(p->abi));

    n_pkgs++;
    (void)n;
    return 0;
}

int pkg_scan(void)
{
    int r;
    n_pkgs = 0;
    for (r = 0; r < 1; r++) {
        struct scan_ctx sc;
        sc.root = ROOTS[r];
        fs_list(ROOTS[r], on_dir, &sc);
    }
    return n_pkgs;
}

int pkg_count(void)
{
    return n_pkgs;
}

struct pkg *pkg_at(int i)
{
    if (i < 0 || i >= n_pkgs)
        return 0;
    return &pkgs[i];
}

struct pkg *pkg_find(const char *name)
{
    int i;
    for (i = 0; i < n_pkgs; i++)
        if (str_eq(pkgs[i].name, name))
            return &pkgs[i];
    return 0;
}

struct pkg *pkg_find_cmd(const char *cmd)
{
    int i;
    for (i = 0; i < n_pkgs; i++)
        if (pkgs[i].provides[0] && str_eq(pkgs[i].provides, cmd))
            return &pkgs[i];
    return 0;
}

const char *pkg_file(const struct pkg *p, const char *rel)
{
    int len = 0, i;
    str_copy(pathbuf, p->dir, sizeof(pathbuf));
    while (pathbuf[len])
        len++;
    if (len < (int)sizeof(pathbuf) - 2) {
        pathbuf[len++] = '\\';
        for (i = 0; rel[i] && len < (int)sizeof(pathbuf) - 1; i++)
            pathbuf[len++] = rel[i];
        pathbuf[len] = 0;
    }
    return pathbuf;
}
