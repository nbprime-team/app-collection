/*
 * prime_platform.cpp — HP Prime 平台适配实现 (SVC 直调版)
 * ========================================================
 * v2: 不再依赖 Python 传 framebuffer 地址 —— 直接调用 Besta OS SVC:
 *   - Display:  SVC 0x1008D 获取活动 LCD (framebuffer, XRGB32), 内部
 *               RGB565 渲染缓冲, present 时转换写入并 D-Cache clean。
 *   - Input:    SVC 0x10040 非阻塞轮询挂起事件, HP 键码 → InputEvent。
 *   - Clock:    SVC 0x100A5 GetSysTime, SVC 0x10008 OSSleep。
 *   - File:     渲染库形态无外部文件, stub。
 * 服务表 (PrimeServices) 仍作为后备: 若 SVC 不可用 (模拟环境), 可用其
 * framebuffer 字段 (此时假定 RGB565 布局)。
 */
#include "prime_platform.h"

#include <cstdint>
#include <vector>

#include "prime_log.h"
#include "prime_svc.h"
#include "prime_doc.h"
#include <cstring>

/* nbprime SDK 资源：toolchain/sdk（C 接口，需 extern "C"） */
extern "C" {
int  prime_hook_install(void (*cb)(void* event));
void prime_hook_remove(void);
int  prime_sys_get_event(void* event);
void prime_sys_sleep(unsigned ms);   /* platform/prime_input.S: SVC 0x10008 */          /* platform/prime_input.S: SVC 0x1003f */
}

/* 触摸手势的跨调用状态（原事件线程的局部 static） */
static bool g_t_down = false;
static int  g_t_x0 = 0;
static int  g_t_y0 = 0;

namespace nmarkdown {
namespace {

/* D-Cache clean: 与 ndless 版同一思路 (LCD DMA 前回写脏行)。
 * ARM926EJ-S (G1) 与 Cortex-A7 (G2) 均支持 CP15 c7,c10 操作。 */
void clean_dcache_range(const void* data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    const std::uintptr_t kCacheLineBytes = 32;
    std::uintptr_t current =
        reinterpret_cast<std::uintptr_t>(data) & ~(kCacheLineBytes - 1U);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(data) + size + kCacheLineBytes - 1U) &
        ~(kCacheLineBytes - 1U);
    for (; current < end; current += kCacheLineBytes) {
        __asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(current) : "memory");
    }
    const std::uint32_t zero = 0;
    __asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(zero) : "memory");
}

/* RGB565 → XRGB32 (Besta 活动 LCD 帧缓冲格式, doom.c 同款) */
inline std::uint32_t rgb565_to_xrgb32(std::uint16_t c) {
    const std::uint32_t r = (c >> 11U) & 0x1FU;
    const std::uint32_t g = (c >> 5U) & 0x3FU;
    const std::uint32_t b = c & 0x1FU;
    return 0xFF000000U | ((r << 3U) | (r >> 2U)) << 16U |
           ((g << 2U) | (g >> 4U)) << 8U | ((b << 3U) | (b >> 2U));
}

/* ---------- 事件线程共享状态 (主线程读, 事件线程写) ---------- */
volatile int      g_ev_ready = 0;      /* 1 = 有事件待主线程消费 */
volatile uint32_t g_ev_out_type = 0;   /* 解析后的 InputEventType */
volatile uint32_t g_ev_raw_type = 0;   /* 原始事件类型 (诊断)     */
volatile uint32_t g_ev_raw_w8 = 0;
volatile uint32_t g_ev_raw_w9 = 0;
void* g_ev_thread = nullptr;

/* HP Prime 物理键扫描码 → nMarkdown 语义事件。
 * 扫描码表来自 HP Prime 键盘布局 (与 devkit 的 HP_KEY 一致)。 */
InputEvent map_scan_code(std::uint32_t scan) {
    InputEvent ev;
    switch (scan) {
        case 0x03: ev.type = InputEventType::ScrollLineUp; break;    /* UP    */
        case 0x05: ev.type = InputEventType::ScrollLineDown; break;  /* DOWN  */
        case 0x02: ev.type = InputEventType::SwipeLeft; break;       /* LEFT  */
        case 0x04: ev.type = InputEventType::SwipeRight; break;      /* RIGHT */
        case 0x0D: ev.type = InputEventType::Activate; break;        /* ENTER */
        case 0x01: ev.type = InputEventType::Back; break;            /* ESC   */
        case 0x83: ev.type = InputEventType::Quit; break;            /* ON    */
        case 0xB9: ev.type = InputEventType::OpenMenu; break;        /* +     */
        case 0xB1: ev.type = InputEventType::OpenBookmarks; break;   /* APPS  */
        case 0x8B: ev.type = InputEventType::OpenSettings; break;    /* SHIFT */
        default: ev.type = InputEventType::None; break;
    }
    return ev;
}

}  // namespace

