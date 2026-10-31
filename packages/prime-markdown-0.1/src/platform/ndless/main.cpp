#include <libndls.h>

#include <cstdio>
#include <string>

#include "clock_ndless.h"
#include "display_ndless.h"
#include "input_ndless.h"
#include "nmarkdown/app/application.h"
#include "nmarkdown/platform/platform.h"

#if defined(NMARKDOWN_FIREBIRD_FONT_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
#include "firebird_font_fixture.h"
#endif

#if defined(NMARKDOWN_FIREBIRD_SYMBOL_GALLERY_FIXTURE)
#include "math_symbol_gallery_fixture.h"
#endif

namespace {

#if defined(NMARKDOWN_FIREBIRD_INTEGRATION)

#if defined(NMARKDOWN_FIREBIRD_MEMORY_PROFILE_FIXTURE)
constexpr char kMemoryProfileDocumentPath[] =
    "zzzz-nmarkdown-memory-book.txt.tns";
constexpr char kMemoryProfileFontPath[] =
    "zzzz-nmarkdown-memory-cjk.ttf.tns";

bool memory_profile_assets_ready() {
    FILE* document = std::fopen(kMemoryProfileDocumentPath, "rb");
    if (document == nullptr) return false;
    std::fclose(document);
    FILE* font = std::fopen(kMemoryProfileFontPath, "rb");
    if (font == nullptr) return false;
    std::fclose(font);
    return true;
}
#endif

#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
constexpr char kRealNovelDocumentPath[] =
    "/documents/ndless/红楼梦.txt.tns";
constexpr char kRealNovelFontPath[] =
    "/documents/ndless/fusion-pixel-12px-proportional-zh_hans.ttf.tns";
constexpr long kRealNovelDocumentBytes = 2622979L;
constexpr long kRealNovelFontBytes = 7012032L;

bool file_has_exact_size(const char* path, long expected) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    const bool seek_ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = seek_ok ? std::ftell(file) : -1L;
    const bool close_ok = std::fclose(file) == 0;
    return seek_ok && close_ok && size == expected;
}

bool real_novel_assets_ready() {
    return file_has_exact_size(
               kRealNovelDocumentPath, kRealNovelDocumentBytes) &&
           file_has_exact_size(kRealNovelFontPath, kRealNovelFontBytes);
}
#endif

constexpr char kIntegrationDocumentPath[] =
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    "/documents/ndless/红楼梦.txt.tns";
#elif defined(NMARKDOWN_FIREBIRD_STATE_FIXTURE)
    "/documents/nmarkdown-state-save.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
    "/documents/nmarkdown-font-menu.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_THEME_FIXTURE)
    "/documents/nmarkdown-theme.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_TOC_FIXTURE)
    "/documents/nmarkdown-toc-jumps.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE)
    "/documents/nmarkdown-format-gallery.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_PAGE_FIXTURE)
    "/documents/nmarkdown-page-mode.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_SYMBOL_GALLERY_FIXTURE)
    "/documents/nmarkdown-math-symbol-gallery.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_OVERSIZED_FORMULA_FIXTURE)
    "/documents/nmarkdown-oversized-formula.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_SCROLL_SWIPE_FIXTURE)
    "/documents/nmarkdown-scroll-swipe.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_IMATH_FIXTURE)
    "/documents/nmarkdown-bold-italic-ij.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_MATH_REVIEW_FIXTURE)
    "/documents/nmarkdown-math-review.md.tns";
#elif defined(NMARKDOWN_FIREBIRD_MATH_FIXTURE)
    "/documents/nmarkdown-math.md.tns";
#else
    "/documents/nmarkdown-firebird.md.tns";
#endif

#if defined(NMARKDOWN_FIREBIRD_STATE_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# State persistence on CX II\n"
    "\n"
    "This fixture saves settings, replaces the adjacent state file, and exits.\n";
#elif defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
constexpr char kIntegrationFontPath[] =
    "/documents/SarasaFixedSC-MenuFixture.ttf.tns";

constexpr char kIntegrationDocument[] =
    "# CJK font picker on CX II\n"
    "\n"
    "The harness opens Settings, enters Fonts and CJK, then backtracks and "
    "reopens the file list without loading the font.\n";
