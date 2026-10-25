/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * loader.c —— 终端自带的 ELF 装载器（M2：包依赖 + 跨包符号解析）。
 *
 * 1. 按 pkg.cfg 的 requires 先装载依赖库，登记其导出符号；
 * 2. 装载主 ELF；
 * 3. 应用 R_ARM_RELATIVE；对 JUMP_SLOT / GLOB_DAT / ABS32 / REL32
 *    按符号名从已加载库的导出表解析并回填；
 * 4. 刷新 cache，调用 entry(api, argc, argv)。
 */
#include <stdint.h>
#include "loader.h"
#include "fs.h"
#include "pkg.h"

extern unsigned fw_malloc(unsigned n);
extern void     fw_free(unsigned p);

#define SYM_MAX 512

struct syment {
    char name[28];
    unsigned addr;
};

static struct syment syms[SYM_MAX];
static int nsyms;
static unsigned loaded[8];
static int n_loaded;

/* 旧 ABI 程序（TCC/Lua）的 PRIMELOG 环：魔数 +0、count +8、data +12、环 32KB */
#define LOG_RING 32768u
static unsigned g_ring_pl;
static unsigned g_ring_lua;
static char g_log[1536];
static int g_legacy_image;
static char g_missing[40];      /* 映像里带 RT_RET: => 旧 hp_config ABI */

static unsigned copy_ring(unsigned ring, char *dst, unsigned cap)
{
    unsigned head, n, start, i, o = 0;
    if (!ring || !cap)
        return 0;
    head = *(volatile unsigned *)(ring + 8);
    n = head < LOG_RING ? head : LOG_RING;
    start = (head - n) & (LOG_RING - 1);
    for (i = 0; i < n && o < cap - 1; i++) {
        char c = *(volatile char *)(ring + 12 + ((start + i) & (LOG_RING - 1)));
        if (c == '\r')
            c = '\n';
        else if ((unsigned char)c < 0x20 && c != '\n')
            c = ' ';               /* tab 等控制字符会让字模出砖块 */
        dst[o++] = c;
    }
    dst[o] = 0;
    return o;
}

static void capture_log(void)
{
    unsigned o = 0;
    g_log[0] = 0;
    o = copy_ring(g_ring_pl, g_log, sizeof(g_log));
    if (o + 1 < sizeof(g_log))
        copy_ring(g_ring_lua, g_log + o, sizeof(g_log) - o);
}

const char *sh_last_log(void)
{
    return g_log;
}

const char *sh_last_missing(void)
{
    return g_missing;
}

static void mem_copy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    unsigned i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

static void mem_zero(void *dst, unsigned n)
{
    unsigned char *d = dst;
    unsigned i;
    for (i = 0; i < n; i++)
        d[i] = 0;
}

