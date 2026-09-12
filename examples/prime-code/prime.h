/*
 * prime.h -- user-facing header for TCC-compiled programs on the HP Prime.
 *
 * `#include "prime.h"` at the top of your C source.  The implementations
 * live in hp_rt.c (compiled on the calculator together with your code)
 * and rt_svc.o / rt_aeabi.o (prebuilt, linked by TCC).
 */
#ifndef PRIME_H
#define PRIME_H

/* size_t / NULL：跟随宿主编译器（__SIZE_TYPE__），使同一份用户代码在计算器
 * 上的 TCC 与 PC 上的 gcc 下都得到正确的 size_t 宽度（ARM 为 32 位、PC 为 64 位）。
 * AI 辅助修订：DeepSeek V4.1 Flash（未人工审查）—— 这是为“统一 TCC 与 PC gcc 的 API/ABI”
 * 所做的平台适配（非修 bug）；ARM 侧 __SIZE_TYPE__ 即 unsigned int，目标文件
 * 字节不变（已验证）。详见 ../API_ABI_CONTRACT.md。 */
#ifndef PRIME_SIZE_T_DEFINED
#define PRIME_SIZE_T_DEFINED
#define SIZE_T_DEFINED
#if defined(__SIZE_TYPE__)
typedef __SIZE_TYPE__ size_t;
#else
typedef unsigned int size_t;
#endif
#endif
#define NULL ((void *)0)

/* console output -> PRIMELOG ring (main.py prints it).
 *
 * printf / sprintf are VARIADIC at the source level and accept 1..8
 * values after the format string:  printf("x=%d y=%x\n", x, y);
 *
 * TCC's ARM backend has no AAPCS variadic-call support (no va_start
 * codegen: anonymous args would land in r0-r3 while the GCC-built runtime
 * reads them off the stack), so "varargs" are implemented with C99
 * variadic macros that dispatch on the argument count onto fixed-arity
 * engine functions (hp_printf8/hp_sprintf8, up to 8 values).
 *
 * Conversions: %d %i %u %x %X %o %c %s %p, with - 0 + space flags and a
 * width; no %f (format doubles with ftoa/dtoa from hp_string.h instead). */
int  hp_printf8(const char *fmt, unsigned long a0, unsigned long a1,
                unsigned long a2, unsigned long a3, unsigned long a4,
                unsigned long a5, unsigned long a6, unsigned long a7);
int  hp_sprintf8(char *buf, const char *fmt, unsigned long a0,
                 unsigned long a1, unsigned long a2, unsigned long a3,
                 unsigned long a4, unsigned long a5, unsigned long a6,
                 unsigned long a7);
int  puts(const char *s);
void prints(const char *s);
void printd(long v);
void printx(unsigned long v);

/* ---- variadic dispatch: count the arguments, pick the arity -----------
 * printf(fmt, v1..v8)   -> 1..9  total args
 * sprintf(buf, fmt, v1..v8) -> 2..10 total args */
#define PP_CAT(a, b)  PP_CAT_(a, b)
#define PP_CAT_(a, b) a ## b
#define PP_NARG(...)  PP_NARG_(__VA_ARGS__, PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, N, ...) N
#define PP_RSEQ_N() 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

#define printf(...)  PP_CAT(hp_printf_,  PP_NARG(__VA_ARGS__))(__VA_ARGS__)
#define sprintf(...) PP_CAT(hp_sprintf_, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define hp_printf_1(f) hp_printf8((f), 0, 0, 0, 0, 0, 0, 0, 0)
#define hp_printf_2(f, a) hp_printf8((f), (unsigned long)(a), 0, 0, 0, 0, 0, 0, 0)
#define hp_printf_3(f, a, b) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), 0, 0, 0, 0, 0, 0)
#define hp_printf_4(f, a, b, c) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), 0, 0, 0, 0, 0)
#define hp_printf_5(f, a, b, c, d) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), (unsigned long)(d), 0, 0, 0, 0)
#define hp_printf_6(f, a, b, c, d, e) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), (unsigned long)(d), (unsigned long)(e), 0, 0, 0)
#define hp_printf_7(f, a, b, c, d, e, g) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), (unsigned long)(d), (unsigned long)(e), (unsigned long)(g), 0, 0)
#define hp_printf_8(f, a, b, c, d, e, g, h) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), (unsigned long)(d), (unsigned long)(e), (unsigned long)(g), (unsigned long)(h), 0)
#define hp_printf_9(f, a, b, c, d, e, g, h, i) hp_printf8((f), (unsigned long)(a), (unsigned long)(b), (unsigned long)(c), (unsigned long)(d), (unsigned long)(e), (unsigned long)(g), (unsigned long)(h), (unsigned long)(i))

#define hp_sprintf_2(b, f) hp_sprintf8((b), (f), 0, 0, 0, 0, 0, 0, 0, 0)
#define hp_sprintf_3(b, f, v1) hp_sprintf8((b), (f), (unsigned long)(v1), 0, 0, 0, 0, 0, 0, 0)
#define hp_sprintf_4(b, f, v1, v2) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), 0, 0, 0, 0, 0, 0)
#define hp_sprintf_5(b, f, v1, v2, v3) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), (unsigned long)(v3), 0, 0, 0, 0, 0)
#define hp_sprintf_6(b, f, v1, v2, v3, v4) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), (unsigned long)(v3), (unsigned long)(v4), 0, 0, 0, 0)
#define hp_sprintf_7(b, f, v1, v2, v3, v4, v5) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), (unsigned long)(v3), (unsigned long)(v4), (unsigned long)(v5), 0, 0, 0)
#define hp_sprintf_8(b, f, v1, v2, v3, v4, v5, v6) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), (unsigned long)(v3), (unsigned long)(v4), (unsigned long)(v5), (unsigned long)(v6), 0, 0)
#define hp_sprintf_9(b, f, v1, v2, v3, v4, v5, v6, v7) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), (unsigned long)(v3), (unsigned long)(v4), (unsigned long)(v5), (unsigned long)(v6), (unsigned long)(v7), 0)
#define hp_sprintf_10(b, f, v1, v2, v3, v4, v5, v6, v7, v8) hp_sprintf8((b), (f), (unsigned long)(v1), (unsigned long)(v2), (unsigned long)(v3), (unsigned long)(v4), (unsigned long)(v5), (unsigned long)(v6), (unsigned long)(v7), (unsigned long)(v8))

/* firmware syscalls (see rt_svc.o); num is one of:
 *   0x10037 malloc   0x10038 calloc   0x10039 realloc   0x1003a free
 *   0x1026f fopen    0x100ca fclose   0x100cf fseek    0x100d0 ftell
 *   0x100d4 fread    0x100d7 fwrite   0x100cb filesize
 *   0x10008 os_sleep 0x1008d get_lcd
 * Convention: push{r0}; push{lr}; svc N; result in r0.
 * NOTE: 0x100a5 (claimed get_time) is NOT usable -- it reboots the G1;
 * there is no time source on the device, so time functions were dropped.
 */
void  __sleep(unsigned ms);

/* memory */
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void  free(void *p);

/* strings / memory */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

/* sorting / misc (hp_rt.c) */
void   qsort(void *base, size_t nmemb, size_t size,
             int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t nmemb,
               size_t size, int (*cmp)(const void *, const void *));
int    abs(int v);
long   labs(long v);

#endif /* PRIME_H */
