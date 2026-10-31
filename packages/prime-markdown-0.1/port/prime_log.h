/*
 * prime_log.h — 共享日志模块 (ELF → 引导器 LogReader 环形缓冲)
 * ===========================================================
 * LogStruct 布局与 generic_loader.py 的 LogReader 严格一致:
 *   { uint32_t head; char buffer[4096]; }
 * 任何模块 (demo / platform / render_api) 均可写日志, 引导器轮询回显。
 * 线程: 单线程使用 (演示/等待循环都在主线程)。
 */
#ifndef PRIME_LOG_H
#define PRIME_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRIME_LOG_SIZE 4096

typedef struct PrimeLogStruct {
    uint32_t head;
    char buffer[PRIME_LOG_SIZE];
} PrimeLogStruct;

/* 写入文本 / 十六进制 / 十进制 (自动续写环形缓冲) */
void prime_log_write(const char* text);
void prime_log_hex(uint32_t v);
/* 打印当前栈指针（判断"注入上下文"下剩余栈量） */
void prime_log_sp(void);
void prime_log_int(int v);


/* 返回日志结构地址 (app_entry 返回给引导器) */
PrimeLogStruct* prime_log_struct(void);

#ifdef __cplusplus
}
#endif

#endif /* PRIME_LOG_H */
