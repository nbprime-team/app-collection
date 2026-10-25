/*
 * prime_demo.cpp — ELF 入口与 Markdown 阅读器
 * ============================================
 * app_entry 是引导器 (main.py) 调用的入口:
 *   1. 校验 BootConfig 魔数
 *   2. 安装 PrimeServices (固件 malloc/free/framebuffer 等)
 *   3. 安装输入钩子 (nbprime SDK prime_hook, 回调内取事件)
 *   4. 初始化字体+渲染系统, 排版内嵌文档
 *   5. 渲染首页并送显; 主循环轮询事件翻页, 按键退出
 *   6. 退出前清屏, 返回 LogStruct* 供日志回显
 *
 * 本条路径**不经过 nMarkdown 的 run_reader**: 直接调用
 * prime_layout_* (布局) + prime_render_* (绘制/送显),
 * 每个 API 内部有 safe_call 保护, C++ 异常不会穿到固件。
 */
#include <cstdint>

#include "nmarkdown/render/surface565.h"
#include "prime_log.h"
#include "prime_render_api.h"
#include "prime_layout.h"
#include "prime_svc.h"
#include "prime_hook.h"
#include "nmarkdown/app/application.h"
#include "nmarkdown/text/text_system.h"
#include "nmarkdown/document/markdown.h"
#include <vector>
#include "prime_doc.h"
#include <cstdlib>
#include "prime_platform.h"
#include "prime_sys.h"

namespace {

using nmarkdown::rgb565;

/* 演示页配色 */
const std::uint16_t kBackground = rgb565(250, 250, 245);  // 浅色纸
const std::uint16_t kInk = rgb565(30, 30, 30);
const std::uint16_t kAccent = rgb565(140, 60, 20);
const std::uint16_t kMuted = rgb565(120, 120, 120);

/* 提示行 (两页共用, 屏幕文字保持英文 — DejaVu 无中文字形) */
void render_hint_line() {
    prime_render_text("ENTER/DOWN/TAP: next   ESC/ON: exit", 6, 226, 11,
                      kMuted, kBackground, 0);
}

/* 页 0: 标题 + 正文 + 基础公式 */
void render_page_0() {
    prime_render_clear(kBackground);

    prime_render_text("PrimeTeX", 6, 32, 22, kAccent, kBackground, 0);
    prime_render_text("LaTeX formula rendering on HP Prime", 6, 62, 16,
                      kInk, kBackground, 0);
    prime_render_text("Embedded FreeType 2.14 + HarfBuzz", 6, 86, 16, kInk,
                      kBackground, 0);

    prime_render_formula("\\frac{1}{2}", 6, 112, 18, kInk, kBackground, 0,
                         300);
    prime_render_formula("\\sqrt{x^2 + 1}", 6, 140, 18, kInk, kBackground, 0,
                         300);
    prime_render_formula("e^{i\\pi} + 1 = 0", 6, 168, 18, kInk, kBackground,
                         0, 300);
    prime_render_formula("\\int_0^1 x^2\\,dx = \\frac{1}{3}", 6, 196, 17,
                         kInk, kBackground, 0, 300);

    render_hint_line();
}

/* 页 1: 公式画廊 (矩阵/极限/求和/括号/希腊字母/高斯分布) */
void render_page_1() {
    prime_render_clear(kBackground);

    prime_render_text("Formula Gallery", 6, 30, 20, kAccent, kBackground, 0);

    prime_render_formula("\\begin{pmatrix} 1 & 2 \\\\ 3 & 4 \\end{pmatrix}",
                         6, 62, 17, kInk, kBackground, 0, 300);
    prime_render_formula(
        "\\lim_{n \\to \\infty} \\left(1 + \\frac{1}{n}\\right)^n = e",
        6, 98, 17, kInk, kBackground, 0, 300);
    prime_render_formula("\\sum_{k=1}^{n} k = \\frac{n(n+1)}{2}", 6, 128,
                         17, kInk, kBackground, 0, 300);
    prime_render_formula("\\left[ \\frac{a}{b} \\right]^2", 6, 156, 17,
                         kInk, kBackground, 0, 300);
    prime_render_formula("\\alpha + \\beta = \\gamma", 6, 184, 17, kInk,
                         kBackground, 0, 300);
    prime_render_formula(
        "f(x) = \\frac{1}{\\sigma \\sqrt{2\\pi}}"
        "\\,e^{-\\frac{(x-\\mu)^2}{2\\sigma^2}}",
        6, 210, 14, kInk, kBackground, 0, 300);

    render_hint_line();
}

/* 渲染一页并返回 0=成功 */
int render_page(int page) {
    if (page == 0) {
        render_page_0();
    } else {
        render_page_1();
    }
    return 0;
}

/* 退出前清屏: 填背景色并送显, 避免屏幕残留演示画面 */
void clear_screen_on_exit() {
    if (prime_render_clear(kBackground) == 0) {
        prime_render_present();
    }
}

}  // namespace

