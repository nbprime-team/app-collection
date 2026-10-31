/*
 * prime_layout.cpp — 原版 block_layout 引擎的 C 适配
 * ====================================================
 * 复用 VirtualDocumentLayout + MarkdownDocument (md4c 解析)。
 * 渲染: layout_window(top, viewport_height, 0) 取可见块,
 * 逐 LayoutLine/LayoutRun 调 TextSystem::draw_run / MathSystem::draw,
 * 样式映射与原版 viewer 一致:
 *   Strong/Heading → TextSynthesisBold; Emphasis → TextSynthesisItalic
 *   Link → 下划线 (baseline+2); Strikethrough → 删除线 (baseline-size/3)
 *   Code → 浅灰背景
 * 浅色主题: ink/paper/accent 与示例 md 配色一致。
 */
#include "prime_layout.h"

#include <cstring>
#include <string>
#include <vector>

#include "nmarkdown/document/markdown.h"
#include "nmarkdown/layout/block_layout.h"
#include "nmarkdown/math/math_system.h"
#include "nmarkdown/render/primitives.h"
#include "nmarkdown/render/surface565.h"
#include "nmarkdown/text/text_system.h"

#include "prime_log.h"
#include "prime_platform.h"
#include "prime_render_api.h"


namespace {

using namespace nmarkdown;

constexpr std::uint16_t kInk = rgb565(30, 30, 30);
constexpr std::uint16_t kPaper = rgb565(255, 255, 255);
constexpr std::uint16_t kAccent = rgb565(20, 80, 180);
constexpr std::uint16_t kCodeBg = rgb565(238, 238, 238);
constexpr std::uint16_t kMuted = rgb565(120, 120, 120);

/* 页面几何 (与 prime_upsilon_main 的 kPageTop/kPageBottom/kMargin 一致) */
constexpr int kPageTop = 24;
constexpr int kMargin = 10;

TextSystem* g_text = nullptr;
MathSystem* g_math = nullptr;
DisplayPrime* g_display = nullptr;

/* ⚠️ 引导器（generic_loader / runelf 的 main.py）是**直接跳 app_entry**，
 * 不会运行 .init_array —— 因此全局对象（MarkdownDocument / VirtualDocumentLayout /
 * std::string）的构造函数永远不会执行，访问即崩溃。
 * 改为「函数内 static」：首次调用时构造（C++ 保证），不依赖 .init_array。 */
static MarkdownDocument& g_doc_inst() { static MarkdownDocument d; return d; }
static VirtualDocumentLayout& g_layout_inst() { static VirtualDocumentLayout l; return l; }
static std::string& g_error_inst() { static std::string e; return e; }
bool g_open = false;

}  // namespace

int prime_layout_open(const char* md_text, int md_len) {
    if (md_text == nullptr) return -1;
    const int len = md_len > 0 ? md_len
                               : static_cast<int>(std::strlen(md_text));
    PrimeRenderShared sh;
    if (prime_render_shared(&sh) != 0) {
        return -2; /* 渲染系统未初始化 */
    }
    g_text = static_cast<TextSystem*>(sh.text);
    g_math = static_cast<MathSystem*>(sh.math);
    g_display = static_cast<DisplayPrime*>(sh.display);
    if (g_text == nullptr || !g_text->ready()) {
        return -3;
    }
    prime_layout_close();

    /* md4c 需要 NUL 结尾? parse_markdown 接受 bytes+size, 无需 NUL */
    if (!parse_markdown(reinterpret_cast<const std::uint8_t*>(md_text),
                        static_cast<std::size_t>(len), g_doc_inst(), g_error_inst())) {
        prime_log_write("md parse err\n");
        return -4;
    }
    LayoutSignature signature;
    signature.content_width = 300; /* kContentW = 320 - 2*10 */
    signature.body_px = 14;
    signature.font_pack_version = 1;
    signature.code_wrap = 1;
    if (!g_layout_inst().initialize(g_doc_inst(), *g_text, signature, g_error_inst(), g_math)) {
        prime_log_write("layout init err\n");
        return -5;
    }
    g_open = true;
    return 0;
}

int prime_layout_total_height(void) {
    return g_open ? fx_ceil(g_layout_inst().total_height()) : 0;
}

int prime_layout_page_count(int page_height) {
    if (!g_open || page_height <= 0) return 1;
    const int total = fx_ceil(g_layout_inst().total_height());
    return total <= 0 ? 1 : (total + page_height - 1) / page_height;
}

int prime_layout_render_page(int page, int page_height) {
    if (!g_open || g_display == nullptr) return -1;
    if (page < 0) page = 0;
    const int page_y = page * page_height;

    Surface565 surface = g_display->surface();
    const Rect clip = surface.bounds();
    fill_rect(surface, clip, kPaper, clip); /* 清屏 */

    std::vector<VisibleBlock> visible =
        g_layout_inst().layout_window(fx_from_int(page_y),
                               fx_from_int(page_height), 0);

    for (const VisibleBlock& vb : visible) {
        const BlockLayout* bl = vb.layout;
        if (bl == nullptr) continue;
        const bool heading = bl->kind == BlockKind::Heading;
        const std::uint16_t fg = heading ? kAccent : kInk;
        const int block_y =
            kPageTop - page_y + fx_floor(vb.document_y);
        for (const LayoutLine& line : bl->lines) {
            const int baseline =
                block_y + fx_floor(line.baseline_y);
            for (const LayoutRun& run : line.runs) {
                const int run_x = kMargin + fx_floor(run.x);
                if (run.math != nullptr) {
                    g_math->draw(surface, *run.math, run_x, baseline, 0,
                                 kInk, kPaper, false, clip);
                    continue;
                }
                std::uint8_t synth = TextSynthesisNone;
                if ((run.style_flags & InlineStyleStrong) != 0) {
                    synth |= TextSynthesisBold;
                }
                if ((run.style_flags & InlineStyleEmphasis) != 0) {
                    synth |= TextSynthesisItalic;
                }
                const std::uint16_t bg =
                    (run.style_flags & InlineStyleCode) != 0 ? kCodeBg
                                                             : kPaper;
                if (!g_text->draw_run(surface, run.glyphs, run_x, baseline,
                                      run.pixel_size, fg, bg, false, true,
                                      clip, synth)) {
                    continue;
                }
                const int run_w = fx_ceil(run.glyphs.width);
                if ((run.style_flags & InlineStyleLink) != 0) {
                    fill_rect(surface,
                              {run_x, baseline + 2, run_w, 1}, kAccent,
                              clip);
                }
                if ((run.style_flags & InlineStyleStrikethrough) != 0) {
                    fill_rect(surface,
                              {run_x, baseline -
                                          fx_ceil(run.pixel_size) / 3,
                               run_w, 1},
                              fg, clip);
                }
            }
        }
    }
    return 0;
}

void prime_layout_close(void) {
    g_layout_inst().clear();
    g_doc_inst().clear();
    g_open = false;
}
