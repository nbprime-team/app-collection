/*
 * prime_log.cpp — 共享日志实现 (见 prime_log.h)
 */
#include "prime_log.h"
#include "prime_svc.h"

/* Upsilon 形态: 无 Python 引导器回显环形缓冲, 日志直接 Printf 上屏
 * (诊断用; muteki 的 Printf 由系统字体渲染到 framebuffer)。 */
#ifdef PRIMETEX_UPSILON
#include <muteki.h>
#endif

namespace {

PrimeLogStruct g_log;  // BSS 清零, 无动态构造

}  // namespace


extern "C" void prime_log_sp(void)
{
    unsigned sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    prime_log_write(" sp=0x");
    prime_log_hex(sp);
}

extern "C" void prime_log_write(const char* text) {
#ifdef PRIMETEX_UPSILON
    Printf("%s", text);
#else
    for (const char* p = text; *p != '\0'; ++p) {
        g_log.buffer[g_log.head % PRIME_LOG_SIZE] = *p;
        ++g_log.head;
    }
#endif
}

extern "C" void prime_log_hex(uint32_t v) {
#ifdef PRIMETEX_UPSILON
    Printf("0x%X", static_cast<unsigned>(v));
#else
    const char* digits = "0123456789ABCDEF";
    char buf[12] = {'0', 'x', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 9; i >= 2; --i) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
    prime_log_write(buf);
#endif
}

extern "C" void prime_log_int(int v) {
#ifdef PRIMETEX_UPSILON
    Printf("%d", v);
#else
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    bool neg = v < 0;
    unsigned u = neg ? static_cast<unsigned>(-v) : static_cast<unsigned>(v);
    do {
        buf[--i] = static_cast<char>('0' + (u % 10u));
        u /= 10u;
    } while (u != 0);
    if (neg) buf[--i] = '-';
    prime_log_write(buf + i);
#endif
}

extern "C" PrimeLogStruct* prime_log_struct(void) {
    return &g_log;
}