/* ---------- Display ---------- */
DisplayPrime::DisplayPrime(const PrimeServices* services)
    : services_(services) {}

bool DisplayPrime::initialize() {
    // 1) 优先 SVC 获取真实活动 LCD (XRGB32)
    PrimeLCD** pp_lcd = prime_get_lcd();
    if (pp_lcd != nullptr && *pp_lcd != nullptr &&
        (*pp_lcd)->window1_bufferstart != 0) {
        fb_addr_ = (*pp_lcd)->window1_bufferstart;
        fb_width_ = (*pp_lcd)->x_res;
        fb_height_ = (*pp_lcd)->y_res;
        fb_format_ = FbFormat::Xrgb32;
    } else if (services_ != nullptr && services_->framebuffer != 0) {
        // 2) 后备: 服务表 framebuffer (假定 RGB565 布局)
        fb_addr_ = services_->framebuffer;
        fb_width_ = static_cast<int>(services_->fb_width);
        fb_height_ = static_cast<int>(services_->fb_height);
        fb_format_ = FbFormat::Rgb565;
    } else {
        return false;
    }
    if (fb_width_ <= 0 || fb_height_ <= 0) {
        fb_format_ = FbFormat::None;
        return false;
    }
    // 内部 RGB565 渲染缓冲: 固定 320x240 逻辑画布 (横屏)。
    // 物理 framebuffer 的竖屏/偏移由 present() 自适应转换。
    try {
        offscreen_.assign(static_cast<std::size_t>(kLogicalWidth) *
                              kLogicalHeight,
                          0);
    } catch (...) {
        fb_format_ = FbFormat::None;
        return false;
    }
    render_surface_ =
        Surface565(offscreen_.data(), kLogicalWidth, kLogicalHeight,
                   kLogicalWidth);
    initialized_ = render_surface_.valid();
    return initialized_;
}

void DisplayPrime::shutdown() {
    initialized_ = false;
    fb_format_ = FbFormat::None;
    render_surface_ = Surface565();
    offscreen_.clear();
}

Surface565 DisplayPrime::surface() {
    return initialized_ ? render_surface_ : Surface565();
}

bool DisplayPrime::fb_info(FbInfo& out) const {
    if (!initialized_) return false;
    out.x_res = static_cast<uint32_t>(fb_width_);
    out.y_res = static_cast<uint32_t>(fb_height_);
    out.bpp = 32;
    out.bytes_per_line = static_cast<uint32_t>(fb_width_) * 4u;
    out.bufferstart = fb_addr_;
    out.format = fb_format_ == FbFormat::Xrgb32
                     ? 1
                     : (fb_format_ == FbFormat::Rgb565 ? 2 : 0);
    return true;
}

/* present: 把 320x240 逻辑画布写入物理 framebuffer。
 * 布局策略 (由服务表 display_flags 配置, 默认 0 = 自动):
 *   - 竖屏 (y_res > x_res): 内容顺时针旋转 90° 后写入 (内容占满竖屏)
 *   - 横屏: 直写
 *   - 居中: 内容在物理屏上居中 (默认开)
 *   - flip_y: framebuffer y 轴反向时置 bit0 (本机无 VFP, 整数运算) */
