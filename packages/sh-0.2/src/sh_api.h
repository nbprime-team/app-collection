/* AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）
 *
 * sh_api.h —— 终端传给子程序的 ABI（v1）。
 * 子程序入口签名：int main(void *api, int argc, char **argv);
 */
#ifndef SH_API_H
#define SH_API_H

#define SH_API_MAGIC 0x31495053u   /* 'SPI1' */
#define SH_API_ABI   1u

struct sh_api {
    unsigned magic;
    unsigned abi;
    unsigned size;
    int (*puts)(const char *s);   /* 追加一行（自动换行） */
};

#endif