static int mem_eq(const unsigned char *a, const unsigned char *b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static uint32_t rd32(const unsigned char *p, unsigned off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static uint16_t rd16(const unsigned char *p, unsigned off)
{
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}

static void cache_flush(void)
{
    unsigned way, set;
    for (way = 0; way < 4; way++) {
        for (set = 0; set < 256; set++) {
            unsigned op = (way << 30) | (set << 5);
            __asm__ volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"(op) : "memory");
        }
    }
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
}

/* "demo_,foo_" 里是否有前缀匹配 name */
static int prefix_match(const char *list, const char *name)
{
    int i = 0;
    if (!list || !list[0])
        return 1;
    while (list[i]) {
        char pre[20];
        int n = 0, k = 0;
        while (list[i] && list[i] != ',') {
            if (n < 19)
                pre[n++] = list[i];
            i++;
        }
        pre[n] = 0;
        while (pre[k] && name[k] == pre[k])
            k++;
        if (pre[k] == 0)
            return 1;
        if (list[i] == ',')
            i++;
    }
    return 0;
}

static void sym_add(const char *name, unsigned addr)
{
    int i;
    if (!name || !name[0])
        return;
    for (i = 0; i < nsyms; i++)
        if (syms[i].name[0]) {
            int k = 0;
            while (syms[i].name[k] && syms[i].name[k] == name[k])
                k++;
            if (syms[i].name[k] == 0 && name[k] == 0)
                return;                 /* 已存在：先加载者优先 */
        }
    if (nsyms >= SYM_MAX)
        return;
    {
        int k = 0;
        while (name[k] && k < 27) {
            syms[nsyms].name[k] = name[k];
            k++;
        }
        syms[nsyms].name[k] = 0;
    }
    syms[nsyms].addr = addr;
    nsyms++;
}

static unsigned sym_find(const char *name)
{
    int i;
    for (i = 0; i < nsyms; i++) {
        int k = 0;
        while (syms[i].name[k] && syms[i].name[k] == name[k])
            k++;
        if (syms[i].name[k] == 0 && name[k] == 0)
            return syms[i].addr;
    }
    return 0;
}

struct dyninfo {
    unsigned symtab, strtab, syment, hash, rel, relsz, jmprel, pltrelsz;
    unsigned init, init_array, init_arraysz;
};

/* 真正的动态段解析（base 为映像基址，dyn_vaddr 为 PT_DYNAMIC 的 vaddr） */
static void parse_dyn(const unsigned char *img, unsigned dyn_vaddr,
                      struct dyninfo *d)
{
    const unsigned char *p = img + dyn_vaddr;
    unsigned t, v;
    mem_zero(d, sizeof(*d));
    d->syment = 16;
    do {
        t = rd32(p, 0);
        v = rd32(p, 4);
        switch (t) {
        case 4:  d->hash = v; break;
        case 5:  d->strtab = v; break;
        case 6:  d->symtab = v; break;
        case 11: d->syment = v; break;
        case 17: d->rel = v; break;
        case 18: d->relsz = v; break;
        case 23: d->jmprel = v; break;
        case 2:  d->pltrelsz = v; break;
        case 12: d->init = v; break;
        case 25: d->init_array = v; break;
        case 27: d->init_arraysz = v; break;
        default: break;
        }
        p += 8;
    } while (t != 0);
}

/* .dynsym 为空时（Lua 这类 -shared 但没导出动态符号的库），
 * 回退到 .symtab：地址同样是 vaddr，加映像基址即可。 */
static void add_exports_symtab(const unsigned char *file, unsigned flen,
                               const unsigned char *img, const char *prefixes)
{
    unsigned shoff, shent, shnum, i;

    if (flen < 52)
        return;
    shoff = rd32(file, 0x20);
    shent = rd16(file, 0x2E);
    shnum = rd16(file, 0x30);
    if (!shoff || !shent || !shnum || shoff + shnum * shent > flen)
        return;

    for (i = 0; i < shnum; i++) {
        const unsigned char *sh = file + shoff + i * shent;
        unsigned type = rd32(sh, 4);
        unsigned off, size, link, entsize;
        unsigned str_off, j, count;
        if (type != 2)                 /* SHT_SYMTAB */
            continue;
        off = rd32(sh, 16);
        size = rd32(sh, 20);
        link = rd32(sh, 24);
        entsize = rd32(sh, 36);
        if (!entsize)
            entsize = 16;
        if (off + size > flen || link >= shnum)
            continue;
        str_off = rd32(file + shoff + link * shent, 16);
        count = size / entsize;
        for (j = 1; j < count; j++) {
            const unsigned char *sym = file + off + j * entsize;
            unsigned name_off = rd32(sym, 0);
            unsigned value = rd32(sym, 4);
            unsigned info = sym[12];
            unsigned shndx = rd16(sym, 14);
            const char *nm;
            if (!name_off || !shndx)
                continue;
            if ((info >> 4) != 1 && (info >> 4) != 2)
                continue;
            nm = (const char *)(file + str_off + name_off);
            if (!prefix_match(prefixes, nm))
                continue;
            sym_add(nm, (unsigned)img + value);
        }
    }
}

static void add_exports(const unsigned char *img, unsigned dyn_vaddr,
                        const char *prefixes)
{
    struct dyninfo d;
    unsigned n = 256, i;
    parse_dyn(img, dyn_vaddr, &d);
    if (!d.symtab || !d.strtab)
        return;
    if (d.hash) {
        n = rd32(img, d.hash + 4);      /* DT_HASH: nchain */
        if (!n || n > 4096)
            n = 256;
    }
    for (i = 1; i < n; i++) {
        const unsigned char *s = img + d.symtab + i * d.syment;
        unsigned name_off = rd32(s, 0);
        unsigned value = rd32(s, 4);
        unsigned info = s[12];
        unsigned shndx = rd16(s, 14);
        const char *nm;
        if (!name_off || !shndx)
            continue;
        if ((info >> 4) != 1 && (info >> 4) != 2)
            continue;
        nm = (const char *)(img + d.strtab + name_off);
        if (!prefix_match(prefixes, nm))
            continue;
        sym_add(nm, (unsigned)img + value);
    }
}

static int apply_relocs(const unsigned char *img, unsigned dyn_vaddr)
{
    struct dyninfo d;
    parse_dyn(img, dyn_vaddr, &d);

    {
        int t;
        for (t = 0; t < 2; t++) {
            unsigned off = t ? d.jmprel : d.rel;
            unsigned sz = t ? d.pltrelsz : d.relsz;
            unsigned k, cnt;
            if (!off || !sz)
                continue;
            cnt = sz / 8;
            for (k = 0; k < cnt; k++) {
                const unsigned char *r = img + off + k * 8;
                unsigned r_off = rd32(r, 0);
                unsigned info = rd32(r, 4);
                unsigned type = info & 0xFF;
                unsigned idx = info >> 8;
                uint32_t *slot = (uint32_t *)((unsigned char *)img + r_off);
                if (type == 23) {
                    *slot += (uint32_t)img;
                    continue;
                }
                if (type != 2 && type != 3 && type != 21 && type != 22)
                    continue;
                if (!idx || !d.symtab)
                    return -4;
                {
                    const unsigned char *s =
                        img + d.symtab + idx * d.syment;
                    unsigned name_off = rd32(s, 0);
                    unsigned shndx = rd16(s, 14);
                    unsigned addr;
                    if (shndx) {
                        /* 符号定义在本映像内：等价于 patch_shared_relocs 折入
                         * st_value，再加映像基址 */
                        addr = (unsigned)img + rd32(s, 4);
                    } else {
                        if (!d.strtab || !name_off)
                            return -4;
                        const char *nm = (const char *)(img + d.strtab + name_off);
                        int z = 0;
                        addr = sym_find(nm);
                        if (!addr) {
                            while (nm[z] && z < 39) {
                                g_missing[z] = nm[z];
                                z++;
                            }
                            g_missing[z] = 0;
                            return -4;
                        }
                    }
                    if (type == 21 || type == 22)
                        *slot = addr;
                    else if (type == 2)
                        *slot = addr + *slot;
                    else
                        *slot = addr + *slot - (uint32_t)slot;
                }
            }
        }
    }
    return 0;
}

/* 装载一个 ELF；成功返回 0，base_out/entry_out 给出基址与入口绝对地址 */
static int load_one(const char *path, const char *prefixes,
                    unsigned *base_out, unsigned *entry_out)
{
    unsigned char *file = 0, *img;
    unsigned flen = 0, raw = 0, max_end = 0, dyn_vaddr = 0, i;
    unsigned phoff, phentsize, phnum;

    if (fs_read(path, &file, &flen))
        return -1;
    if (flen < 52 || !mem_eq(file, (const unsigned char *)"\x7f" "ELF", 4) ||
        file[4] != 1 || file[5] != 1 || rd16(file, 18) != 40) {
        fs_free(file);
        return -2;
    }

    phoff = rd32(file, 28);
    phentsize = rd16(file, 42);
    phnum = rd16(file, 44);
    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = file + phoff + i * phentsize;
        unsigned type = rd32(ph, 0);
        if (type == 1) {
            unsigned end = rd32(ph, 8) + rd32(ph, 20);
            if (end > max_end)
                max_end = end;
        } else if (type == 2) {
            dyn_vaddr = rd32(ph, 8);
        }
    }
    if (!max_end) {
        fs_free(file);
        return -2;
    }

    raw = fw_malloc(max_end + 32);
    if (!raw) {
        fs_free(file);
        return -3;
    }
    img = (unsigned char *)((raw + 7) & ~7u);
    mem_zero(img, max_end);

    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = file + phoff + i * phentsize;
        if (rd32(ph, 0) != 1)
            continue;
        mem_copy(img + rd32(ph, 8), file + rd32(ph, 4), rd32(ph, 16));
    }

    if (dyn_vaddr) {
        int rc = apply_relocs(img, dyn_vaddr);
        if (rc) {
            fw_free(raw);
            fs_free(file);
            return rc;
        }
        if (prefixes && prefixes[0]) {
            add_exports(img, dyn_vaddr, prefixes);
            add_exports_symtab(file, flen, img, prefixes);
        }
    }

    cache_flush();

    /* 库的构造器：DT_INIT / DT_INIT_ARRAY（本仓库自己的应用没有，库会有） */
    if (dyn_vaddr) {
        struct dyninfo d;
        parse_dyn(img, dyn_vaddr, &d);
        if (d.init)
            ((void (*)(void))((unsigned char *)img + d.init))();
        if (d.init_array) {
            unsigned n = d.init_arraysz / 4, k;
            for (k = 0; k < n; k++) {
                unsigned f = ((unsigned *)((unsigned char *)img + d.init_array))[k];
                if (f && f < (1u << 28))
                    ((void (*)(void))(img + f))();
            }
        }
    }

    if (n_loaded < 8)
        loaded[n_loaded++] = raw;

    /* 旧 ABI 判定：rt_core 的 hp_entry 会写 "RT_RET:" 到 PRIMELOG */
    g_legacy_image = 0;
    {
        unsigned k;
        for (k = 0; k + 7 < max_end; k++) {
            if (img[k] == 'R' && img[k + 1] == 'T' && img[k + 2] == '_' &&
                img[k + 3] == 'R' && img[k + 4] == 'E' && img[k + 5] == 'T' &&
                img[k + 6] == ':') {
                g_legacy_image = 1;
                break;
            }
        }
    }

    /* 找输出环：PRIMELOG（TCC）或 PLUARING（Lua），布局相同 */
    {
        unsigned k;
        for (k = 0; k + 12 < max_end; k++) {
            int primelog = (img[k] == 'P' && img[k + 1] == 'R' && img[k + 2] == 'I' &&
                            img[k + 3] == 'M' && img[k + 4] == 'E' && img[k + 5] == 'L' &&
                            img[k + 6] == 'O' && img[k + 7] == 'G');
            int luar = (img[k] == 'P' && img[k + 1] == 'L' && img[k + 2] == 'U' &&
                        img[k + 3] == 'A' && img[k + 4] == 'R' && img[k + 5] == 'I' &&
                        img[k + 6] == 'N' && img[k + 7] == 'G');
            if (primelog || luar) {
                unsigned cnt = rd32(img, k + 8);
                if (cnt <= LOG_RING) {
                    if (luar) {
                        if (!g_ring_lua)
                            g_ring_lua = (unsigned)(img + k);
                    } else if (!g_ring_pl) {
                        g_ring_pl = (unsigned)(img + k);
                    }
                }
            }
        }
    }

    *base_out = (unsigned)img;
    *entry_out = (unsigned)img + rd32(file, 24);
    fs_free(file);
    return 0;
}

