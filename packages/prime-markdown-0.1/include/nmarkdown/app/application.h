#ifndef NMARKDOWN_APP_APPLICATION_H
#define NMARKDOWN_APP_APPLICATION_H

#include <cstddef>
#include <string>

#include "nmarkdown/document/state.h"
#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

struct ReaderOptions {
    std::size_t maximum_source_bytes = 4U * 1024U * 1024U;
    // Stdio-backed fonts are random-access streams, so these are on-disk
    // admission limits rather than retained-heap budgets. Platforms with less
    // headroom may override them independently.
    std::size_t maximum_font_bytes = 20U * 1024U * 1024U;
    std::size_t maximum_external_font_bytes = 20U * 1024U * 1024U;
    // Total bytes of external font payloads that may be promoted from a
    // random-access stream into a retained RAM buffer. A resident face skips
    // per-glyph storage reads entirely, which is the dominant cold-page cost
    // on NAND-backed calculators. Promotion is best-effort: it happens only
    // when the payload fits this budget, the allocation succeeds, and a heap
    // reserve remains afterwards; otherwise the face keeps streaming. Zero
    // disables promotion.
    std::size_t maximum_resident_font_bytes = 0;
    // Promotion is attempted only when the document being opened (or already
    // open) is at least this large: a small file finishes its handful of cold
    // glyph loads faster than one multi-megabyte upfront font read, while a
    // long document repays that read many times over. Zero promotes whenever
    // the budget allows, regardless of document size.
    std::uint64_t minimum_resident_font_document_bytes = 0;
    bool persist_state = true;
    bool open_browser_on_empty_path = false;
    ReadingMode initial_reading_mode = ReadingMode::VerticalScroll;
    std::string document_root = ".";
    std::string initial_body_font_path;
    std::string initial_body_italic_font_path;
    std::string initial_monospace_font_path;
    std::string initial_cjk_font_path;
};

int run_reader(Display& display,
               Input& input,
               FileSystem& files,
               Clock& clock,
               const char* document_path,
               ReaderOptions options = {});

}  // namespace nmarkdown

#endif
