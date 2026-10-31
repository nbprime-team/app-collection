#ifndef NMARKDOWN_LAYOUT_PLAIN_TEXT_LAYOUT_H
#define NMARKDOWN_LAYOUT_PLAIN_TEXT_LAYOUT_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nmarkdown/document/search.h"
#include "nmarkdown/document/utf8.h"
#include "nmarkdown/io/random_access.h"
#include "nmarkdown/layout/block_layout.h"

namespace nmarkdown {

class TextSystem;

// Desktop and decoder-backed sources can expose complete contiguous storage.
// Ndless uses a 1 MiB sequential cache because the tested TI-OS allocator
// cannot retain a multi-megabyte novel together with fonts and layout state.
// Only the first 64 KiB is filled before the first screen is shown.
constexpr std::size_t kPlainTextRawCacheBytes = 8U * 1024U * 1024U;
constexpr std::size_t kPlainTextNdlessRawCacheBytes = 1U * 1024U * 1024U;
constexpr std::size_t kPlainTextInitialCacheBytes = 64U * 1024U;
// One idle filesystem quantum must remain shorter than an input frame on the
// slow NAND-backed TI filesystem. Keep refills small enough that a touch
// beginning just after the input poll cannot sit behind a long speculative
// read.
constexpr std::size_t kPlainTextPrefetchBytes = 1U * 1024U;
// A background layout quantum shapes only this much source and retains the
// first completed wrapped line. At the default CJK size this is several lines
// of context, while remaining small enough for one HarfBuzz call between
// input polls on calculator hardware.
constexpr std::size_t kPlainTextLineProbeBytes = 192;
constexpr std::size_t kPlainTextPreviousScreens = 5;
constexpr std::size_t kPlainTextFutureScreens = 10;
// Keep several complete page layouts ready before spending idle time on
// farther speculation. Three pages absorb ordinary touchpad bursts without
// forcing shaping or storage reads into the gesture handler.
constexpr std::size_t kPlainTextMinimumReadyScreens = 3;
constexpr std::size_t kPlainTextGlyphWarmFutureScreens =
    kPlainTextFutureScreens;

struct PlainTextVisibleLine {
    const LayoutLine* line = nullptr;
    int baseline_y = 0;
};

// TXT reader layout. It deliberately has no Markdown parser, DocumentIR, or
// global line/page index. A sequential raw-byte cache is accompanied by a
// bounded deque of shaped screens and recent page-start byte offsets.
class PlainTextLayout {
public:
    bool initialize(std::shared_ptr<RandomAccessData> source,
                    std::uint32_t source_offset,
                    std::uint32_t source_size,
                    const Utf8ValidationResult& sampled_validation,
                    TextSystem& text,
                    const LayoutSignature& signature,
                    int viewport_height,
                    std::string& error);
    void clear();
    bool reconfigure(const LayoutSignature& signature, std::string& error);

    bool loaded() const { return source_ != nullptr; }
    bool empty() const { return source_size_ == 0; }
    std::uint32_t source_size() const { return source_size_; }
    std::uint32_t current_source_offset() const;
    std::uint32_t visible_source_end() const;
    bool at_end() const;
    int approximate_current_page() const {
        return std::max(1, approximate_page_);
    }

    bool move_line(int direction, std::string& error);
    bool move_page(int direction, std::string& error);
    bool scroll_pixels(int pixels, std::string& error);
    bool seek_source(std::uint32_t source_offset, std::string& error);
    bool seek_percentage(unsigned percentage, std::string& error);

    std::vector<PlainTextVisibleLine> visible_lines(std::string& error);
    bool prepare_screen_window(std::string& error);
    std::size_t preload_next_glyphs(std::size_t maximum_glyphs);
    std::size_t preload_deferred_page_glyphs(
        std::size_t maximum_glyphs);
    bool perform_incremental_work(std::string& error);
    std::vector<SearchMatch> search(std::string_view query,
                                    SearchMode mode,
                                    std::size_t maximum_results,
                                    std::string& error) const;
    std::string snippet(std::uint32_t source_offset,
                        std::string& error) const;
    bool initial_cache_contains_cjk() const;

    std::size_t raw_cache_size() const {
        return source_memory_ != nullptr ? source_size_
                                         : raw_cache_valid_size_;
    }
    std::size_t raw_cache_capacity() const {
        return source_memory_ != nullptr ? source_size_
                                         : raw_cache_limit_;
    }
    std::size_t cached_screen_count() const { return screens_.size(); }
    std::size_t previous_screen_count() const {
        return std::min(current_screen_, screens_.size());
    }
    std::size_t future_screen_count() const {
        return current_screen_ < screens_.size()
                   ? screens_.size() - current_screen_ - 1
                   : 0;
    }
    std::size_t pending_screen_line_count() const {
        return pending_screen_.lines.size();
    }
    std::size_t deferred_forward_page_count() const {
        return deferred_forward_pages_;
    }
    std::uint64_t incremental_layout_step_count() const {
        return incremental_layout_steps_;
    }

private:
    struct Screen {
        std::uint32_t start_offset = 0;
        std::uint32_t next_offset = 0;
        std::vector<LayoutLine> lines;
        std::size_t next_preload_line = 0;
        std::size_t next_preload_run = 0;
        std::size_t next_preload_glyph = 0;
        bool eof = false;
    };
    struct PageTarget {
        std::size_t screen = 0;
        std::size_t line = 0;
    };