/* 演示页 C API: 渲染第 1 页 (标题+基础公式), 供宿主直接调用 */
extern "C" int prime_render_demo_page(void) {
    render_page_0();
    return 0;
}

/* 固件堆探测: 逐步 malloc/free, 把「能分配多大」打印到日志。
 * 用于验证「立刻重启 = 加载/排版内存不足」这一猜想。 */
static void probe_heap(void)
{
    static const unsigned kb[] = { 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u };
    unsigned i;

    prime_log_write("[MEM] ---- firmware heap probe ----\n");
    for (i = 0; i < sizeof(kb) / sizeof(kb[0]); ++i) {
        void* p = malloc(kb[i] * 1024u);
        prime_log_write("[MEM] malloc ");
        prime_log_int((int)kb[i]);
        prime_log_write("K -> ");
        prime_log_write(p ? "ok\n" : "FAIL\n");
        if (p != nullptr) free(p);
    }
    prime_log_write("[MEM] ---- probe end ----\n");
}

/* ---------- ELF 入口 ----------
 * 最小可用路径: 不经过 nMarkdown 的 run_reader, 直接用
 *   prime_layout_* (布局) + prime_render_* (绘制/送显) + 输入钩子,
 * 保证「能渲染内容 + 能按键退出」。 */
extern "C" void* app_entry(PrimeBootConfig* cfg) {
    /* 日志只走内存环形缓冲, 由 main.py 在 app_entry 返回后回显 */
    prime_log_write("[B] S0 entry\n");
    if (cfg == nullptr || cfg->magic != 0xD00BC760u) {
        prime_log_write("[B] S0 magic FAIL\n");
        return nullptr;
    }

    const PrimeServices* svc = reinterpret_cast<const PrimeServices*>(
        static_cast<std::uintptr_t>(cfg->services));
    prime_set_services(svc);
    prime_log_write("[B] S1 services ok\n");
    probe_heap();                       /* 此时 malloc/free 才可用 */

    prime_set_system_variable(85u /* SYSVAR_SYSPWCTRL */, 2u /* SET */, 1u);
    prime_log_write("[B] S2 sysvar ok\n");

    prime_clear_screen(0);
    prime_log_write("[B] S3 clear ok\n");

    /* 输入钩子: 回调内调用 prime_sys_get_event, 主循环轮询共享状态 */
    if (prime_render_event_thread_start() != 0) {
        prime_log_write("[B] S4 hook install FAIL\n");
    } else {
        prime_log_write("[B] S4 hook installed\n");
    }

    /* 渲染系统 (内含 framebuffer + 字体初始化, 失败返回负值) */
    const int init_rc = prime_render_init();
    prime_log_write("[B] S5 render_init=");
    prime_log_int(init_rc);
    prime_log_write("\n");

    /* 排版内嵌文档 */
    const int open_rc = prime_layout_open(kPrimeDocText,
                                          static_cast<int>(kPrimeDocLen));
    prime_log_write("[B] S6 layout_open=");
    prime_log_int(open_rc);
    prime_log_write("\n");

    if (init_rc == 0 && open_rc == 0) {
        const int page_height = 196;
        const int page_count = prime_layout_page_count(page_height);
        prime_log_write("[B] S6 pages=");
        prime_log_int(page_count);
        prime_log_write("\n");

        static nmarkdown::InputPrime input(svc);
        int page = 0;
        prime_layout_render_page(page, page_height);
        prime_render_present();
        prime_log_write("[B] S7 first page shown\n");

        bool quit = false;
        int idle_ticks = 0;              /* 空闲 60s 自动退出, 保证不会卡死 */
        while (!quit) {
            nmarkdown::InputEvent event;
            if (input.poll(event)) {
                idle_ticks = 0;
                bool redraw = false;
                switch (event.type) {
                case nmarkdown::InputEventType::Quit:
                case nmarkdown::InputEventType::Back:
                    quit = true;
                    break;
                case nmarkdown::InputEventType::ScrollLineDown:
                case nmarkdown::InputEventType::PageDown:
                case nmarkdown::InputEventType::SwipeUp:
                case nmarkdown::InputEventType::Activate:
                    if (page + 1 < page_count) {
                        ++page;
                        redraw = true;
                    }
                    break;
                case nmarkdown::InputEventType::ScrollLineUp:
                case nmarkdown::InputEventType::PageUp:
                case nmarkdown::InputEventType::SwipeDown:
                    if (page > 0) {
                        --page;
                        redraw = true;
                    }
                    break;
                default:
                    break;
                }
                if (redraw) {
                    prime_layout_render_page(page, page_height);
                    prime_render_present();
                }
            } else if (++idle_ticks > 3000) {   /* 3000 x 20ms = 60s */
                prime_log_write("[B] S8 idle timeout\n");
                quit = true;
            }
            prime_sys_sleep(20);
        }

        prime_layout_close();
    }

    prime_render_event_thread_stop();
    prime_render_clear(kBackground);
    prime_render_present();
    prime_log_write("[B] S9 exit\n");
    return prime_log_struct();
}