void DisplayPrime::present() {
    if (!initialized_) return;
    const std::uint32_t flags =
        services_ != nullptr ? services_->display_flags : 0u;
    const bool auto_mode = flags == 0u;
    const bool flip_y = (flags & 1u) != 0;
    const bool force_rotate = (flags & 2u) != 0;
    const bool center_off = (flags & 4u) != 0;
    const bool portrait = fb_height_ > fb_width_;
    const bool do_rotate = force_rotate || (auto_mode && portrait);
    const bool do_center = !center_off;

    const int cw = kLogicalWidth;
    const int ch = kLogicalHeight;

    switch (fb_format_) {
        case FbFormat::Xrgb32: {
            // RGB565 逻辑画布 → XRGB32 活动 LCD 帧缓冲
            std::uint32_t* fb = reinterpret_cast<std::uint32_t*>(
                static_cast<std::uintptr_t>(fb_addr_));
            const int fb_w = fb_width_;
            const int fb_h = fb_height_;
            if (do_rotate) {
                // 顺时针 90°: 内容 (cx, cy) → fb (fx=ch-1-cy, fy=cx)
                // 旋转后内容尺寸 = ch x cw (竖屏)
                const int ox = do_center ? (fb_w - ch) / 2 : 0;
                const int oy = do_center ? (fb_h - cw) / 2 : 0;
                for (int cy = 0; cy < ch; ++cy) {
                    const std::uint16_t* src = render_surface_.row(cy);
                    const int fx = ch - 1 - cy + ox;
                    if (fx < 0 || fx >= fb_w) continue;
                    std::uint32_t* dst = fb + fx;
                    for (int cx = 0; cx < cw; ++cx) {
                        const int fy = cx + oy;
                        if (fy < 0 || fy >= fb_h) continue;
                        dst[static_cast<std::size_t>(fy) * fb_w] =
                            rgb565_to_xrgb32(src[cx]);
                    }
                }
            } else {
                const int ox = do_center ? (fb_w - cw) / 2 : 0;
                const int oy = do_center ? (fb_h - ch) / 2 : 0;
                for (int y = 0; y < ch; ++y) {
                    const std::uint16_t* src = render_surface_.row(y);
                    const int fy = oy + (flip_y ? (ch - 1 - y) : y);
                    if (fy < 0 || fy >= fb_h) continue;
                    std::uint32_t* dst =
                        fb + static_cast<std::size_t>(fy) * fb_w;
                    for (int x = 0; x < cw; ++x) {
                        if (ox + x >= fb_w) break;
                        if (ox + x < 0) continue;
                        dst[ox + x] = rgb565_to_xrgb32(src[x]);
                    }
                }
            }
            clean_dcache_range(fb,
                               static_cast<std::size_t>(fb_w) * fb_h * 4U);
            break;
        }
        case FbFormat::Rgb565: {
            // 服务表 framebuffer: 直接 565 拷贝 (行距按 stride_bytes)
            std::uint16_t* fb = reinterpret_cast<std::uint16_t*>(
                static_cast<std::uintptr_t>(fb_addr_));
            const std::size_t stride_words =
                services_ != nullptr
                    ? static_cast<std::size_t>(services_->fb_stride_bytes) / 2U
                    : static_cast<std::size_t>(fb_width_);
            if (do_rotate) {
                const int ox = do_center ? (fb_width_ - ch) / 2 : 0;
                const int oy = do_center ? (fb_height_ - cw) / 2 : 0;
                for (int cy = 0; cy < ch; ++cy) {
                    const std::uint16_t* src = render_surface_.row(cy);
                    const int fx = ch - 1 - cy + ox;
                    if (fx < 0 || fx >= fb_width_) continue;
                    for (int cx = 0; cx < cw; ++cx) {
                        const int fy = cx + oy;
                        if (fy < 0 || fy >= fb_height_) continue;
                        fb[static_cast<std::size_t>(fy) * stride_words + fx] =
                            src[cx];
                    }
                }
            } else {
                const int ox = do_center ? (fb_width_ - cw) / 2 : 0;
                const int oy = do_center ? (fb_height_ - ch) / 2 : 0;
                for (int y = 0; y < ch; ++y) {
                    const std::uint16_t* src = render_surface_.row(y);
                    const int fy = oy + (flip_y ? (ch - 1 - y) : y);
                    if (fy < 0 || fy >= fb_height_) continue;
                    for (int x = 0; x < cw; ++x) {
                        if (ox + x < 0 || ox + x >= fb_width_) continue;
                        fb[static_cast<std::size_t>(fy) * stride_words + ox +
                           x] = src[x];
                    }
                }
            }
            clean_dcache_range(
                fb, static_cast<std::size_t>(fb_width_) * 2U * fb_height_);
            break;
        }
        case FbFormat::None:
            break;
    }
}

/* ---------- Input ---------- */
InputPrime::InputPrime(const PrimeServices* services) : services_(services) {}

void InputPrime::drain(void) {
    /* 事件线程的 ClearAllEvents() 已清空队列; 这里仅清共享标志与触摸态 */
    g_ev_ready = 0;
}

bool InputPrime::poll(InputEvent& event) {
    event = InputEvent();
#ifdef PRIMETEX_EVENT_DIAG
    prime_log_write("[ev] ty=");
    prime_log_hex(g_ev_raw_type);
    prime_log_write(" w8=");
    prime_log_hex(g_ev_raw_w8);
    prime_log_write(" w9=");
    prime_log_hex(g_ev_raw_w9);
    prime_log_write(" out=");
    prime_log_int(static_cast<int>(g_ev_out_type));
    prime_log_write("\n");
#endif
    event.type = static_cast<InputEventType>(g_ev_out_type);
    g_ev_ready = 0;
    return true;
}

/* ---------- 输入：nbprime SDK 原生钩子 ----------
 * 用 nbprime 的 toolchain/sdk/prime_hook.c 把固件输入分发入口 0x307FBFA0
 * 换成指向本文件回调的 trampoline。ABI 见 toolchain/sdk/docs/hook_abi.md：
 *   words[7]     = 事件类型 (0x10 按键按下 / 0x1,0x2,0x8 触摸)
 *   words[8]>>16 = 键码 或 触摸 X ;  words[9] = 触摸 Y
 * 回调在固件上下文中执行：先取事件（SDK 契约），再立刻解析为 InputEvent。
 *
 * 对照：Upsilon 的「OSCreateThread + 阻塞 GetEvent」在**原生固件下不可用**
 * （见 toolchain/sdk/README.md 的平台说明），故这里用钩子路线。 */
