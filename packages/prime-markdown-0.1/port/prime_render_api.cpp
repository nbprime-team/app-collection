/*
 * prime_render_api.cpp — 渲染 API 实现
 * ====================================
 * 内部持有 TextSystem + MathSystem 的惰性单例 (函数内 static, 首次调用
 * 构造; 本移植无全局静态对象, 全部对象在运行时构造)。
 */
#include "prime_render_api.h"

#include <string>
#include <string_view>

#include "nmarkdown/math/math_system.h"
#include "nmarkdown/render/primitives.h"
#include "nmarkdown/render/surface565.h"
#include "nmarkdown/text/text_renderer.h"
#include "nmarkdown/text/text_system.h"

#include "prime_platform.h"
#include "prime_sys.h"

namespace {

using namespace nmarkdown;

struct RenderRuntime {
    DisplayPrime display{prime_get_services()};
    TextSystem text;
    MathSystem math{text};
    bool ready = false;
};

RenderRuntime& runtime() {
    static RenderRuntime rt;  // 首次调用时构造 (非静态初始化)
    return rt;
}

template <typename Fn>
int safe_call(Fn&& fn) {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return -2;  // 内存不足
    } catch (...) {
        return -1;  // 其他异常
    }
}

}  // namespace

/* 暴露内部渲染上下文给 prime_layout (共享 TextSystem/MathSystem/Display,
 * 避免二次初始化字体)。指针生命周期: prime_render_init 后有效。 */
int prime_render_shared(PrimeRenderShared* out) {
    if (out == nullptr) return -1;
    RenderRuntime& rt = runtime();
    out->text = &rt.text;
    out->math = &rt.math;
    out->display = &rt.display;
    return rt.ready ? 0 : -2;
}

/* ---------- 生命周期 ---------- */
int prime_render_init(void) {
    return safe_call([]() -> int {
        RenderRuntime& rt = runtime();
        if (rt.ready) return 0;
        if (!rt.display.initialize()) return -3;  // framebuffer 未配置
        std::string error;
        if (!rt.text.initialize(error)) return -4;  // 字体加载失败
        rt.ready = true;
        return 0;
    });
}

void prime_render_shutdown(void) {
    runtime().display.shutdown();
}

/* ---------- 绘制 ---------- */
int prime_render_clear(uint16_t color) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready) return -4;
        rt.display.surface().clear(color);
        return 0;
    });
}

int prime_render_text(const char* utf8, int x, int baseline_y, int pixel_size,
                      uint16_t foreground, uint16_t background, int dark_theme) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready || utf8 == nullptr) return -4;
        const std::string source(utf8);
        GlyphRun run;
        const Fx size = fx_from_int(pixel_size);
        if (!rt.text.shape(source.data(), source.size(), size, run,
                           FontRole::BodySans, TextSpacing::Natural)) {
            return -5;
        }
        const Surface565 surface = rt.display.surface();
        rt.text.draw_run(surface, run, x, baseline_y, size, foreground,
                         background, dark_theme != 0, true, surface.bounds());
        return 0;
    });
}

/* 带样式文本: 复用 text_system 的 outline synthesis (粗体/斜体) */
int prime_render_text_ex(const char* utf8, int x, int baseline_y,
                         int pixel_size, uint16_t foreground,
                         uint16_t background, int mono,
                         unsigned style_flags) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready || utf8 == nullptr) return -4;
        const std::string source(utf8);
        GlyphRun run;
        const Fx size = fx_from_int(pixel_size);
        const FontRole role =
            mono != 0 ? FontRole::Monospace : FontRole::BodySans;
        if (!rt.text.shape(source.data(), source.size(), size, run, role,
                           TextSpacing::Natural)) {
            if (role == FontRole::Monospace) {
                /* mono 变体缺失时回退 BodySans */
                if (!rt.text.shape(source.data(), source.size(), size, run,
                                   FontRole::BodySans,
                                   TextSpacing::Natural)) {
                    return -5;
                }
            } else {
                return -5;
            }
        }
        std::uint8_t synthesis = 0;
        if ((style_flags & 1U) != 0) synthesis |= TextSynthesisBold;
        if ((style_flags & 2U) != 0) synthesis |= TextSynthesisItalic;
        const Surface565 surface = rt.display.surface();
        rt.text.draw_run(surface, run, x, baseline_y, size, foreground,
                         background, false, true, surface.bounds(),
                         synthesis);
        return 0;
    });
}

/* 水平线 (下划线/删除线): fill_rect 高 1px */
int prime_render_hline(int x, int y, int width, uint16_t color) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready || width <= 0) return -4;
        const Surface565 surface = rt.display.surface();
        fill_rect(surface, Rect{x, y, width, 1}, color, surface.bounds());
        return 0;
    });
}

