/*
 * prime_runtime.cpp — HP Prime 移植的运行时支撑
 * =============================================
 * 目标 ELF 是 freestanding: 无 libc、无 OS。本文件提供:
 *   - operator new/delete → 固件后门 malloc/free (经 PrimeServices)
 *   - C 名字的 malloc/realloc/free/calloc (FreeType ftsystem.c 依赖)
 *   - 最小 libc 子集 (memcpy/memset/... 供 libgcc/libstdc++/FreeType)
 *   - abort/__assert_func 等失败路径 (死循环 halt)
 *
 * 关键约束:
 *   - 不链接 crt0/crtbegin: 程序没有 _start 与静态初始化器运行机制,
 *     因此本移植约定「禁止全局非平凡动态对象」, 一切对象在 app_entry
 *     之后的函数内创建。libstdc++ 的异常/分配机制不依赖静态 init。
 *   - 内存分配一律走服务表; 服务表未初始化时返回 NULL (而非崩溃),
 *     operator new 此时抛 std::bad_alloc 属预期行为。
 */
#include <cstddef>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "prime_svc.h"
#include "prime_sys.h"

extern "C" {

/* 服务表全局指针 —— 平凡初始化 (零), 无动态构造 */
static const PrimeServices* g_services = 0;

/* 由 app_entry 在进入业务逻辑前调用 */
void prime_set_services(const PrimeServices* svc) {
    g_services = svc;
}

const PrimeServices* prime_get_services(void) {
    return g_services;
}

/* ---------- 内存分配 ---------- */
/* 固件后门通常只暴露 malloc/free (无 realloc), 所以本运行时用一张
 * 固定大小分配记录表记录每块大小, 使 realloc 能安全拷贝 min(old,new)。
 * FreeType 的字形加载器会频繁 realloc 扩展缓冲区, 此表必须存在。 */
/* 8 字节对齐分配
 * ---------------
 * ARM926EJ-S 上 LDRD/STRD 访问 8 字节值（double / long long）要求 8 字节对齐，
 * 而固件堆只保证 4 字节；非对齐访问会触发 data abort（在设备上表现为无故重启）。
 * 做法与 SDK 的 hp_rt.c 一致：多要 16 字节，对齐后在 [-2] 存原始指针、
 * [-1] 存请求大小（供 realloc）。C++ 的 operator new、FreeType、HarfBuzz
 * 全部经此路径分配。 */
static void* prime_aligned_alloc(uint32_t size) {
    uint32_t* raw = (uint32_t*)prime_lmalloc(size + 16u);   /* SVC 0x10037 */
    uint32_t* aligned;
    if (raw == 0 && g_services != 0 && g_services->malloc != 0) {
        raw = (uint32_t*)g_services->malloc(size + 16u);    /* 后备：引导器服务表 */
    }
    if (raw == 0) return 0;
    aligned = (uint32_t*)(((uintptr_t)raw + 15u) & ~(uintptr_t)7u);
    aligned[-2] = (uint32_t)(uintptr_t)raw;
    aligned[-1] = size;
    return aligned;
}

/* C 名字分配器: FreeType(ftsystem.c) 直接调用这些符号 */
void* malloc(size_t size) { return prime_aligned_alloc((uint32_t)size); }

void free(void* ptr) {
    if (ptr == 0) return;
    prime_lfree((void*)(uintptr_t)((uint32_t*)ptr)[-2]);    /* SVC 0x1003A */
}

void* realloc(void* ptr, size_t size) {
    uint32_t old_size;
    uint32_t copy_n;
    void* new_ptr;
    if (ptr == 0) return prime_aligned_alloc((uint32_t)size);
    old_size = ((uint32_t*)ptr)[-1];
    new_ptr = prime_aligned_alloc((uint32_t)size);
    if (new_ptr == 0) return 0;
    copy_n = (old_size < (uint32_t)size) ? old_size : (uint32_t)size;
    for (uint32_t i = 0; i < copy_n; ++i) {
        ((uint8_t*)new_ptr)[i] = ((const uint8_t*)ptr)[i];
    }
    free(ptr);
    return new_ptr;
}

void* calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* p = prime_aligned_alloc((uint32_t)total);
    if (p != 0) {
        uint8_t* b = (uint8_t*)p;
        for (size_t i = 0; i < total; ++i) b[i] = 0;
    }
    return p;
}