static void release_all(void)
{
    int i;
    g_missing[0] = 0;
    for (i = 0; i < n_loaded; i++)
        fw_free(loaded[i]);
    n_loaded = 0;
    nsyms = 0;
}

typedef int (*entry_fn)(void *api, int argc, char **argv);

/* 旧 ABI：TCC / Lua 期望的 hp_config（magic 0xD00BC760） */
struct hp_config {
    unsigned magic;
    int argc;
    char **argv;
    unsigned envc;
    char **envs;
};

#define HP_CONFIG_MAGIC 0xD00BC760u
#define ARGV_MAX 16

static int invoke(unsigned entry, int legacy, struct sh_api *api,
                  int argc, char **argv)
{
    if (!legacy)
        return ((entry_fn)entry)(api, argc, argv);

    {
        unsigned alloc[2 + ARGV_MAX];
        int n_alloc = 0;
        struct hp_config cfg;
        int i, n = argc < ARGV_MAX ? argc : ARGV_MAX;

        for (i = 0; i < n; i++) {
            int len = 0, k;
            unsigned p;
            while (argv[i][len])
                len++;
            p = fw_malloc(len + 1);
            if (!p)
                break;
            for (k = 0; k <= len; k++)
                ((char *)p)[k] = argv[i][k];
            alloc[n_alloc++] = p;
        }
        {
            unsigned arr = 0;
            if (n > 0) {
                arr = fw_malloc(n * 4);
                if (!arr) {
                    while (n_alloc)
                        fw_free(alloc[--n_alloc]);
                    return -3;
                }
                alloc[n_alloc++] = arr;
                for (i = 0; i < n; i++)
                    ((unsigned *)arr)[i] = alloc[i];
            }
            cfg.magic = HP_CONFIG_MAGIC;
            cfg.argc = n;
            cfg.argv = (char **)arr;
            cfg.envc = 0;
            cfg.envs = 0;
        }
        {
            unsigned c = fw_malloc(sizeof(cfg));
            struct hp_config *pc;
            int rc;
            if (!c) {
                while (n_alloc)
                    fw_free(alloc[--n_alloc]);
                return -3;
            }
            pc = (struct hp_config *)c;
            pc->magic = cfg.magic;
            pc->argc = cfg.argc;
            pc->argv = cfg.argv;
            pc->envc = 0;
            pc->envs = 0;
            rc = ((int (*)(void *, void *))entry)(pc, 0);
            fw_free(c);
            while (n_alloc)
                fw_free(alloc[--n_alloc]);
            return rc;
        }
    }
}