#elif defined(NMARKDOWN_FIREBIRD_THEME_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Dark theme transition\n"
    "\n"
    "The settings overlay must repaint without rebuilding document layout.\n"
    "\n"
    "Inline *italic*, `monospace`, and $x^2+y^2=z^2$ exercise each renderer.\n"
    "\n"
    "This paragraph keeps the document scrollable after the theme changes. "
    "Input remains live when the harness closes settings and presses Down.\n"
    "\n"
    "## Responsive after repaint\n"
    "\n"
    "Reopening settings in dark mode proves the transition completed.\n";
#elif defined(NMARKDOWN_FIREBIRD_TOC_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Contents start\n"
    "\n"
    "The first screen establishes the initial document position.\n"
    "\n"
    "## Alpha destination\n"
    "\n"
    "Alpha paragraph one fills the first destination with wrapping text.\n"
    "\n"
    "Alpha paragraph two keeps the following heading outside this viewport.\n"
    "\n"
    "Alpha paragraph three makes this a real lazy-layout jump target.\n"
    "\n"
    "Alpha paragraph four separates the two destinations on the screen.\n"
    "\n"
    "Alpha paragraph five provides another measured block before the jump.\n"
    "\n"
    "Alpha paragraph six completes the section used by the first jump.\n"
    "\n"
    "## Beta destination\n"
    "\n"
    "Beta is the second table-of-contents jump in the same app session.\n"
    "\n"
    "The harness reopens and closes the table of contents after arriving here.\n"
    "\n"
    "## Final reference\n"
    "\n"
    "This final heading keeps another selectable entry after Beta.\n";
#elif defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE)
constexpr char kIntegrationFontPath[] =
    "/documents/nmarkdown-format-gallery.ttf.tns";
constexpr char kIntegrationBodyFontPath[] =
    "/documents/nmarkdown-format-body.ttf.tns";
constexpr char kIntegrationItalicFontPath[] =
    "/documents/nmarkdown-format-italic.ttf.tns";
constexpr char kIntegrationMonoFontPath[] =
    "/documents/nmarkdown-format-mono.ttf.tns";

constexpr char kIntegrationDocument[] =
    "# Format gallery on CX II\n"
    "\n"
    "Plain **bold**, *italic*, and `inline code`.\n"
    "\n"
    "中文排版测试，日本語かなカナ。\n"
    "\n"
    "```text\n"
    "grid[02] = 320x240;\n"
    "```\n"
    "\n"
    "Inline: $a+\\sqrt{x^2+1}=b$.\n"
    "\n"
    "$$\\sqrt{\\frac{x^2+y^2}{1+\\sqrt{z+1}}}$$\n";
#elif defined(NMARKDOWN_FIREBIRD_PAGE_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Horizontal Scroll verification\n"
    "\n"
    "This screen demonstrates context-preserving navigation on the calculator. "
    "The header keeps the filename unobstructed across its available width.\n"
    "\n"
    "This second paragraph fills the first screen with enough text to make the "
    "next screen step a real layout boundary rather than an empty frame.\n"
    "\n"
    "## Next screen boundary\n"
    "\n"
    "Tab or an upward swipe advances while retaining visual context. The next "
    "frame starts at a complete line instead of clipping a heading.\n"
    "\n"
    "The progress strip remains two pixels high and follows the continuous "
    "reading position without displaying a page count.\n"
    "\n"
    "## Final section\n"
    "\n"
    "Additional content verifies that lazy layout growth still reaches the exact "
    "end of the document.\n";
#elif defined(NMARKDOWN_FIREBIRD_SYMBOL_GALLERY_FIXTURE)
// The exact Markdown bytes come from samples/math-symbol-gallery.md through
// the generated math_symbol_gallery_fixture.h header.
#elif defined(NMARKDOWN_FIREBIRD_OVERSIZED_FORMULA_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Oversized formula interaction\n"
    "\n"
    "The formula below is visible with this prose and opens as a pannable canvas.\n"
    "\n"
    "$$\n"
    "\\begin{align}\n"
    "    v + w & = 0  & \\text{Given} \\tag 1 \\\\\n"
    "       -w & = -w + 0 & \\text{additive identity} \\tag 2 \\\\\n"
    "   -w + 0 & = -w + (v + w) & \\text{equations $(1)$ and $(2)$} \\\\\n"
    "\\end{align}\n"
    "$$\n"
    "\n"
    "The reader remains responsive after local formula panning.\n";