/* 精确测量文本宽度: shape 一次, 用 GlyphRun.width (26.6 定点 /64) */
int prime_render_measure(const char* utf8, int len, int pixel_size,
                         int mono) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready || utf8 == nullptr || len <= 0) return 0;
        GlyphRun run;
        const FontRole role =
            mono != 0 ? FontRole::Monospace : FontRole::BodySans;
        if (!rt.text.shape(utf8, static_cast<std::size_t>(len), pixel_size,
                           run, role, TextSpacing::Natural)) {
            if (mono != 0 &&
                !rt.text.shape(utf8, static_cast<std::size_t>(len),
                               pixel_size, run, FontRole::BodySans,
                               TextSpacing::Natural)) {
                return 0;  /* 调用方回退估算 */
            }
            return 0;
        }
        const int px = run.width / 64;
        return px > 0 ? px : 0;
    });
}

int prime_render_formula(const char* latex, int x, int baseline_y,
                         int pixel_size, uint16_t foreground,
                         uint16_t background, int dark_theme, int max_width) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready || latex == nullptr) return -4;
        std::shared_ptr<const MathLayoutResult> laid_out;
        const Fx size = fx_from_int(pixel_size);
        const Fx width_limit =
            max_width > 0 ? fx_from_int(max_width) : fx_from_int(320);
        if (!rt.math.layout(std::string_view(latex), MathStyle::Text, size,
                            width_limit, laid_out)) {
            return -6;  // 公式解析/布局失败
        }
        const Surface565 surface = rt.display.surface();
        rt.math.draw(surface, *laid_out, x, baseline_y, 0, foreground,
                     background, dark_theme != 0, surface.bounds());
        return 0;
    });
}

/* 公式布局宽高 (不绘制): MathLayoutResult.metrics (26.6 定点) */
int prime_render_formula_metrics(const char* latex, int pixel_size,
                                 int max_width, int* out_width,
                                 int* out_height) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready || latex == nullptr || out_width == nullptr ||
            out_height == nullptr) {
            return -4;
        }
        std::shared_ptr<const MathLayoutResult> laid_out;
        const Fx size = fx_from_int(pixel_size);
        const Fx width_limit =
            max_width > 0 ? fx_from_int(max_width) : fx_from_int(320);
        if (!rt.math.layout(std::string_view(latex), MathStyle::Text, size,
                            width_limit, laid_out)) {
            return -6;
        }
        *out_width = fx_ceil(laid_out->metrics.width);
        *out_height = fx_ceil(laid_out->metrics.ascent +
                              laid_out->metrics.descent);
        return 0;
    });
}

int prime_render_present(void) {
    return safe_call([]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready) return -4;
        rt.display.present();
        return 0;
    });
}

int prime_render_width(void) {
    return runtime().display.surface().width();
}

int prime_render_height(void) {
    return runtime().display.surface().height();
}

int prime_render_fb_info(PrimeFbInfo* out) {
    if (out == nullptr) return -1;
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready) return -4;
        DisplayPrime::FbInfo fb;
        if (!rt.display.fb_info(fb)) return -3;
        out->x_res = fb.x_res;
        out->y_res = fb.y_res;
        out->bpp = fb.bpp;
        out->bytes_per_line = fb.bytes_per_line;
        out->bufferstart = fb.bufferstart;
        out->format = fb.format;
        out->logical_w = rt.display.surface().width();
        out->logical_h = rt.display.surface().height();
        return 0;
    });
}

int prime_render_count_ink(uint16_t background) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready) return -4;
        const Surface565 s = rt.display.surface();
        int count = 0;
        for (int y = 0; y < s.height(); ++y) {
            const std::uint16_t* row = s.row(y);
            for (int x = 0; x < s.width(); ++x) {
                if (row[x] != background) ++count;
            }
        }
        return count;
    });
}

int prime_render_wait_key(int timeout_ms) {
    return safe_call([&]() -> int {
        RenderRuntime& rt = runtime();
        if (!rt.ready) return -4;
        const int thr_rc = prime_event_thread_start();  // 幂等
        if (thr_rc != 0) return -7;  // 事件线程创建失败
        InputPrime input(prime_get_services());
        ClockPrime clock(prime_get_services());
        input.drain();  // 清共享标志
        // 超时用「轮询次数 × 30ms」计数, 不依赖 GetSysTime (注入上下文不可靠)
        const int timeout_polls =
            timeout_ms > 0 ? (timeout_ms + 29) / 30 : 0;  // 每轮 30ms
        int polls = 0;
        for (;;) {
            InputEvent ev;
            if (input.poll(ev)) {
                if (ev.type != InputEventType::None) {
                    return static_cast<int>(ev.type);
                }
            }
            clock.sleep_ms(30);
            if (timeout_ms > 0 && ++polls >= timeout_polls) {
                return 0;  // 超时
            }
        }
    });
}

int prime_render_event_thread_start(void) {
    return prime_event_thread_start();
}

void prime_render_event_thread_stop(void) {
    prime_event_thread_stop();
}