namespace {

void prime_input_on_event(void* event) {
    std::uint32_t* buf = static_cast<std::uint32_t*>(event);

    prime_sys_get_event(event);              /* SDK 契约：回调内自行取事件 */

    const std::uint32_t type = buf[7];
    std::uint32_t out = 0;

    if (type == 0x10U) {                     /* 按键按下 */
        out = static_cast<std::uint32_t>(map_scan_code(buf[8] >> 16U).type);
    } else if (type == 0x1U) {               /* 触摸按下 */
        g_t_down = true;
        g_t_x0 = static_cast<int>(buf[8] >> 16U);
        g_t_y0 = static_cast<int>(buf[9]);
    } else if (type == 0x2U) {
        /* 触摸移动：忽略（以按下/抬起位置判定滑动） */
    } else if (type == 0x8U && g_t_down) {   /* 触摸抬起 */
        const int dx = static_cast<int>(buf[8] >> 16U) - g_t_x0;
        const int dy = static_cast<int>(buf[9]) - g_t_y0;
        g_t_down = false;
        if (dx < -30 || dy > 30)      out = static_cast<std::uint32_t>(InputEventType::ScrollLineDown);
        else if (dx > 30 || dy < -30) out = static_cast<std::uint32_t>(InputEventType::ScrollLineUp);
        else                          out = static_cast<std::uint32_t>(InputEventType::Activate);
    }
    /* keyup (0x100000) 忽略 */

    if (out != 0) {
        g_ev_raw_type = buf[7];
        g_ev_raw_w8 = buf[8];
        g_ev_raw_w9 = buf[9];
        g_ev_out_type = out;
        g_ev_ready = 1;
    }
}

}  // namespace

/* 名字沿用（调用点不改）：实际是「安装 / 卸载 nbprime 原生钩子」 */
int prime_event_thread_start(void) {
    return prime_hook_install(prime_input_on_event) ? 0 : -1;
}

void prime_event_thread_stop(void) {
    prime_hook_remove();
    g_ev_ready = 0;
}

/* ---------- Clock ---------- */
ClockPrime::ClockPrime(const PrimeServices* services) : services_(services) {}

std::uint64_t ClockPrime::milliseconds() const {
    /* 不调 SVC 0x100A5 (GetSysTime)：注入上下文下该服务不可靠（参考实现注明
     * "注入上下文不可靠"并绕开）。改为单调递增的估算时钟，只用于动画节流与
     * 耗时统计，不需要真实时刻。 */
    return ++busy_estimate_ms_;
}

void ClockPrime::sleep_ms(std::uint32_t duration) {
    busy_estimate_ms_ += (duration != 0 ? duration : 1);
    prime_os_sleep(duration);
}

/* ---------- FileSystem (stub) ---------- */
/* 内嵌示例文档：只认 kPrimeDocPath（app-collection 无文件 API） */
bool FileSystemPrime::probe(const char* path, DocumentProbe& result,
                            std::string& error) {
    result = DocumentProbe();
    if (path == nullptr || std::strcmp(path, kPrimeDocPath) != 0) {
        error = "embedded filesystem: unknown document";
        return false;
    }
    result.size = kPrimeDocLen;
    result.bytes_sampled = kPrimeDocLen;
    return true;
}

bool FileSystemPrime::read_all(const char* path, std::size_t maximum_size,
                               std::vector<std::uint8_t>& data,
                               std::string& error) {
    if (path == nullptr || std::strcmp(path, kPrimeDocPath) != 0) {
        error = "embedded filesystem: unknown document";
        return false;
    }
    if (kPrimeDocLen > maximum_size) {
        error = "embedded document exceeds the configured size limit";
        return false;
    }
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(kPrimeDocText);
    data.assign(p, p + kPrimeDocLen);
    return true;
}

bool FileSystemPrime::read_range(const char* path, std::uint64_t offset,
                                 std::uint8_t* out, std::size_t length,
                                 std::string& error) {
    if (path == nullptr || std::strcmp(path, kPrimeDocPath) != 0) {
        error = "embedded filesystem: unknown document";
        return false;
    }
    if (offset > kPrimeDocLen || length > kPrimeDocLen - offset) {
        error = "embedded filesystem: read past end of document";
        return false;
    }
    std::memcpy(out, kPrimeDocText + offset, length);
    return true;
}

bool FileSystemPrime::write_atomic(const char*, const std::uint8_t*, std::size_t,
                                   std::string& error) {
    error = "embedded filesystem is read-only";
    return false;
}

}  // namespace nmarkdown