#elif defined(NMARKDOWN_FIREBIRD_SCROLL_SWIPE_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Scroll swipe direction\n"
    "\n"
    "This long document verifies direct horizontal swipes in Scroll mode.\n"
    "\n"
    "## First segment\n"
    "\n"
    "Line 01 keeps the beginning distinct and gives the first screen room.\n"
    "\n"
    "Line 02 provides ordinary prose with predictable vertical flow.\n"
    "\n"
    "Line 03 remains before the middle of the fixture.\n"
    "\n"
    "Line 04 keeps the document taller than a single viewport.\n"
    "\n"
    "Line 05 gives the earlier-direction swipe content to reveal.\n"
    "\n"
    "## Middle segment\n"
    "\n"
    "Line 06 starts the central region of the scrolling fixture.\n"
    "\n"
    "Line 07 makes a later-direction swipe visibly change the frame.\n"
    "\n"
    "Line 08 retains enough content above and below the viewport.\n"
    "\n"
    "Line 09 prevents the first full step from reaching the document end.\n"
    "\n"
    "Line 10 continues with a separate layout block.\n"
    "\n"
    "## Later segment\n"
    "\n"
    "Line 11 begins content reached by a rightward swipe.\n"
    "\n"
    "Line 12 leaves room for the final liveness key.\n"
    "\n"
    "Line 13 confirms that scrolling did not stall after touch input.\n"
    "\n"
    "Line 14 keeps the final position away from the lower clamp.\n"
    "\n"
    "Line 15 provides another stable paragraph for framebuffer capture.\n"
    "\n"
    "## Final segment\n"
    "\n"
    "Line 16 is additional tail content.\n"
    "\n"
    "Line 17 keeps the fixture safely longer than three screens.\n"
    "\n"
    "Line 18 completes the Scroll-mode swipe document.\n";
#elif defined(NMARKDOWN_FIREBIRD_IMATH_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Bold italic i / j review\n"
    "\n"
    "Ordinary italic variables: $i \\qquad j$\n"
    "\n"
    "Bold italic commands: $\\imath \\qquad \\jmath$\n"
    "\n"
    "Accented command forms:\n"
    "\n"
    "$$\\hat{\\imath} \\quad \\hat{\\jmath} \\quad "
    "\\vec{\\imath} \\quad \\vec{\\jmath}$$\n";
#elif defined(NMARKDOWN_FIREBIRD_MATH_REVIEW_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Complex math on CX II\n"
    "\n"
    "$$\\begin{aligned}"
    "q_{+}&=\\frac{-p+\\sqrt{p^2-4qr}}{2q}\\\\"
    "q_{-}&=\\frac{-p-\\sqrt{p^2-4qr}}{2q}"
    "\\end{aligned}$$\n"
    "\n"
    "$$A^{-1}\\mathbf{b}=\\frac{1}{ad-bc}"
    "\\begin{pmatrix}d&-b\\\\-c&a\\end{pmatrix}"
    "\\begin{pmatrix}b_1\\\\b_2\\end{pmatrix}$$\n";
#elif defined(NMARKDOWN_FIREBIRD_MATH_FIXTURE)
constexpr char kIntegrationDocument[] =
    "# Math layout on CX II\n"
    "\n"
    "Inline: $x_i^2 + \\sqrt{y} = z$\n"
    "\n"
    "$$\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}\\qquad"
    "\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}"
    "\\begin{pmatrix}x_1\\\\x_2\\end{pmatrix}\\qquad"
    "\\left\\{\\frac{p}{q}\\right\\}$$\n";
#elif defined(NMARKDOWN_FIREBIRD_FONT_FIXTURE)
constexpr char kIntegrationFontPath[] =
    "/documents/nmarkdown-firebird-font.ttf.tns";