/* ---------- 最小 libc 子集 ---------- */
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

extern "C" void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        ++x; ++y;
    }
    return 0;
}

extern "C" size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

extern "C" int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

extern "C" int strncmp(const char* a, const char* b, size_t n) {
    while (n-- && *a && *a == *b) { ++a; ++b; }
    if (n == (size_t)-1) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

extern "C" char* strchr(const char* s, int c) {
    while (*s) { if (*s == (char)c) return (char*)s; ++s; }
    return (c == 0) ? (char*)s : 0;
}

extern "C" char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

extern "C" char* strstr(const char* haystack, const char* needle) {
    if (*needle == '\0') return (char*)haystack;
    for (const char* h = haystack; *h != '\0'; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*b == '\0') return (char*)h;
    }
    return 0;
}

extern "C" void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void*)p;
        ++p;
    }
    return 0;
}

/* 通用快速排序 (FreeType 用于 cmap 表排序, 数组较小) */
/* ctype（nmarkdown 的解析/排版层用到） */
extern "C" int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
extern "C" int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
extern "C" int isdigit(int c) { return c >= '0' && c <= '9'; }
extern "C" int isalnum(int c) { return isalpha(c) || isdigit(c); }
extern "C" int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* libc 字符分类（nmarkdown 的 app 层用到） */
extern "C" int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
extern "C" int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* md4c 需要 bsearch（在已排序表里做二分查找） */
extern "C" void* bsearch(const void* key, const void* base, size_t count,
                         size_t size, int (*compare)(const void*, const void*)) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t* p = (const uint8_t*)base + mid * size;
        int c = compare(key, p);
        if (c == 0) return (void*)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

extern "C" void qsort(void* base, size_t count, size_t size,
                      int (*compare)(const void*, const void*)) {
    uint8_t* arr = (uint8_t*)base;
    if (count < 2) return;
    /* 简单插入排序 (稳定, 对 cmap 的已近排序数据快) */
    for (size_t i = 1; i < count; ++i) {
        uint8_t* key = arr + i * size;
        size_t j = i;
        while (j > 0 &&
               compare(key - size, key) > 0) {
            /* 交换 (j-1) 与 j */
            uint8_t* a = arr + (j - 1) * size;
            uint8_t* b = arr + j * size;
            for (size_t k = 0; k < size; ++k) {
                uint8_t t = a[k]; a[k] = b[k]; b[k] = t;
            }
            --j;
        }
    }
}

/* libstdc++ hashtable rehash 需要的 floor (soft-float) */
extern "C" double floor(double x) {
    long long i = (long long)x;
    if (x >= 0.0 || (double)i == x) return (double)i;
    return (double)(i - 1);
}

/* libstdc++ atexit_arm.o 引用; 本移植无全局对象, 忽略注册即可 */
extern "C" int __cxa_atexit(void (*func)(void*), void* arg, void* dso) {
    (void)func; (void)arg; (void)dso;
    return 0;
}

/* ARM EABI 异常展开表查找 (动态链接器通常提供, 我们自给)。
 * 返回本 ELF 的 .ARM.exidx 段基址 (0 = 失败); 链接器定义
 * __exidx_start/__exidx_end。 */
extern "C" int __gnu_Unwind_Find_exidx(void* pc, int* pcount) {
    extern char __exidx_start[] __attribute__((weak));
    extern char __exidx_end[] __attribute__((weak));
    (void)pc;
    if (pcount != 0) {
        *pcount = (int)((unsigned long)__exidx_end -
                        (unsigned long)__exidx_start) / 8;
    }
    return (int)(unsigned long)__exidx_start;
}

