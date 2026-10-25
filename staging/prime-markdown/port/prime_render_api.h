/*
 * prime_render_api.h — 对外 C 级渲染 API (字体 + 公式)
 * =====================================================
 * 把 nMarkdown 的 TextSystem (FreeType 字体) 与 MathSystem (LaTeX 公式)
 * 暴露为简单的 C 接口, 供演示 main 或你的宿主程序调用。
 *
 * 使用流程:
 *   1. app_entry 里调用 prime_set_services() (见 prime_sys.h)
 *   2. prime_render_init()          — 加载嵌入字体, 初始化渲染系统
 *   3. prime_render_clear()/text()/formula() — 绘制到 framebuffer
 *   4. prime_render_present()       — D-Cache clean, 送显
 *
 * 线程: 非线程安全, 单线程使用。
 * 异常: 所有 C++ 异常在 API 边界捕获并转换为错误码。
 */
#ifndef PRIME_RENDER_API_H
#define PRIME_RENDER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回值: 0 = 成功; 非 0 = 错误 */
int prime_render_init(void);          /* 初始化字体+公式渲染系统 (0=成功) */
void prime_render_shutdown(void);     /* 释放资源 (可选调用) */

int prime_render_clear(uint16_t color);                 /* 清屏 */
int prime_render_text(const char* utf8,                 /* UTF-8 文本 */
                      int x, int baseline_y,            /* 基线坐标 (px) */
                      int pixel_size,                   /* 字号 (12-22 推荐) */
                      uint16_t foreground,
                      uint16_t background,
                      int dark_theme);                  /* 0=浅色 1=深色 */
/* 带样式文本: mono=1 等宽(代码块), style_flags: bit0=粗体 bit1=斜体
 * (复用 text_system 的 outline synthesis, 无需字体变体) */
int prime_render_text_ex(const char* utf8, int x, int baseline_y,
                         int pixel_size, uint16_t foreground,
                         uint16_t background, int mono,
                         unsigned style_flags);
/* 画水平线 (下划线/删除线用): 从 (x, y) 起 width 像素, 高 1px */
int prime_render_hline(int x, int y, int width, uint16_t color);
/* 精确测量文本像素宽度 (shape + GlyphRun.width, 26.6 定点 → 像素)。
 * 返回 >0 宽度; 失败返回 0 (调用方回退估算)。mono=1 等宽。 */
int prime_render_measure(const char* utf8, int len, int pixel_size,
                         int mono);
int prime_render_formula(const char* latex,             /* LaTeX 公式 */
                         int x, int baseline_y,
                         int pixel_size,
                         uint16_t foreground,
                         uint16_t background,
                         int dark_theme,
                         int max_width);                /* 最大宽度限制 (px) */
/* 公式布局宽高 (layout 一次, 不绘制):
 * out_width/out_height 为像素; 失败返回非 0 (调用方回退估算) */
int prime_render_formula_metrics(const char* latex, int pixel_size,
                                 int max_width, int* out_width,
                                 int* out_height);
int prime_render_present(void);       /* 送显 (D-Cache clean) */

/* 演示页: 渲染一个包含文本+公式的测试页 (README 中的演示内容) */
int prime_render_demo_page(void);

/* framebuffer 信息 (供外部确认渲染目标) */
int  prime_render_width(void);
int  prime_render_height(void);

/* 物理 framebuffer 诊断信息 (真机排障用) */
typedef struct PrimeFbInfo {
    uint32_t x_res;          /* 物理 framebuffer 宽 (像素)  */
    uint32_t y_res;          /* 物理 framebuffer 高 (像素)  */
    uint32_t bpp;            /* 每像素位数 (32=XRGB32)      */
    uint32_t bytes_per_line; /* 每行字节数                  */
    uint32_t bufferstart;    /* framebuffer 物理地址        */
    int      format;         /* 0=none 1=XRGB32 2=RGB565   */
    int      logical_w;      /* 逻辑画布宽 (320)            */
    int      logical_h;      /* 逻辑画布高 (240)            */
} PrimeFbInfo;
int prime_render_fb_info(PrimeFbInfo* out);

/* 统计逻辑画布上非 background 色的像素数 (渲染自检) */
int prime_render_count_ink(uint16_t background);

/* 内部渲染上下文 (prime_layout 共享字体/公式/显示实例) */
typedef struct PrimeRenderShared {
    void* text;     /* nmarkdown::TextSystem*    */
    void* math;     /* nmarkdown::MathSystem*    */
    void* display;  /* DisplayPrime*             */
} PrimeRenderShared;
int prime_render_shared(PrimeRenderShared* out);

/* 等待按键/触摸: 轮询事件线程共享状态直到事件或超时。
 * 首次调用会启动事件线程 (eventg1 权威: call 上下文 OSCreateThread,
 * 线程内 ClearAllEvents + 阻塞 GetEvent, 主线程轮询共享)。
 * 返回 InputEventType 枚举值 (0=None/超时; 常用: 13=Quit[ON],
 * 11=Activate[ENTER/点按], 12=Back[ESC], 2=ScrollLineDown[下键/左滑],
 * 1=ScrollLineUp[上键/右滑]) */
int prime_render_wait_key(int timeout_ms);  /* timeout<=0 无限等待 */

/* 事件线程生命周期: wait_key 会自动启动 (幂等); ELF 退出前必须 stop */
int  prime_render_event_thread_start(void);
void prime_render_event_thread_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* PRIME_RENDER_API_H */