    bool ensure_cache(std::uint32_t offset,
                      std::size_t minimum_bytes,
                      std::string& error);
    bool prefetch_raw_cache(std::string& error);
    std::string_view cached_view(std::uint32_t offset,
                                 std::size_t maximum_bytes);
    void copy_cached_bytes(std::size_t logical_offset,
                           std::uint8_t* destination,
                           std::size_t size) const;
    void copy_into_cache(std::size_t logical_offset,
                         const std::uint8_t* source,
                         std::size_t size);
    bool build_line(std::uint32_t start_offset,
                    LayoutLine& line,
                    bool& eof,
                    std::string& error);
    void invalidate_probe_lines();
    bool build_screen(std::uint32_t start_offset,
                      Screen& screen,
                      std::string& error);
    bool build_future_screen_step(std::string& error);
    bool ensure_future_screen(std::string& error);
    void reset_pending_screen();
    void reset_auto_page_rows();
    void observe_auto_line_metrics(const LayoutLine& line);
    bool auto_page_fit() const;
    bool find_cached_position(std::uint32_t offset,
                              std::size_t& screen,
                              std::size_t& line) const;
    bool set_cached_position(std::uint32_t offset);
    bool rebuild_at(std::uint32_t offset, std::string& error);
    bool rebuild_previous_region(std::string& error);
    bool advance_position(std::size_t& screen,
                          std::size_t& line,
                          std::string& error);
    bool retreat_position(std::size_t& screen,
                          std::size_t& line,
                          std::string& error);
    void trim_screen_window();
    bool move_page_now(int direction,
                       bool allow_defer,
                       std::string& error);
    bool find_forward_page_target(PageTarget& target,
                                  bool& needs_more_layout) const;
    bool cached_viewport_ready(const PageTarget& target) const;
    bool page_glyphs_preloaded(const PageTarget& target) const;
    bool page_glyphs_cached(const PageTarget& target) const;
    void synchronize_glyph_cache_generation();
    void reset_glyph_preload_cursors();
    void begin_deferred_target(const PageTarget& target);
    void rewind_deferred_warm_cursor();
    void finish_deferred_warm_pass();
    void queue_forward_page();
    void reset_deferred_target();
    std::uint32_t line_offset(std::size_t screen, std::size_t line) const;
    std::uint32_t line_end(std::size_t screen, std::size_t line) const;
    std::uint32_t align_to_line_start(std::uint32_t offset,
                                      std::string& error) const;
    bool read_logical(std::uint32_t offset,
                      std::uint8_t* data,
                      std::size_t size) const;

    std::shared_ptr<RandomAccessData> source_;
    const std::uint8_t* source_memory_ = nullptr;
    std::uint32_t source_offset_ = 0;
    std::uint32_t source_size_ = 0;
    Utf8ValidationResult sampled_validation_{};
    TextSystem* text_ = nullptr;
    LayoutSignature signature_{};
    int viewport_height_ = 0;
    std::size_t auto_rows_per_page_ = 0;

    std::vector<std::uint8_t> raw_cache_;
    std::vector<std::uint8_t> raw_cache_scratch_;
    std::uint32_t raw_cache_offset_ = 0;
    std::size_t raw_cache_head_ = 0;
    std::size_t raw_cache_valid_size_ = 0;
    std::size_t raw_cache_limit_ = 0;
    std::deque<Screen> screens_;
    Screen pending_screen_;
    int pending_screen_height_ = 0;
    bool pending_screen_active_ = false;
    std::size_t current_screen_ = 0;
    std::size_t current_line_ = 0;
    bool bottom_align_visible_rows_ = true;
    std::deque<std::uint32_t> page_start_offsets_;
    int approximate_page_ = 1;
    int pixel_scroll_remainder_ = 0;
    std::size_t deferred_forward_pages_ = 0;
    PageTarget deferred_target_;
    bool deferred_target_valid_ = false;
    bool deferred_target_glyphs_ready_ = false;
    std::size_t deferred_warm_screen_ = 0;
    std::size_t deferred_warm_line_ = 0;
    std::size_t deferred_warm_run_ = 0;
    std::size_t deferred_warm_glyph_ = 0;
    int deferred_warm_height_ = 0;
    std::size_t deferred_warm_rows_ = 0;
    std::uint64_t deferred_warm_start_evictions_ = 0;
    bool deferred_warm_failed_ = false;
    std::uint8_t deferred_warm_retry_count_ = 0;
    std::uint64_t incremental_layout_steps_ = 0;
    std::uint64_t glyph_cache_clear_generation_ = 0;
    bool prefetch_raw_next_ = false;
    // One build_line() probe shapes several wrapped lines through HarfBuzz;
    // only the first used to be kept. The remaining sealed lines are cached
    // here and served to the naturally sequential next calls, so cold screens
    // and pages need roughly one shaping pass per screen instead of one per
    // line. A hit requires an exact start-offset match; content is immutable
    // and the cache is dropped whenever the layout signature changes.
    std::vector<LayoutLine> probe_lines_;
    std::size_t probe_next_ = 0;
    std::uint32_t probe_next_offset_ = 0;
    bool probe_valid_ = false;
};

}  // namespace nmarkdown

#endif