constexpr char kIntegrationDocument[] =
    "# Monospace + CJK on CX II\n"
    "\n"
    "Body fallback: 中文排版测试，日本語かなカナの表示。\n"
    "\n"
    "Inline code: `const width = 320;`\n"
    "\n"
    "```text\n"
    "grid[02] = 320x240;\n"
    "中文：标点换行，日本語かなカナ：句読点。\n"
    "```\n"
    "\n"
    "> Sarasa Fixed SC was loaded directly as the monospace font.\n";
#else
constexpr char kIntegrationDocument[] =
    "# nMarkdown Firebird verification\n"
    "\n"
    "This raw **Markdown** file was created on the emulated calculator and "
    "opened directly.\n"
    "\n"
    "- UTF-8: alpha beta gamma = α β γ\n"
    "- Cyrillic: Привет\n"
    "- Math: $x^2 + y^2 = z^2$\n"
    "\n"
    "> The ARM parser, layout engine, font rasterizer, RGB565 compositor, "
    "and lcd_blit path produced this frame.\n"
    "\n"
    "| Capability | State |\n"
    "| --- | --- |\n"
    "| Direct .md | active |\n"
    "| Font packs | separate |\n";
#endif

#if !defined(NMARKDOWN_FIREBIRD_BROWSER_CANCEL_FIXTURE) && \
    !defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
