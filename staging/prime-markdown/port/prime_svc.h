/*
 * prime_svc.h — Besta OS 系统调用 (SVC) 包装层
 * =============================================
 * HP Prime G1 的 Besta OS 是一套 SVC 系统调用内核: 所有系统功能通过
 * `swi #0x10xxxx` 指令调用, muteki 逆向工程 (Project Muteki) 已确认这些
 * SVC 在 HP 原始固件 (Besta OS) 上可用, 不需要经过 PrimeDebug 后门或
 * Python 传地址 —— ELF 在任意加载地址都能直接调用。
 *
 * SVC ABI (与 doom.c / g1sdk syscalls.s 一致):
 *   调用方压入 {r0=第一个参数} 和 {lr}, 然后 `swi #n`; 参数走 r0-r3,
 *   返回值在 r0, OS 的 SVC handler 负责弹栈恢复。
 *
 * 编号来源: g1sdk/src/syscalls.s (muteki), 签名参考 include/muteki 头文件。
 * 注意: 本层只暴露移植工程需要的子集, 完整列表见 SDK 的 syscalls.s。
 */
#ifndef PRIME_SVC_H
#define PRIME_SVC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ LCD / 显示 ============ */
/* SVC 0x1007E ClearScreen: 清屏 (doom 在 GetEvent 前调用, 疑为事件投递触发点) */
void prime_clear_screen(uint32_t fill_with_fg);
/* 活动 LCD 对象 (doom.c 的 LCD_MAGIC 布局) */
typedef struct PrimeLCD {
    uint16_t some_val;
    uint16_t x_res;          /* 320 */
    uint16_t y_res;          /* 240 */
    uint16_t pixel_bits;
    uint16_t bytes_per_line;
    uint16_t brightness_level;
    uint32_t unk1_0;
    uint32_t window1_bufferstart;  /* framebuffer 物理地址 (XRGB32) */
} PrimeLCD;

/* SVC 0x1008D GetActiveLCD: 返回 LCD_MAGIC** (指向指针的指针) */
PrimeLCD** prime_get_lcd(void);
/* SVC 0x10029/0x1002A: 背光开关 */
void prime_lcd_on(void);
void prime_lcd_off(void);
/* SVC 0x1002B: 查询背光 */
int prime_check_lcd_on(void);

/* ============ 输入 / 事件 ============ */
/* SVC 0x10046 TestPendEvent: 非阻塞探测+处理挂起事件 (必须传 ui_event_t*)。
 * 返回 1=有事件已处理, 0=无。buf 至少 72 字节。 */
int prime_test_pend_event(void* buf);
/* SVC 0x10040 GetPendEvent: 非阻塞取一个挂起事件 (doom 未用, 语义不明) */
int prime_get_pend_event(void* buf);
/* SVC 0x1003F GetEvent: 取一个事件, 无事件时阻塞 (doom/eventg1 事件线程同款)。
 * 事件缓冲必须 ≥ 72 字节 (ui_event_t 完整结构, eventg1 打印到 ptr[16])。
 * 缓冲过小会导致 GetEvent 越界写栈 → 事件丢失/行为异常。 */
int prime_get_event(void* buf);
/* SVC 0x1004A ClearAllEvents: 清空所有事件 (eventg1 事件线程开头调用) */
void prime_clear_all_events(void);
/* HP Prime 事件缓冲布局 (doom.c 实测解析):
 *   words[7]     = 事件类型: 0x10 keydown, 0x100000 keyup,
 *                  0x1 触摸按下, 0x2 触摸移动, 0x8 触摸抬起
 *   words[8]>>16 = 键码 (keydown/keyup) 或触摸 X
 *   words[9]     = 触摸 Y */

/* ============ 时间 ============ */
/* SVC 0x100A5 GetSysTime: 8 个 16 位字段 */
typedef struct PrimeDateTime {
    uint16_t year;
    uint16_t month;
    uint16_t weekday;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint16_t millis;
} PrimeDateTime;

void prime_get_sys_time(PrimeDateTime* dt);

/* ============ 内存 ============ */
/* SVC 0x10037/0x10038/0x10039/0x1003A — 与 devkit 导出表 get_addr 同源 */
void* prime_lmalloc(uint32_t size);
void* prime_lcalloc(uint32_t count, uint32_t size);
void* prime_lrealloc(void* ptr, uint32_t size);
void  prime_lfree(void* ptr);

/* ============ 线程 ============ */
/* SVC 0x10000 OSCreateThread(func, user_data, stack_size, defer_start) → thread_t*
 * SVC 0x10001 OSTerminateThread(thr, exit_code) → int
 * SVC 0x10007 OSExitThread(exit_code) — 终止当前线程 (替代 return, 避免
 *   Upsilon 的 app 返回清理路径触发重启) */
void* prime_os_create_thread(void* func, void* user_data, uint32_t stack_size,
                             uint32_t defer_start);
int   prime_os_terminate_thread(void* thr, int exit_code);
void  prime_os_exit_thread(int exit_code);

/* 线程 / 电源 */
/* SVC 0x10008 OSSleep: 睡眠 duration 毫秒 */
void prime_os_sleep(uint32_t duration);
/* SVC 0x1004C SetSystemVariable: 关电源管理 (doom 用 var=85, action=2, val=1) */
int prime_set_system_variable(uint32_t var, uint32_t action, uint32_t value);

#ifdef __cplusplus
/* ============ 文件 IO (SVC 0x1026F/0x100CA/0x100D7) ============
 * 路径与模式必须是 UTF-16LE（固件要求）；模式只接受 "rb" / "wb+"。 */
unsigned prime_fopen(unsigned path_utf16, unsigned mode_utf16);
unsigned prime_fclose(unsigned fd);
unsigned prime_fwrite(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
}

#endif

#endif /* PRIME_SVC_H */
