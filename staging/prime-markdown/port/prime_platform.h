/*
 * prime_platform.h — HP Prime 平台适配 (nmarkdown::Display/Input/Clock/FileSystem)
 * =================================================================================
 * 替代原项目的 platform/ndless 层 (它依赖 TI Nspire 的 libndls.h, HP Prime 没有)。
 *
 * 显示: 320x240 RGB565 framebuffer 直写 (地址来自 PrimeServices, 由引导器配置)。
 *       写屏后做 D-Cache clean (ARM CP15), 避免 DMA 读到脏缓存行导致花屏。
 * 输入: 通过服务表 key_read() 轮询物理键, 扫描码→InputEvent 映射 (HP Prime 键表)。
 * 时钟: 服务表 clock_ms() 或忙等估算。
 * 文件: 渲染库形态不使用外部文件, 全部返回"不支持"。
 */
#ifndef PRIME_PLATFORM_H
#define PRIME_PLATFORM_H

#include <cstdint>
#include <vector>

#include "nmarkdown/platform/platform.h"
#include "prime_sys.h"

namespace nmarkdown {

/* ---------- 显示 ---------- */
class DisplayPrime final : public Display {
public:
    /* 逻辑画布: 渲染坐标固定 320x240 横屏, present 时按物理 framebuffer
     * 布局自适应 (竖屏旋转 / 居中 / 翻转), 由 display_flags 配置。 */
    static constexpr int kLogicalWidth = 320;
    static constexpr int kLogicalHeight = 240;

    explicit DisplayPrime(const PrimeServices* services);

    bool initialize() override;
    void shutdown() override;
    Surface565 surface() override;
    void present() override;

    /* 物理 framebuffer 信息 (供诊断日志/宿主确认渲染目标) */
    struct FbInfo {
        uint32_t x_res, y_res, bpp, bytes_per_line, bufferstart;
        int format;  /* 0=none 1=xrgb32 2=rgb565 */
    };
    bool fb_info(FbInfo& out) const;

private:
    enum class FbFormat { None, Xrgb32, Rgb565 };

    const PrimeServices* services_;
    bool initialized_ = false;
    FbFormat fb_format_ = FbFormat::None;
    std::uint32_t fb_addr_ = 0;
    int fb_width_ = 0;
    int fb_height_ = 0;
    std::vector<std::uint16_t> offscreen_;  // RGB565 渲染缓冲 (固定 320x240)
    Surface565 render_surface_;
};

/* ---------- 输入 ---------- */
class InputPrime final : public Input {
public:
    explicit InputPrime(const PrimeServices* services);
    bool poll(InputEvent& event) override;

    /* 丢弃当前已挂起的所有事件 (启动时清残留) */
    void drain(void);

private:
    const PrimeServices* services_;
};

/* 事件线程管理 (eventg1 权威结构):
 *   事件线程由 app_entry (call 上下文) 直接 OSCreateThread 创建, 栈=0 (系统
 *   默认), 线程内 ClearAllEvents() + 阻塞 GetEvent; 主线程轮询共享状态。
 *   (doom/eventg1 均为此模式; 线程由 OSCreateThread 线程嵌套创建或缓冲
 *   过小是此前事件拿不到/行为异常的根因) */
int  prime_event_thread_start(void);
void prime_event_thread_stop(void);

/* ---------- 时钟 ---------- */
class ClockPrime final : public Clock {
public:
    explicit ClockPrime(const PrimeServices* services);
    std::uint64_t milliseconds() const override;
    void sleep_ms(std::uint32_t duration) override;

private:
    const PrimeServices* services_;
    /* 注入上下文下 SVC 0x100A5 (GetSysTime) 不可靠（参考实现亦注明不可依赖），
     * 故用自身累加的估算时钟；milliseconds() 为 const，故为 mutable。 */
    mutable std::uint64_t busy_estimate_ms_ = 0;
};

/* ---------- 文件系统 (stub: 渲染库无外部文件) ---------- */
class FileSystemPrime final : public FileSystem {
public:
    bool probe(const char*, DocumentProbe&, std::string&) override;
    bool read_all(const char*, std::size_t, std::vector<std::uint8_t>&,
                  std::string&) override;
    bool read_range(const char*, std::uint64_t, std::uint8_t*, std::size_t,
                    std::string&) override;
    bool write_atomic(const char*, const std::uint8_t*, std::size_t,
                      std::string&) override;
};

}  // namespace nmarkdown

#endif /* PRIME_PLATFORM_H */