int sh_run_elf_abi(const char *path, int legacy, struct sh_api *api,
                   int argc, char **argv)
{
    unsigned base = 0, entry = 0;
    int rc;

    release_all();
    rc = load_one(path, 0, &base, &entry);
    if (rc)
        return rc;
    rc = invoke(entry, legacy, api, argc, argv);
    capture_log();
    release_all();
    return rc;
}

int sh_run_elf(const char *path, struct sh_api *api, int argc, char **argv)
{
    /* 先按 shell ABI 装载；装入后按映像特征纠正 */
    unsigned base = 0, entry = 0;
    int rc;

    release_all();
    g_ring_pl = g_ring_lua = 0;
    rc = load_one(path, 0, &base, &entry);
    if (rc)
        return rc;
    rc = invoke(entry, g_legacy_image, api, argc, argv);
    capture_log();
    release_all();
    return rc;
}

static int run_pkg(struct pkg *p, struct sh_api *api, int argc, char **argv);

int sh_run_pkg(struct pkg *p, struct sh_api *api, int argc, char **argv)
{
    int rc;
    release_all();
    g_ring_pl = g_ring_lua = 0;
    rc = run_pkg(p, api, argc, argv);
    release_all();
    return rc;
}

static int load_requires(const struct pkg *p)
{
    int i = 0;
    while (p->requires[i]) {
        char name[24];
        int n = 0, k;
        while (p->requires[i] && p->requires[i] != ',') {
            if (n < 23)
                name[n++] = p->requires[i];
            i++;
        }
        name[n] = 0;
        for (k = 0; k < n; k++)
            if (name[k] == '-') {       /* name-ver 形式：截断版本 */
                name[k] = 0;
                break;
            }
        if (p->requires[i] == ',')
            i++;
        {
            struct pkg *dep = pkg_find(name);
            unsigned base = 0, entry = 0;
            const char *rel;
            int rc;
            if (!dep)
                return -5;
            rel = dep->kind[0] == 'l' ? dep->lib : dep->entry;
            if (!rel[0])
                rel = dep->entry;
            rc = load_one(pkg_file(dep, rel), dep->exports, &base, &entry);
            if (rc)
                return rc;
        }
    }
    return 0;
}

static int run_pkg(struct pkg *p, struct sh_api *api, int argc, char **argv)
{
    unsigned base = 0, entry = 0;
    const char *rel;
    int rc;

    rc = load_requires(p);
    if (rc)
        return rc;

    rel = (p->kind[0] == 'l') ? p->lib : p->entry;
    if (!rel[0])
        return -4;
    rc = load_one(pkg_file(p, rel), p->exports, &base, &entry);
    if (rc)
        return rc;
    rc = invoke(entry, p->abi[0] == 'h', api, argc, argv);
    capture_log();
    return rc;
}