/* ---------- stdio stub ----------
 * 渲染库只用内存字体 (load_from_memory/FT_New_Memory_Face), 不经过
 * FreeType 的文件流路径; ftsystem.c 的 FT_Stream_Open / font_pack.cpp 的
 * load_from_file 虽被编译但不会执行。提供失败/空操作 stub 以满足
 * --no-undefined。若未来启用外部字体文件, 需替换为 Besta OS 文件 SVC
 * (_afopen 0x100C9 族)。 */
extern "C" void* fopen(const char* path, const char* mode) {
    (void)path; (void)mode;
    return 0;
}
extern "C" int fclose(void* stream) {
    (void)stream;
    return 0;
}
extern "C" std::size_t fread(void* ptr, std::size_t size, std::size_t count,
                             void* stream) {
    (void)ptr; (void)size; (void)count; (void)stream;
    return 0;
}
extern "C" int fseek(void* stream, long offset, int whence) {
    (void)stream; (void)offset; (void)whence;
    return -1;
}
extern "C" long ftell(void* stream) {
    (void)stream;
    return 0;
}

/* libstdc++ 全局唯一对象句柄 */
extern "C" void* __dso_handle = 0;

/* abs (libstdc++/primitives 用) */
extern "C" int abs(int value) {
    return value < 0 ? -value : value;
}

/* ---------- 字符串/格式化 (HarfBuzz 序列化路径用) ---------- */
extern "C" char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = '\0';
    return dst;
}

extern "C" long strtol(const char* s, char** endptr, int base) {
    long value = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '-') { sign = -1; ++s; }
    else if (*s == '+') { ++s; }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (base == 0) {
        base = (*s == '0') ? 8 : 10;
    }
    const char* start = s;
    for (;; ++s) {
        int digit;
        char c = *s;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        value = value * base + digit;
    }
    if (s == start) {
        if (endptr) *endptr = (char*)s;
        return 0;
    }
    if (endptr) *endptr = (char*)s;
    return sign * value;
}

extern "C" unsigned long strtoul(const char* s, char** endptr, int base) {
    return (unsigned long)strtol(s, endptr, base);
}

/* mini vsnprintf: 支持 %d %u %x %X %s %c %% 及 %ld/%lu; 宽度/精度忽略。
 * HarfBuzz 仅在 feature/variation 序列化路径使用, 正确性覆盖足够。 */
extern "C" int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    if (size == 0) return 0;
    char* out = buf;
    const char* end = buf + size - 1;
    const char* p = fmt;
    while (*p && out < end) {
        if (*p != '%') { *out++ = *p++; continue; }
        ++p;
        if (*p == '%') { *out++ = '%'; ++p; continue; }
        /* 跳过可选 flags/width/precision (简化) */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' ||
               *p == '#' || (*p >= '1' && *p <= '9') || *p == '.') {
            ++p;
        }
        char length = 0;
        if (*p == 'l') { length = 'l'; ++p; if (*p == 'l') { ++p; } }
        else if (*p == 'h') { ++p; if (*p == 'h') ++p; }
        char spec = *p++;
        char tmp[24];
        unsigned int tn = 0;
        if (spec == 's') {
            const char* s = va_arg(ap, const char*);
            if (s == 0) s = "(null)";
            while (*s && out < end) *out++ = *s++;
            continue;
        } else if (spec == 'c') {
            int c = va_arg(ap, int);
            if (out < end) *out++ = (char)c;
            continue;
        }
        if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' ||
            spec == 'X') {
            long v;
            if (length == 'l') {
                v = (spec == 'u') ? (long)va_arg(ap, unsigned long)
                                  : va_arg(ap, long);
            } else {
                v = (spec == 'u') ? (long)va_arg(ap, unsigned int)
                                  : (long)va_arg(ap, int);
            }
            unsigned long uv = (spec == 'd' || spec == 'i') && v < 0
                                   ? (unsigned long)(-v)
                                   : (unsigned long)v;
            if ((spec == 'd' || spec == 'i') && v < 0 && out < end) {
                *out++ = '-';
            }
            if (spec == 'x' || spec == 'X') {
                char digits[24];
                unsigned int dn = 0;
                if (uv == 0) digits[dn++] = '0';
                while (uv) {
                    unsigned int d = uv & 0xFU;
                    digits[dn++] = (char)(d < 10 ? '0' + d
                                                 : (spec == 'X' ? 'A' : 'a') + d - 10);
                    uv >>= 4;
                }
                while (dn && out < end) *out++ = digits[--dn];
            } else {
                if (uv == 0) tmp[tn++] = '0';
                while (uv) { tmp[tn++] = (char)('0' + (uv % 10)); uv /= 10; }
                while (tn && out < end) *out++ = tmp[--tn];
            }
        }
    }
    *out = '\0';
    return (int)(out - buf);
}