const char* prepare_integration_document() {
    FILE* file = std::fopen(kIntegrationDocumentPath, "wb");
    if (file == nullptr) {
        return nullptr;
    }
#if defined(NMARKDOWN_FIREBIRD_SYMBOL_GALLERY_FIXTURE)
    const std::uint8_t* document = kMathSymbolGalleryFixture;
    const std::size_t size = kMathSymbolGalleryFixtureSize;
#else
    const char* document = kIntegrationDocument;
    const std::size_t size = sizeof(kIntegrationDocument) - 1;
#endif
    const bool wrote = std::fwrite(document, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    return wrote && closed ? kIntegrationDocumentPath : nullptr;
}
#endif

#if defined(NMARKDOWN_FIREBIRD_FONT_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
#if !defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE)
const char* prepare_integration_font() {
    FILE* file = std::fopen(kIntegrationFontPath, "wb");
    if (file == nullptr) return nullptr;
    const bool wrote = std::fwrite(kFirebirdFontFixture, 1,
                                   kFirebirdFontFixtureSize, file) ==
                       kFirebirdFontFixtureSize;
    const bool closed = std::fclose(file) == 0;
    return wrote && closed ? kIntegrationFontPath : nullptr;
}
#endif

#if defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE)
const char* prepare_format_font(const char* path,
                                const std::uint8_t* bytes,
                                std::size_t size) {
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return nullptr;
    const bool wrote = std::fwrite(bytes, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    return wrote && closed ? path : nullptr;
}
#endif
#endif

void integration_log(const char* message) {
    std::printf("NMARKDOWN_IT/1 %s\n", message);
    std::fflush(stdout);
}

#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(NMARKDOWN_FIREBIRD_INTEGRATION)
    integration_log("ENTER_MAIN");
#endif

    enable_relative_paths(argv);

#if defined(NMARKDOWN_FIREBIRD_INTEGRATION)
    integration_log("RELATIVE_PATHS_READY");
#endif

    if (argc > 0 && argv[0] != nullptr && argv[0][0] != '\0') {
        std::string program_name(argv[0]);
        const std::size_t slash = program_name.find_last_of("/\\");
        if (slash != std::string::npos) program_name.erase(0, slash + 1);
        if (program_name.size() > 4 &&
            program_name.compare(program_name.size() - 4, 4, ".tns") == 0) {
            program_name.resize(program_name.size() - 4);
        }
        cfg_register_fileext("md", program_name.c_str());
        cfg_register_fileext("markdown", program_name.c_str());
        cfg_register_fileext("txt", program_name.c_str());
    }

    const char* document_path = argc > 1 ? argv[1] : nullptr;
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    // This diagnostic must never inherit an arbitrary file-association
    // argument. Its evidence is valid only for the exact transferred novel.
    document_path = kRealNovelDocumentPath;
#endif
#if defined(NMARKDOWN_FIREBIRD_INTEGRATION)
#if defined(NMARKDOWN_FIREBIRD_BROWSER_CANCEL_FIXTURE)
    // Leave document_path empty so run_reader opens its startup file browser.
    integration_log("FIXTURE_READY");
#else
    if (document_path == nullptr) {
#if defined(NMARKDOWN_FIREBIRD_MEMORY_PROFILE_FIXTURE)
        document_path = kMemoryProfileDocumentPath;
#elif defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        document_path = kRealNovelDocumentPath;
#else
        document_path = prepare_integration_document();
#endif
    }
    integration_log(document_path == nullptr ? "FIXTURE_FAIL" : "FIXTURE_READY");
#if defined(NMARKDOWN_FIREBIRD_OVERSIZED_FORMULA_FIXTURE)
    if (document_path != nullptr) integration_log("OVERSIZED_FORMULA_READY");
#endif
#if defined(NMARKDOWN_FIREBIRD_SCROLL_SWIPE_FIXTURE)
    if (document_path != nullptr) integration_log("SCROLL_SWIPE_READY");
#endif
#if defined(NMARKDOWN_FIREBIRD_IMATH_FIXTURE)
    if (document_path != nullptr) integration_log("IMATH_FIXTURE_READY");
#endif
#if defined(NMARKDOWN_FIREBIRD_SYMBOL_GALLERY_FIXTURE)
    if (document_path != nullptr) {
        std::printf(
            "NMARKDOWN_IT/1 SYMBOL_GALLERY_EXACT bytes=%lu "
            "fnv1a64=%016llx sha256=%s\n",
            static_cast<unsigned long>(kMathSymbolGalleryFixtureSize),
            static_cast<unsigned long long>(
                kMathSymbolGalleryFixtureFnv1a64),
            kMathSymbolGalleryFixtureSha256);
        std::fflush(stdout);
        integration_log("SYMBOL_GALLERY_READY");
    }
#endif
#endif
#endif
#if defined(NMARKDOWN_FIREBIRD_MEMORY_PROFILE_FIXTURE)
    integration_log("MEMORY_PROFILE_ASSET_WAIT");
    bool memory_profile_ready = false;
    for (unsigned int attempt = 0; attempt < 12000U; ++attempt) {
        if (memory_profile_assets_ready()) {
            memory_profile_ready = true;
            break;
        }
        msleep(10);
    }
    integration_log(memory_profile_ready ? "MEMORY_PROFILE_ASSETS_READY"
                                         : "MEMORY_PROFILE_ASSETS_TIMEOUT");
    if (!memory_profile_ready) return 1;
#endif
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    integration_log("REAL_NOVEL_ASSET_WAIT");
    bool real_novel_ready = false;
    for (unsigned int attempt = 0; attempt < 12000U; ++attempt) {
        if (real_novel_assets_ready()) {
            real_novel_ready = true;
            break;
        }
        msleep(10);
    }
    integration_log(real_novel_ready ? "REAL_NOVEL_ASSETS_READY"
                                     : "REAL_NOVEL_ASSETS_TIMEOUT");
    if (!real_novel_ready) return 1;
#endif
    nmarkdown::ClockNdless clock;
#if defined(NMARKDOWN_FIREBIRD_INTEGRATION)
    integration_log(clock.has_hardware_timer() ? "CLOCK_HARDWARE"
                                               : "CLOCK_LOGICAL");
#endif
    nmarkdown::DisplayNdless display;
    nmarkdown::InputNdless input(clock);
    nmarkdown::StdioFileSystem files;
    nmarkdown::ReaderOptions options;
    // The classic CX and the CX II carry the same 64 MiB of SDRAM; only the
    // CPU clock differs, and the per-clock calibrations live in the clock and
    // display adapters. The old smaller classic-CX budgets were a placeholder
    // for an unprofiled heap, which runtime guards now cover on both models:
    // every admission path reports allocation failure cleanly, and resident
    // font promotion is additionally gated by an explicit working-set heap
    // reserve probe with silent fallback to streaming. Keep one budget set so
    // the two models only ever differ by what their live heap can prove.
    options.maximum_source_bytes = 8U * 1024U * 1024U;
    options.maximum_font_bytes = 20U * 1024U * 1024U;
    // External Stdio faces open as streams; faces within the resident budget
    // are then promoted into RAM so cold glyph loads stop paying per-read
    // NAND latency.
    options.maximum_external_font_bytes = 20U * 1024U * 1024U;
    options.maximum_resident_font_bytes = 12U * 1024U * 1024U;
    // Promote only for documents large enough to repay the one-time
    // sequential font read. Around a quarter megabyte of CJK prose the
    // cumulative per-glyph NAND reads of streaming overtake reading a whole
    // subsetted CJK face once; smaller files open faster with streaming.
    // Fonts still upgrade automatically when a large document is opened
    // later in the session.
    options.minimum_resident_font_document_bytes = 256U * 1024U;
#if defined(NMARKDOWN_FIREBIRD_INTEGRATION) && \
    !defined(NMARKDOWN_FIREBIRD_STATE_FIXTURE)
    options.persist_state = false;
#endif
#if defined(NMARKDOWN_FIREBIRD_PAGE_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    options.initial_reading_mode = nmarkdown::ReadingMode::HorizontalScroll;
#endif
#if defined(NMARKDOWN_FIREBIRD_SYMBOL_GALLERY_FIXTURE)
    options.initial_reading_mode = nmarkdown::ReadingMode::HorizontalScroll;
#endif
    options.open_browser_on_empty_path = true;
    options.document_root = get_documents_dir();
#if defined(NMARKDOWN_FIREBIRD_FONT_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE) || \
    defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
#if defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE)
    const char* integration_body_font_path = prepare_format_font(
        kIntegrationBodyFontPath, kFirebirdBodyFontFixture,
        kFirebirdBodyFontFixtureSize);
    const char* integration_italic_font_path = prepare_format_font(
        kIntegrationItalicFontPath, kFirebirdItalicFontFixture,
        kFirebirdItalicFontFixtureSize);
    const char* integration_mono_font_path = prepare_format_font(
        kIntegrationMonoFontPath, kFirebirdMonoFontFixture,
        kFirebirdMonoFontFixtureSize);
    const char* integration_font_path = prepare_format_font(
        kIntegrationFontPath, kFirebirdFontFixture,
        kFirebirdFontFixtureSize);
#else
    const char* integration_font_path = prepare_integration_font();
#endif
#if defined(NMARKDOWN_FIREBIRD_FORMAT_FIXTURE)
    // The legacy explicit-path format fixture verifies the renderer's internal
    // face roles. Production exposes them as three family selections. Only the
    // ASCII UI bootstrap and Latin Modern Math remain embedded.
    options.initial_body_font_path = integration_body_font_path == nullptr
                                         ? "/documents/missing-body.ttf.tns"
                                         : integration_body_font_path;
    options.initial_body_italic_font_path =
        integration_italic_font_path == nullptr
            ? "/documents/missing-italic.ttf.tns"
            : integration_italic_font_path;
    options.initial_monospace_font_path = integration_mono_font_path == nullptr
                                              ? "/documents/missing-mono.ttf.tns"
                                              : integration_mono_font_path;
    options.initial_cjk_font_path = integration_font_path == nullptr
                                        ? "/documents/missing-font.ttf.tns"
                                        : integration_font_path;
#elif defined(NMARKDOWN_FIREBIRD_FONT_FIXTURE)
    options.initial_monospace_font_path = integration_font_path == nullptr
                                              ? "/documents/missing-font.ttf.tns"
                                              : integration_font_path;
#else
    // The menu fixture intentionally leaves the CJK family unloaded initially.
    // Discovery reads only metadata; the harness later selects the family to
    // exercise payload loading and session-catalog reuse.
    integration_log(integration_font_path == nullptr
                        ? "FONT_MENU_FILE_FAIL"
                        : "FONT_MENU_FILE_READY");
#endif
#endif
#if defined(NMARKDOWN_FIREBIRD_MEMORY_PROFILE_FIXTURE)
    options.initial_cjk_font_path = kMemoryProfileFontPath;
#endif
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    options.initial_cjk_font_path = kRealNovelFontPath;
#endif

    const int result = nmarkdown::run_reader(display,
                                             input,
                                             files,
                                             clock,
                                             document_path,
                                             options);
#if defined(NMARKDOWN_FIREBIRD_INTEGRATION)
    integration_log(result == 0 ? "EXIT_OK" : "EXIT_ERROR");
#endif
    return result;
}
