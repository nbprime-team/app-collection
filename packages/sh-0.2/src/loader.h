/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查） */
#ifndef LOADER_H
#define LOADER_H

#include "sh_api.h"

struct pkg;

/* 返回值：>=0 为子程序退出码；<0 为装载失败（-1 文件，-2 格式，-3 内存，
 * -4 不支持的符号重定位） */
int sh_run_elf(const char *path, struct sh_api *api, int argc, char **argv);
int sh_run_elf_abi(const char *path, int legacy, struct sh_api *api,
                   int argc, char **argv);
int sh_run_pkg(struct pkg *p, struct sh_api *api, int argc, char **argv);
/* 最后一次旧 ABI 调用的 PRIMELOG 文本（无则空串） */
const char *sh_last_log(void);
const char *sh_last_missing(void);

#endif