extern "C" int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* 浮点舍入 (HarfBuzz glyph extents 用) */
extern "C" float floorf(float x) {
    int i = (int)x;
    if (x >= 0.0f || (float)i == x) return (float)i;
    return (float)(i - 1);
}
extern "C" float ceilf(float x) {
    int i = (int)x;
    if (x <= 0.0f || (float)i == x) return (float)i;
    return (float)(i + 1);
}

extern "C" double fabs(double x) {
    return x < 0.0 ? -x : x;
}

/* ---------- libstdc++ 可选路径 stub ----------
 * 以下仅被错误路径/未使用功能引用 (random_device, strerror), 提供
 * 空操作以满足 --no-undefined; 不会被正常渲染流程调用。 */
extern "C" char* getenv(const char* name) {
    (void)name;
    return 0;
}
extern "C" int getentropy(void* buffer, size_t length) {
    (void)buffer; (void)length;
    return -1;
}
extern "C" int arc4random(void) {
    return 0;
}
extern "C" long read(int fd, void* buf, unsigned long count) {
    (void)fd; (void)buf; (void)count;
    return -1;
}
extern "C" int __xpg_strerror_r(int errnum, char* buf, size_t buflen) {
    (void)errnum;
    if (buf != 0 && buflen > 0) buf[0] = '\0';
    return 22;  /* EINVAL: 本移植无错误字符串 */
}

/* ---------- setjmp/longjmp (无 VFP 版) ----------
 * FreeType ttcmap 验证器用 ft_setjmp。G1 (ARM926EJ-S) 无 VFP, 不能使用
 * newlib 带 vstmia {d8-d15} 的版本 —— 这里只保存整数寄存器
 * (r4-r11, sp, lr = 10 字), 与 newlib jmp_buf 前 10 个字布局一致。 */
extern "C" int setjmp(void* env) __attribute__((naked, target("arm")));
extern "C" void longjmp(void* env, int val) __attribute__((naked, target("arm")));

extern "C" int setjmp(void* env) {
    __asm volatile(
        "stmia r0!, {r4, r5, r6, r7, r8, r9, sl, fp, sp, lr}\n\t"
        "mov r0, #0\n\t"
        "mov pc, lr");
}

extern "C" void longjmp(void* env, int val) {
    __asm volatile(
        "ldmia r0!, {r4, r5, r6, r7, r8, r9, sl, fp, sp, lr}\n\t"
        "movs r0, r1\n\t"
        "moveq r0, #1\n\t"
        "mov pc, lr");
}

extern "C" void abort(void) {
    /* 无 OS: 直接挂起。连按 ON 键可退出 (HP Prime OS 恢复)。 */
    for (;;) { /* halt */ }
}

extern "C" void __assert_func(const char* file, int line, const char* func,
                              const char* failedexpr) {
    (void)file; (void)line; (void)func; (void)failedexpr;
    abort();
}

extern "C" int __errno(void) { return 0; }

/* 供 libstdc++ 使用的可写 errno 存储 */
int* __aeabi_errno_addr(void) {
    static int errno_value = 0;
    return &errno_value;
}

} /* extern "C" */

/* ---------- C++ operator new/delete ---------- */
#include <new>

void* operator new(std::size_t size) {
    void* p = malloc(size);
    if (p == 0) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t size) {
    void* p = malloc(size);
    if (p == 0) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return malloc(size);
}

void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
