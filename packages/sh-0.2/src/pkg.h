/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查） */
#ifndef PKG_H
#define PKG_H

#define PKG_MAX 8

struct pkg {
    char name[20];
    char ver[8];
    char kind[8];        /* app | tool | lib */
    char abi[12];        /* shell（默认）| hpconfig */
    char dir[120];
    char entry[96];      /* kind=app/tool */
    char lib[96];        /* kind=lib */
    char exports[20];    /* 符号前缀，逗号分隔 */
    char provides[20];   /* 命令名 */
    char requires[96];   /* 逗号分隔的 name-ver */
};

int  pkg_scan(void);
int  pkg_count(void);
struct pkg *pkg_at(int i);
struct pkg *pkg_find(const char *name);
struct pkg *pkg_find_cmd(const char *cmd);
const char *pkg_file(const struct pkg *p, const char *rel);

#endif
