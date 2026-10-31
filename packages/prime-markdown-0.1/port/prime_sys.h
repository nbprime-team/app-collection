/*
 * prime_sys.h — HP Prime 移植的系统服务表与 ABI 约定
 * ====================================================
 * 背景: HP Prime 上的原生 ELF 是 freestanding (无 OS/libc ABI), 由
 * generic_loader.py (PrimeDebug 后门) 注入并调用。本文件定义:
 *
 *   1. PrimeServices —— 引导器构造并写入内存的系统服务表。
 *      引导器把固件后门 malloc/free 地址、framebuffer 地址等填入该表,
 *      再把表地址作为 BootConfig 的第 8 个字段 (services) 传给 ELF 入口。
 *   2. PrimeBootConfig —— 与 generic_loader.py 的 struct.pack 严格一致的
 *      BootConfig 布局 (32 字节, 8 字段)。
 *   3. app_entry —— ELF 入口签名 (与 devkit 约定一致)。
 *
 * 布局一致性: 修改本文件必须同步修改 generic_loader.py 的
 * BootConfigBuilder 与 port/prime_runtime.cpp。
 */
#ifndef PRIME_SYS_H
#define PRIME_SYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 9 x uint32 —— 布局与 generic_loader.py 的 struct.pack("<IIIIIIIII") 一致。
 * v2 注: 移植 ELF 现通过 Besta OS SVC 直接获取 LCD framebuffer 与内存分配
 * (见 port/prime_svc.h), framebuffer 字段仅当 SVC 不可用 (模拟环境) 时
 * 使用 (假定 RGB565 布局, XRGB32 由 SVC 提供)。
 * display_flags 是 ELF 读取显示布局配置的通道 (服务表总是被构造):
 *   bit0: flip_y       — 垂直翻转 (framebuffer y 轴反向时)
 *   bit1: force_rotate — 强制旋转 90° (竖屏 framebuffer 自动旋转, 无需置位)
 *   bit2: no_center    — 不居中 (默认居中)
 * 0 = 自动: 竖屏 (y_res>x_res) 时旋转 90° + 居中, 横屏直写 + 居中。 */
typedef struct PrimeServices {
    void*    (*malloc)(uint32_t size);  /* 固件后门 malloc (可选后备)     */
    void     (*free)(void* ptr);        /* 固件后门 free                   */
    uint32_t framebuffer;               /* LCD framebuffer 地址 (0=自动)   */
    uint32_t fb_width;                  /* 屏幕宽度 (0=自动)               */
    uint32_t fb_height;                 /* 屏幕高度 (0=自动)               */
    uint32_t fb_stride_bytes;           /* 每行字节数 (0=自动)             */
    uint32_t (*clock_ms)(void);         /* 毫秒时钟 (0=不可用)             */
    uint32_t (*key_read)(void);         /* 读物理键扫描码 (0=无键/未实现)   */
    uint32_t display_flags;             /* bit0 flip_y | bit1 rotate | bit2 no_center */
} PrimeServices;

/* 环境变量对 (与 generic_loader.py 的 8 字节 EnvPair 一致) */
typedef struct PrimeEnvPair {
    char* name;
    char* val;
} PrimeEnvPair;

/* 键位映射 (uint16 hp_scan, uint16 app_key — 与 generic_loader.py 的 <HH 一致) */
typedef struct PrimeKeyMap {
    uint16_t hp_scan;
    uint16_t app_key;
} PrimeKeyMap;

/* 32 字节, 8 字段 —— 与 generic_loader.py 的 struct.pack("<IIIIIIII") 一致 */
typedef struct PrimeBootConfig {
    uint32_t       magic;        /* 0xD00BC760, 由 C 端校验               */
    uint32_t       argc;
    char**         argv;
    uint32_t       env_count;
    PrimeEnvPair*  envs;
    uint32_t       key_count;
    PrimeKeyMap*   keys;
    uint32_t       services;     /* PrimeServices* 地址, 0 = 未提供      */
} PrimeBootConfig;

/* ELF 入口: 由 shellcode 加载后引导器以 R0=cfg 调用, 返回日志结构体地址 */
void* app_entry(PrimeBootConfig* cfg);

/* 运行时服务表安装/读取 (prime_runtime.cpp 实现) */
void prime_set_services(const PrimeServices* svc);
const PrimeServices* prime_get_services(void);

#ifdef __cplusplus
}
#endif

#endif /* PRIME_SYS_H */
