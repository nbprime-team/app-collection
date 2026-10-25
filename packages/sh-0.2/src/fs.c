/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * fs.c —— 固件文件系统的最小封装（读文件、判断目录、列目录）。
 */
#include <stdint.h>
#include "fs.h"

extern unsigned fw_malloc(unsigned n);
extern void     fw_free(unsigned p);
extern int      fw_fopen(unsigned path16, unsigned mode16);
extern int      fw_fclose(int fd);
extern unsigned fw_fread(unsigned buf, unsigned nmemb, unsigned size, int fd);
extern unsigned fw_filesize(int fd);
extern int      fw_find_first(unsigned path16, unsigned ctx, unsigned mask);
extern int      fw_find_next(unsigned ctx);
extern int      fw_find_close(unsigned ctx);
extern unsigned fw_get_attr(unsigned path16);
extern int      fw_mkdir(unsigned path16);
extern int      fw_rmdir(unsigned path16);
extern int      fw_remove(unsigned path16);
extern int      fw_rename(unsigned p1, unsigned p2);
extern int      fw_copy(unsigned p1, unsigned p2);

static unsigned short u16buf[256];
static unsigned short u16buf2[256];

static unsigned short *to_u16b(const char *s)
{
    int i;
    for (i = 0; i < 250 && s[i]; i++)
        u16buf2[i] = (unsigned short)(unsigned char)s[i];
    u16buf2[i] = 0;
    return u16buf2;
}

static unsigned short *to_u16(const char *s)
{
    int i;
    for (i = 0; i < 250 && s[i]; i++)
        u16buf[i] = (unsigned short)(unsigned char)s[i];
    u16buf[i] = 0;
    return u16buf;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

int fs_read(const char *path, unsigned char **out, unsigned *outlen)
{
    static unsigned short m16[4];
    int fd;
    unsigned size;
    unsigned char *buf;

    m16[0] = 'r';
    m16[1] = 'b';
    m16[2] = 0;

    fd = fw_fopen((unsigned)to_u16(path), (unsigned)m16);
    if (fd <= 0 || (unsigned)fd == 0xFFFFFFFFu)
        return -1;
    size = fw_filesize(fd);
    if (size > (2u << 20)) {
        fw_fclose(fd);
        return -1;
    }
    if (!size) {                     /* 空文件是合法的 */
        fw_fclose(fd);
        *out = 0;
        *outlen = 0;
        return 0;
    }
    buf = (unsigned char *)fw_malloc(size);
    if (!buf) {
        fw_fclose(fd);
        return -1;
    }
    if (fw_fread((unsigned)buf, 1, size, fd) != size) {
        fw_free((unsigned)buf);
        fw_fclose(fd);
        return -1;
    }
    fw_fclose(fd);
    *out = buf;
    *outlen = size;
    return 0;
}

void fs_free(void *p)
{
    if (p)
        fw_free((unsigned)p);
}

int fs_mkdir(const char *p) { return fw_mkdir((unsigned)to_u16(p)) == 0; }
int fs_rmdir(const char *p) { return fw_rmdir((unsigned)to_u16(p)) == 0; }
int fs_remove(const char *p) { return fw_remove((unsigned)to_u16(p)) == 0; }

int fs_rename(const char *a, const char *b)
{
    return fw_rename((unsigned)to_u16(a), (unsigned)to_u16b(b)) == 0;
}

int fs_copy(const char *a, const char *b)
{
    return fw_copy((unsigned)to_u16(a), (unsigned)to_u16b(b)) == 0;
}

int fs_touch(const char *path)
{
    static unsigned short m16[4];
    int fd;
    m16[0] = 'w'; m16[1] = 'b'; m16[2] = '+'; m16[3] = 0;
    fd = fw_fopen((unsigned)to_u16(path), (unsigned)m16);
    if (fd <= 0 || (unsigned)fd == 0xFFFFFFFFu)
        return 0;
    fw_fclose(fd);
    return 1;
}

int fs_is_dir(const char *path)
{
    unsigned a = fw_get_attr((unsigned)to_u16(path));
    if (a == 0xFFFFFFFFu)
        return 0;
    return (a & 0x10u) != 0;
}

int fs_list(const char *dir, int (*cb)(const char *, unsigned, int, void *),
            void *ud)
{
    char pattern[160];
    unsigned ctx;
    int n = 0, i;
    static char seen[40][40];
    int seen_n = 0;

    for (i = 0; dir[i] && i < 150; i++)
        pattern[i] = dir[i];
    pattern[i++] = '\\';
    pattern[i++] = '*';
    pattern[i] = 0;

    ctx = fw_malloc(256);
    if (!ctx)
        return -1;

    /* 文件与目录的属性掩码不同，试几种，按名字去重由调用者负责 */
    {
        static const unsigned masks[3] = { 0x37u, 0xFFu, 0x00u };
        int m;
        for (m = 0; m < 3; m++) {
            int ok = fw_find_first((unsigned)to_u16(pattern), ctx, masks[m]);
            if (ok != 0)
                continue;
            do {
                unsigned char *c = (unsigned char *)ctx;
                unsigned lfn = *(unsigned *)(c + 8);
                unsigned sfn = *(unsigned *)(c + 12);
                unsigned size = *(unsigned *)(c + 20);
                unsigned attr = c[37];
                const unsigned short *nm =
                    (const unsigned short *)(lfn ? lfn : sfn);
                char name[40];
                int k = 0;
                if (!nm)
                    continue;
                while (nm[k] && k < 39) {
                    name[k] = (char)(nm[k] < 128 ? nm[k] : '?');
                    k++;
                }
                name[k] = 0;
                if (!name[0] || str_eq(name, ".") || str_eq(name, ".."))
                    continue;
                {
                    int dup = 0, q;
                    for (q = 0; q < seen_n; q++)
                        if (str_eq(seen[q], name)) {
                            dup = 1;
                            break;
                        }
                    if (dup)
                        continue;
                    if (seen_n < 40) {
                        int z = 0;
                        while (name[z] && z < 39) {
                            seen[seen_n][z] = name[z];
                            z++;
                        }
                        seen[seen_n][z] = 0;
                        seen_n++;
                    }
                }
                n++;
                if (cb && cb(name, size, (attr & 0x10u) != 0, ud))
                    goto done;
            } while (fw_find_next(ctx) == 0);
        }
    }
done:
    fw_find_close(ctx);
    fw_free(ctx);
    return n;
}
