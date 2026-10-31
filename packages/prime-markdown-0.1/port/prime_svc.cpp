/*
 * prime_svc.cpp — Besta OS SVC 包装实现
 * =====================================
 * 每个包装是 naked 函数, 只执行 doom.c / syscalls.s 同款压栈+SWI 序列:
 *   push {r0}; push {lr}; swi #N
 * 参数经 r0-r3 传入, 返回值留在 r0, OS handler 恢复现场。
 */
#include "prime_svc.h"

#define NAKED_SWI(code)                                        \
    __asm volatile("push {r0}\n\t"                             \
                   "push {lr}\n\t"                             \
                   "swi %0\n\t" :: "i"(code))

/* naked 必须: 纯汇编函数, 无编译器 prologue/epilogue。
 * target("arm") 保险: 即使编译单元是 thumb 也生成 ARM 指令。 */
#define NAKED_FN __attribute__((naked, target("arm")))

#define SWI_GET_LCD              0x1008D
#define SWI_CLEAR_SCREEN         0x1007E
#define SWI_LCD_ON               0x10029
#define SWI_LCD_OFF              0x1002A
#define SWI_CHECK_LCD_ON         0x1002B
#define SWI_TEST_PEND_EVENT      0x10046
#define SWI_GET_PEND_EVENT       0x10040
#define SWI_GET_EVENT            0x1003F
#define SWI_CLEAR_ALL_EVENTS     0x1004A
#define SWI_GET_SYS_TIME         0x100A5
#define SWI_LMALLOC              0x10037
#define SWI_LCALLOC              0x10038
#define SWI_LREALLOC             0x10039
#define SWI_LFREE                0x1003A
#define SWI_OS_SLEEP             0x10008
#define SWI_OS_CREATE_THREAD     0x10000
#define SWI_OS_TERMINATE_THREAD  0x10001
#define SWI_OS_EXIT_THREAD       0x10007
#define SWI_SET_SYSTEM_VARIABLE  0x1004C
#define SWI_FOPEN                0x1026F
#define SWI_FCLOSE               0x100CA
#define SWI_FWRITE               0x100D7

extern "C" {

/* 显示 */
NAKED_FN PrimeLCD** prime_get_lcd(void) {
    NAKED_SWI(SWI_GET_LCD);
}
NAKED_FN void prime_clear_screen(uint32_t fill_with_fg) {
    NAKED_SWI(SWI_CLEAR_SCREEN);
}
NAKED_FN void prime_lcd_on(void) {
    NAKED_SWI(SWI_LCD_ON);
}
NAKED_FN void prime_lcd_off(void) {
    NAKED_SWI(SWI_LCD_OFF);
}
NAKED_FN int prime_check_lcd_on(void) {
    NAKED_SWI(SWI_CHECK_LCD_ON);
}

/* 输入 */
NAKED_FN int prime_test_pend_event(void* buf) {
    NAKED_SWI(SWI_TEST_PEND_EVENT);
}
NAKED_FN int prime_get_pend_event(void* buf) {
    NAKED_SWI(SWI_GET_PEND_EVENT);
}
NAKED_FN int prime_get_event(void* buf) {
    NAKED_SWI(SWI_GET_EVENT);
}
NAKED_FN void prime_clear_all_events(void) {
    NAKED_SWI(SWI_CLEAR_ALL_EVENTS);
}

/* 时间 */
NAKED_FN void prime_get_sys_time(PrimeDateTime* dt) {
    NAKED_SWI(SWI_GET_SYS_TIME);
}

/* 内存 */
NAKED_FN void* prime_lmalloc(uint32_t size) {
    NAKED_SWI(SWI_LMALLOC);
}
NAKED_FN void* prime_lcalloc(uint32_t count, uint32_t size) {
    NAKED_SWI(SWI_LCALLOC);
}
NAKED_FN void* prime_lrealloc(void* ptr, uint32_t size) {
    NAKED_SWI(SWI_LREALLOC);
}
NAKED_FN void prime_lfree(void* ptr) {
    NAKED_SWI(SWI_LFREE);
}

/* 线程/电源 */
NAKED_FN void prime_os_sleep(uint32_t duration) {
    NAKED_SWI(SWI_OS_SLEEP);
}
NAKED_FN void* prime_os_create_thread(void* func, void* user_data,
                                      uint32_t stack_size,
                                      uint32_t defer_start) {
    NAKED_SWI(SWI_OS_CREATE_THREAD);
}
NAKED_FN int prime_os_terminate_thread(void* thr, int exit_code) {
    NAKED_SWI(SWI_OS_TERMINATE_THREAD);
}
NAKED_FN void prime_os_exit_thread(int exit_code) {
    NAKED_SWI(SWI_OS_EXIT_THREAD);
}
NAKED_FN int prime_set_system_variable(uint32_t var, uint32_t action, uint32_t value) {
    NAKED_SWI(SWI_SET_SYSTEM_VARIABLE);
}


/* 文件 IO（落盘用；路径/模式须 UTF-16LE） */
extern "C" {
NAKED_FN unsigned prime_fopen(unsigned path_utf16, unsigned mode_utf16) {
    NAKED_SWI(SWI_FOPEN);
}
NAKED_FN unsigned prime_fclose(unsigned fd) {
    NAKED_SWI(SWI_FCLOSE);
}
NAKED_FN unsigned prime_fwrite(unsigned buf, unsigned nmemb, unsigned size, unsigned fd) {
    NAKED_SWI(SWI_FWRITE);
}
}  /* extern "C" (file IO) */

} /* extern "C" */
