#include "nmarkdown/layout/plain_text_layout.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "nmarkdown/document/utf8.h"
#include "nmarkdown/text/text_system.h"

namespace nmarkdown {
namespace {

constexpr std::size_t kSearchChunkBytes = 64U * 1024U;
constexpr std::size_t kSearchBoundaryOverlap = 512;
constexpr std::size_t kMaximumPageStartOffsets = 32;
constexpr std::uint32_t kPreviousRegionProbeBytes = 4096;
constexpr int kMostlyVisiblePercent = 85;

bool continuation(std::uint8_t value) {
    return (value & 0xC0U) == 0x80U;
}

std::size_t complete_utf8_prefix(std::string_view value) {
    if (value.empty()) return 0;
    std::size_t lead = value.size() - 1;
    std::size_t continuation_count = 0;
    while (lead > 0 &&
           continuation(static_cast<std::uint8_t>(value[lead])) &&
           continuation_count < 3) {
        --lead;
        ++continuation_count;
    }
    const std::uint8_t first = static_cast<std::uint8_t>(value[lead]);
    std::size_t expected = 1;
    if (first >= 0xC2U && first <= 0xDFU) expected = 2;
    else if (first >= 0xE0U && first <= 0xEFU) expected = 3;
    else if (first >= 0xF0U && first <= 0xF4U) expected = 4;
    if (value.size() - lead < expected) return lead;
    return value.size();
}

int line_advance_px(const LayoutLine& line) {
    return std::max(1, fx_ceil(line.advance));
}

int line_ink_bottom_px(const LayoutLine& line, int top) {
    const Fx descent = line.descent < 0 ? -line.descent : line.descent;
    return top + fx_ceil(line.ascent) + fx_ceil(descent);
}

int line_ink_height_px(const LayoutLine& line) {
    return line_ink_bottom_px(line, 0);
}

int screen_cache_target_height(const LayoutSignature& signature,
                               int viewport_height) {
    const int guard_line = std::max<int>(
        signature.body_px + 8,
        signature.line_height_px != 0
            ? signature.line_height_px
            : signature.body_px + std::max<int>(
                  2, (signature.body_px + 4) / 5));
    return viewport_height + guard_line;
}

std::uint32_t safe_line_end(const LayoutLine& line) {
    return line.source_offset + line.source_length;
}

}  // namespace

bool PlainTextLayout::initialize(
    std::shared_ptr<RandomAccessData> source,
    std::uint32_t source_offset,
    std::uint32_t source_size,
    const Utf8ValidationResult& sampled_validation,
    TextSystem& text,
    const LayoutSignature& signature,
    int viewport_height,
    std::string& error) {
    clear();
    error.clear();
    if (source == nullptr ||
        static_cast<std::uint64_t>(source_offset) + source_size >
            source->size()) {
        error = "plain-text source range is invalid";
        return false;
    }
    if (!sampled_validation.valid()) {
        error = "plain text is not valid UTF-8";
        return false;
    }
    if (!text.ready() || signature.content_width < 24 ||
        signature.body_px < 6 || viewport_height <= 0) {
        error = "plain-text layout configuration is invalid";
        return false;
    }

    source_ = std::move(source);
    source_offset_ = source_offset;
    source_size_ = source_size;
    sampled_validation_ = sampled_validation;
    text_ = &text;
    glyph_cache_clear_generation_ =
        text.glyph_cache_clear_generation();
    signature_ = signature;
    viewport_height_ = viewport_height;
    reset_auto_page_rows();

    const std::uint8_t* contiguous = source_->contiguous_data();
    if (contiguous != nullptr) {
        source_memory_ = contiguous + source_offset_;
        raw_cache_limit_ = source_size_;
    }
    // NAND random reads are much slower than RAM on the calculator. Reserve a
    // large sequential window, but fill only the first 64 KiB before showing
    // the document. Later small idle quanta grow the window without turning
    // cold open into a multi-second blocking read. Host and decoded memory
    // sources can still expose the complete document without a second copy.
    if (source_memory_ == nullptr) {
#if defined(NMARKDOWN_NDLESS)
        raw_cache_limit_ = std::min<std::size_t>(
            kPlainTextNdlessRawCacheBytes, source_size_);
#else
        raw_cache_limit_ = std::min<std::size_t>(
            kPlainTextRawCacheBytes, source_size_);
#endif
        try {
            raw_cache_.reserve(raw_cache_limit_);
            raw_cache_scratch_.resize(
                std::min(kPlainTextInitialCacheBytes, raw_cache_limit_));
        } catch (const std::bad_alloc&) {
            raw_cache_.clear();
            raw_cache_scratch_.clear();
            raw_cache_limit_ = std::min<std::size_t>(
                1U * 1024U * 1024U, source_size_);
            try {
                raw_cache_.reserve(raw_cache_limit_);
                raw_cache_scratch_.resize(
                    std::min(kPlainTextInitialCacheBytes, raw_cache_limit_));
            } catch (const std::bad_alloc&) {
                raw_cache_.clear();
                raw_cache_scratch_.clear();
                raw_cache_limit_ = std::min<std::size_t>(
                    kPlainTextInitialCacheBytes, source_size_);
                try {
                    raw_cache_.reserve(raw_cache_limit_);
                } catch (const std::bad_alloc&) {
                    raw_cache_.clear();
                }
            }
        }
    }

    if (source_size_ == 0) return true;
    if ((source_memory_ == nullptr &&
         !ensure_cache(0, kPlainTextInitialCacheBytes, error)) ||
        !rebuild_at(0, error)) {
        clear();
        return false;
    }
    return true;
}

void PlainTextLayout::clear() {
    source_.reset();
    source_memory_ = nullptr;
    source_offset_ = 0;
    source_size_ = 0;
    sampled_validation_ = {};
    text_ = nullptr;
    signature_ = {};
    viewport_height_ = 0;
    auto_rows_per_page_ = 0;
    raw_cache_.clear();
    raw_cache_scratch_.clear();
    raw_cache_offset_ = 0;
    raw_cache_head_ = 0;
    raw_cache_valid_size_ = 0;
    raw_cache_limit_ = 0;
    screens_.clear();
    reset_pending_screen();
    current_screen_ = 0;
    current_line_ = 0;
    bottom_align_visible_rows_ = true;
    page_start_offsets_.clear();
    approximate_page_ = 1;
    pixel_scroll_remainder_ = 0;
    deferred_forward_pages_ = 0;
    reset_deferred_target();
    incremental_layout_steps_ = 0;
    glyph_cache_clear_generation_ = 0;
    prefetch_raw_next_ = false;
    invalidate_probe_lines();
}

bool PlainTextLayout::reconfigure(const LayoutSignature& signature,
                                  std::string& error) {
    error.clear();
    if (!loaded()) {
        error = "plain-text layout has no source";
        return false;
    }
    if (signature == signature_) return true;
    const std::uint32_t position = current_source_offset();
    signature_ = signature;
    // Cached probe lines were wrapped for the previous width and font.
    invalidate_probe_lines();
    reset_auto_page_rows();
    page_start_offsets_.clear();
    approximate_page_ = 1;
    return rebuild_at(position, error);
}

bool PlainTextLayout::read_logical(std::uint32_t offset,
                                   std::uint8_t* data,
                                   std::size_t size) const {
    if (source_ == nullptr || offset > source_size_ ||
        size > source_size_ - offset) {
        return false;
    }
    if (source_memory_ != nullptr) {
        if (size != 0) {
            std::memcpy(data, source_memory_ + offset, size);
        }
        return true;
    }
    if (offset >= raw_cache_offset_) {
        const std::size_t local = offset - raw_cache_offset_;
        if (local <= raw_cache_valid_size_ &&
            size <= raw_cache_valid_size_ - local) {
            if (size != 0) {
                copy_cached_bytes(local, data, size);
            }
            return true;
        }
    }
    return source_->read(
        static_cast<std::uint64_t>(source_offset_) + offset, data, size);
}

void PlainTextLayout::copy_cached_bytes(
    std::size_t logical_offset,
    std::uint8_t* destination,
    std::size_t size) const {
    if (size == 0 || raw_cache_.empty()) return;
    const std::size_t physical =
        (raw_cache_head_ + logical_offset) % raw_cache_.size();
    const std::size_t first =
        std::min(size, raw_cache_.size() - physical);
    std::memcpy(destination, raw_cache_.data() + physical, first);
    if (first < size) {
        std::memcpy(destination + first, raw_cache_.data(), size - first);
    }
}

void PlainTextLayout::copy_into_cache(
    std::size_t logical_offset,
    const std::uint8_t* source,
    std::size_t size) {
    if (size == 0 || raw_cache_.empty()) return;
    const std::size_t physical =
        (raw_cache_head_ + logical_offset) % raw_cache_.size();
    const std::size_t first =
        std::min(size, raw_cache_.size() - physical);
    std::memcpy(raw_cache_.data() + physical, source, first);
    if (first < size) {
        std::memcpy(raw_cache_.data(), source + first, size - first);
    }
}

bool PlainTextLayout::ensure_cache(std::uint32_t offset,
                                   std::size_t minimum_bytes,
                                   std::string& error) {
    error.clear();
    if (offset > source_size_) {
        error = "plain-text cache offset is outside the source";
        return false;
    }
    if (source_memory_ != nullptr) {
        return true;
    }
    const std::uint64_t cache_end =
        static_cast<std::uint64_t>(raw_cache_offset_) +
        raw_cache_valid_size_;
    if (offset >= raw_cache_offset_ && offset <= cache_end &&
        cache_end - offset >= minimum_bytes) {
        return true;
    }

    // Preserve and extend the current prefix when the next layout screen
    // reaches its end. The old code discarded the accumulated cache at this
    // boundary, which made continuous downward scrolling return to storage.
    if (raw_cache_valid_size_ != 0 && raw_cache_head_ == 0 &&
        offset >= raw_cache_offset_ && offset <= cache_end) {
        const std::size_t required = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                source_size_,
                static_cast<std::uint64_t>(offset) + minimum_bytes) -
            raw_cache_offset_);
        const std::size_t maximum_here = std::min<std::size_t>(
            raw_cache_limit_, source_size_ - raw_cache_offset_);
        if (required <= maximum_here) {
            const std::size_t old_size = raw_cache_valid_size_;
            const std::size_t next_size = std::min<std::size_t>(
                maximum_here,
                std::max(required, old_size + kPlainTextPrefetchBytes));
            try {
                raw_cache_.resize(next_size);
            } catch (const std::bad_alloc&) {
                raw_cache_.resize(old_size);
                error = "could not extend the plain-text byte cache";
                return false;
            }
            if (!source_->read(
                    static_cast<std::uint64_t>(source_offset_) +
                    raw_cache_offset_ +
                        static_cast<std::uint32_t>(old_size),
                    raw_cache_.data() + old_size, next_size - old_size)) {
                raw_cache_.resize(old_size);
                error = "could not extend the plain-text byte cache";
                return false;
            }
            raw_cache_valid_size_ = next_size;
            return true;
        }
    }

    const std::size_t available = source_size_ - offset;
    const std::size_t count = std::min<std::size_t>(
        {available, raw_cache_limit_,
         std::max(kPlainTextInitialCacheBytes, minimum_bytes)});
    try {
        raw_cache_.resize(count);
    } catch (const std::bad_alloc&) {
        raw_cache_.clear();
        error = "could not allocate the plain-text byte cache";
        return false;
    }
    if (count != 0 &&
        !source_->read(
            static_cast<std::uint64_t>(source_offset_) + offset,
            raw_cache_.data(), count)) {
        raw_cache_.clear();
        raw_cache_valid_size_ = 0;
        error = "could not fill the plain-text byte cache";
        return false;
    }
    raw_cache_offset_ = offset;
    raw_cache_head_ = 0;
    raw_cache_valid_size_ = count;
    return true;
}

bool PlainTextLayout::prefetch_raw_cache(std::string& error) {
    error.clear();
    if (source_memory_ != nullptr) {
        return false;
    }
    const std::size_t maximum_here = std::min<std::size_t>(
        raw_cache_limit_, source_size_ - raw_cache_offset_);
    if (raw_cache_valid_size_ < maximum_here) {
        // A partially filled cache is always linear. Circular rotation begins
        // only after it reaches its fixed capacity.
        if (raw_cache_head_ != 0) return false;
        const std::size_t old_size = raw_cache_valid_size_;
        const std::size_t next_size = std::min<std::size_t>(
            maximum_here, old_size + kPlainTextPrefetchBytes);
        try {
            raw_cache_.resize(next_size);
        } catch (const std::bad_alloc&) {
            raw_cache_.resize(old_size);
            // The active window remains valid. Stop speculative growth rather
            // than turning a cache optimization into a document failure.
            raw_cache_limit_ = old_size;
            return false;
        }
        if (!source_->read(
                static_cast<std::uint64_t>(source_offset_) +
                    raw_cache_offset_ +
                    static_cast<std::uint32_t>(old_size),
                raw_cache_.data() + old_size, next_size - old_size)) {
            raw_cache_.resize(old_size);
            error = "could not prefetch the plain-text byte cache";
            return false;
        }
        raw_cache_valid_size_ = next_size;
        return true;
    }

    const std::uint64_t cache_end =
        static_cast<std::uint64_t>(raw_cache_offset_) +
        raw_cache_valid_size_;
    if (raw_cache_valid_size_ == 0 || cache_end >= source_size_) {
        return false;
    }

    // Once the active position has left a generous amount of history behind,
    // slide the full window during idle time and read its new tail. This
    // prevents the first line at each 1 MiB boundary from paying the NAND
    // refill cost on its input frame while retaining far more history than the
    // 12 shaped previous screens require.
    // Keep the history reserve independent of the intentionally tiny NAND
    // refill quantum. This covers many previous pages even though each idle
    // read is now only 1 KiB.
    constexpr std::size_t kRetainedHistoryBytes = 16U * 1024U;
    const std::uint32_t current = current_source_offset();
    const std::uint64_t protected_end =
        static_cast<std::uint64_t>(raw_cache_offset_) +
        kRetainedHistoryBytes;
    if (current < protected_end + kPlainTextPrefetchBytes) {
        return false;
    }
    const std::size_t shift = std::min<std::size_t>(
        {kPlainTextPrefetchBytes,
         raw_cache_valid_size_,
         static_cast<std::size_t>(source_size_ - cache_end)});
    if (shift == 0 ||
        raw_cache_scratch_.size() < shift ||
        raw_cache_.empty()) {
        return false;
    }

    const std::uint32_t next_offset =
        raw_cache_offset_ + static_cast<std::uint32_t>(shift);
    const std::size_t next_size = std::min<std::size_t>(
        raw_cache_valid_size_, source_size_ - next_offset);
    const std::size_t retained =
        std::min<std::size_t>(raw_cache_valid_size_ - shift, next_size);
    const std::size_t refill = next_size - retained;
    if (refill != 0 &&
        !source_->read(
            static_cast<std::uint64_t>(source_offset_) + next_offset +
                static_cast<std::uint32_t>(retained),
            raw_cache_scratch_.data(), refill)) {
        error = "could not slide the plain-text byte cache";
        return false;
    }

    // Discarding the oldest quantum only advances the physical head. The
    // retained bytes stay in place; only the new tail is copied, so a small
    // refill never turns into a nearly 1 MiB memmove on the calculator.
    const std::size_t next_head =
        (raw_cache_head_ + shift) % raw_cache_.size();
    raw_cache_head_ = next_head;
    if (refill != 0) {
        copy_into_cache(retained, raw_cache_scratch_.data(), refill);
    }
    raw_cache_offset_ = next_offset;
    raw_cache_valid_size_ = next_size;
    return true;
}

std::string_view PlainTextLayout::cached_view(
    std::uint32_t offset,
    std::size_t maximum_bytes) {
    if (source_memory_ != nullptr) {
        if (offset > source_size_) return {};
        return std::string_view(
            reinterpret_cast<const char*>(source_memory_ + offset),
            std::min<std::size_t>(
                source_size_ - offset, maximum_bytes));
    }
    if (offset < raw_cache_offset_) return {};
    const std::size_t local = offset - raw_cache_offset_;
    if (local > raw_cache_valid_size_ || raw_cache_.empty()) return {};
    const std::size_t available = std::min<std::size_t>(
        raw_cache_valid_size_ - local, maximum_bytes);
    const std::size_t physical =
        (raw_cache_head_ + local) % raw_cache_.size();
    const std::size_t contiguous =
        std::min(available, raw_cache_.size() - physical);
    if (contiguous == available) {
        return std::string_view(
            reinterpret_cast<const char*>(raw_cache_.data() + physical),
            contiguous);
    }

    const std::size_t linear_size =
        std::min(available, raw_cache_scratch_.size());
    if (linear_size < available) {
        return {};
    }
    copy_cached_bytes(local, raw_cache_scratch_.data(), linear_size);
    return std::string_view(
        reinterpret_cast<const char*>(raw_cache_scratch_.data()),
        linear_size);
}

void PlainTextLayout::invalidate_probe_lines() {
    probe_lines_.clear();
    probe_next_ = 0;
    probe_next_offset_ = 0;
    probe_valid_ = false;
}

bool PlainTextLayout::build_line(
    std::uint32_t start_offset,
    LayoutLine& line,
    bool& eof,
    std::string& error) {
    error.clear();
    line = {};
    eof = false;
    start_offset = std::min(start_offset, source_size_);
    if (start_offset >= source_size_) {
        eof = true;
        return false;
    }

    // Serve naturally sequential requests from the sealed lines the previous
    // probe already shaped: no HarfBuzz call, no byte-cache access. A hit
    // requires the exact next offset, so out-of-order callers simply miss.
    if (probe_valid_ && probe_next_ < probe_lines_.size() &&
        start_offset == probe_next_offset_) {
        line = std::move(probe_lines_[probe_next_]);
        ++probe_next_;
        const std::uint32_t next =
            std::min(safe_line_end(line), source_size_);
        if (probe_next_ >= probe_lines_.size()) {
            invalidate_probe_lines();
        } else {
            probe_next_offset_ = next;
        }
        if (next <= start_offset) {
            error = "plain-text line made no forward progress";
            return false;
        }
        eof = next >= source_size_;
        return true;
    }
    invalidate_probe_lines();

    const std::size_t needed = std::min<std::size_t>(
        kPlainTextLineProbeBytes, source_size_ - start_offset);
    if (!ensure_cache(start_offset, needed, error)) {
        return false;
    }

    std::string_view available =
        cached_view(start_offset, kPlainTextLineProbeBytes);
    const std::size_t safe_size = complete_utf8_prefix(available);
    available = available.substr(0, safe_size);
    if (available.empty()) {
        error = "plain-text cache ended inside a UTF-8 character";
        return false;
    }

    BlockLayout block;
    std::size_t consumed = 0;
    const Fx target = fx_from_int(std::max<int>(
        signature_.body_px + 8, signature_.line_height_px));
    if (!layout_plain_text_region(available, start_offset, *text_,
                                  signature_, target, block, consumed)) {
        error = "could not wrap the active plain-text region";
        return false;
    }
    if (consumed == 0 || block.lines.empty()) {
        error = "plain-text layout made no forward progress";
        return false;
    }
    const Utf8ValidationResult validation = utf8_validate(
        reinterpret_cast<const std::uint8_t*>(available.data()), consumed,
        false);
    if (!validation.valid()) {
        error = "plain text is not valid UTF-8";
        return false;
    }

    line = std::move(block.lines.front());
    const std::uint32_t next =
        std::min(safe_line_end(line), source_size_);
    if (next <= start_offset) {
        error = "plain-text line made no forward progress";
        return false;
    }

    // Retain the probe's remaining sealed lines for the sequential calls
    // that follow. The final line is still open unless the probe reached the
    // end of the source — more bytes could extend or re-wrap it — so it is
    // dropped and shaped again by the next probe. Continuation lines were
    // shaped with left context inside one HarfBuzz run; for the supported
    // scripts their cluster advances match a fresh run started at the line
    // boundary, and every consumer chains from the served line's own end, so
    // the partition stays continuous either way.
    const bool probe_reached_end =
        static_cast<std::uint64_t>(start_offset) + consumed >= source_size_;
    const std::size_t keep = probe_reached_end
                                 ? block.lines.size()
                                 : block.lines.size() - 1;
    if (keep >= 2) {
        // A wrap point drops the inter-word whitespace run from both
        // neighbouring lines, while a fresh probe started at that whitespace
        // folds it (glyph-free) into the following line's source range. Fold
        // the same bytes into each retained line so served lines chain
        // exactly like freshly probed ones: every line starts where the
        // previous one ended.
        std::uint32_t expected = next;
        for (std::size_t index = 1; index < keep; ++index) {
            LayoutLine& candidate = block.lines[index];
            if (candidate.source_offset < expected) break;
            const std::uint32_t gap = candidate.source_offset - expected;
            candidate.source_offset = expected;
            candidate.source_length += gap;
            expected = std::min(safe_line_end(candidate), source_size_);
            probe_lines_.push_back(std::move(candidate));
        }
        if (!probe_lines_.empty()) {
            probe_next_ = 0;
            probe_next_offset_ = next;
            probe_valid_ = true;
        }
    }
    eof = next >= source_size_;
    return true;
}

void PlainTextLayout::reset_auto_page_rows() {
    auto_rows_per_page_ = 0;
    if (signature_.line_height_px != 0 || viewport_height_ <= 0) return;
    // Leave a small safety reserve above the normal leading, then distribute
    // the remaining pixels evenly between the fitted rows at paint time.
    const int minimum_advance = std::max<int>(
        1, static_cast<int>(signature_.body_px) + 4);
    auto_rows_per_page_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(viewport_height_ / minimum_advance));
}

void PlainTextLayout::observe_auto_line_metrics(const LayoutLine& line) {
    if (!auto_page_fit()) return;
    const int minimum_advance = std::max<int>(
        static_cast<int>(signature_.body_px) + 4,
        line_advance_px(line));
    const std::size_t safe_rows = std::max<std::size_t>(
        1, static_cast<std::size_t>(viewport_height_ / minimum_advance));
    auto_rows_per_page_ = std::min(auto_rows_per_page_, safe_rows);
}

bool PlainTextLayout::auto_page_fit() const {
    return signature_.line_height_px == 0 && auto_rows_per_page_ != 0;
}

bool PlainTextLayout::build_screen(std::uint32_t start_offset,
                                   Screen& screen,
                                   std::string& error) {
    error.clear();
    screen = {};
    screen.start_offset = std::min(start_offset, source_size_);
    screen.next_offset = screen.start_offset;
    if (screen.start_offset >= source_size_) {
        screen.eof = true;
        return true;
    }

    int height = 0;
    const int target_height =
        screen_cache_target_height(signature_, viewport_height_);
    std::uint32_t cursor = screen.start_offset;
    while (height < target_height && cursor < source_size_) {
        LayoutLine line;
        bool eof = false;
        if (!build_line(cursor, line, eof, error)) {
            return false;
        }
        observe_auto_line_metrics(line);
        cursor = safe_line_end(line);
        height += line_advance_px(line);
        screen.lines.push_back(std::move(line));
        if (eof) break;
    }
    if (screen.lines.empty() || cursor <= screen.start_offset) {
        error = "plain-text screen made no forward progress";
        return false;
    }
    screen.next_offset = std::min(cursor, source_size_);
    screen.eof = screen.next_offset >= source_size_;
    return true;
}

void PlainTextLayout::reset_pending_screen() {
    pending_screen_ = {};
    pending_screen_height_ = 0;
    pending_screen_active_ = false;
}

void PlainTextLayout::reset_deferred_target() {
    deferred_target_ = {};
    deferred_target_valid_ = false;
    deferred_target_glyphs_ready_ = false;
    deferred_warm_screen_ = 0;
    deferred_warm_line_ = 0;
    deferred_warm_run_ = 0;
    deferred_warm_glyph_ = 0;
    deferred_warm_height_ = 0;
    deferred_warm_rows_ = 0;
    deferred_warm_start_evictions_ = 0;
    deferred_warm_failed_ = false;
    deferred_warm_retry_count_ = 0;
}

void PlainTextLayout::queue_forward_page() {
    if (deferred_forward_pages_ == 0) {
        reset_deferred_target();
    }
    deferred_forward_pages_ = std::min<std::size_t>(
        deferred_forward_pages_ + 1, 64);
}

bool PlainTextLayout::build_future_screen_step(std::string& error) {
    error.clear();
    if (screens_.empty()) return source_size_ == 0;
    const Screen& tail = screens_.back();
    if (tail.eof) return false;

    if (pending_screen_active_ &&
        pending_screen_.start_offset != tail.next_offset) {
        reset_pending_screen();
    }
    if (!pending_screen_active_) {
        pending_screen_.start_offset = tail.next_offset;
        pending_screen_.next_offset = tail.next_offset;
        pending_screen_active_ = true;
    }

    LayoutLine line;
    bool eof = false;
    if (!build_line(pending_screen_.next_offset, line, eof, error)) {
        return false;
    }
    observe_auto_line_metrics(line);
    ++incremental_layout_steps_;
    pending_screen_.next_offset = safe_line_end(line);
    pending_screen_height_ += line_advance_px(line);
    pending_screen_.lines.push_back(std::move(line));
    pending_screen_.eof = eof;

    if (pending_screen_height_ >=
            screen_cache_target_height(signature_, viewport_height_) ||
        eof) {
        screens_.push_back(std::move(pending_screen_));
        reset_pending_screen();
    }
    return true;
}

bool PlainTextLayout::ensure_future_screen(std::string& error) {
    error.clear();
    if (screens_.empty()) return source_size_ == 0;
    if (screens_.back().eof) return false;
    const std::size_t before = screens_.size();
    while (screens_.size() == before) {
        if (!build_future_screen_step(error)) return false;
    }
    return true;
}

std::uint32_t PlainTextLayout::line_offset(std::size_t screen,
                                           std::size_t line) const {
    if (screen >= screens_.size()) return source_size_;
    const Screen& item = screens_[screen];
    if (line >= item.lines.size()) return item.next_offset;
    return item.lines[line].source_offset;
}

std::uint32_t PlainTextLayout::line_end(std::size_t screen,
                                        std::size_t line) const {
    if (screen >= screens_.size()) return source_size_;
    const Screen& item = screens_[screen];
    if (line >= item.lines.size()) return item.next_offset;
    return safe_line_end(item.lines[line]);
}

std::uint32_t PlainTextLayout::current_source_offset() const {
    if (screens_.empty()) return 0;
    return line_offset(current_screen_, current_line_);
}

std::uint32_t PlainTextLayout::visible_source_end() const {
    if (screens_.empty()) return 0;
    std::size_t screen = current_screen_;
    std::size_t line = current_line_;
    std::uint32_t end = current_source_offset();
    if (auto_page_fit()) {
        std::size_t rows = 0;
        while (screen < screens_.size() &&
               rows < auto_rows_per_page_) {
            const Screen& item = screens_[screen];
            while (line < item.lines.size() &&
                   rows < auto_rows_per_page_) {
                end = std::max(end, line_end(screen, line));
                ++line;
                ++rows;
            }
            ++screen;
            line = 0;
        }
        return end;
    }

    int y = 0;
    bool viewport_filled = false;
    while (screen < screens_.size() && !viewport_filled) {
        const Screen& item = screens_[screen];
        while (line < item.lines.size() && !viewport_filled) {
            end = std::max(end, line_end(screen, line));
            const LayoutLine& layout_line = item.lines[line];
            const int ink_bottom = line_ink_bottom_px(layout_line, y);
            y += line_advance_px(layout_line);
            ++line;
            viewport_filled = bottom_align_visible_rows_
                                  ? ink_bottom >= viewport_height_
                                  : y >= viewport_height_;
        }
        ++screen;
        line = 0;
    }
    return end;
}

bool PlainTextLayout::at_end() const {
    if (source_size_ == 0) return true;
    if (screens_.empty()) return false;
    const Screen& tail = screens_.back();
    return tail.eof && visible_source_end() >= source_size_;
}

bool PlainTextLayout::advance_position(std::size_t& screen,
                                       std::size_t& line,
                                       std::string& error) {
    error.clear();
    if (screen >= screens_.size()) return false;
    if (line + 1 < screens_[screen].lines.size()) {
        ++line;
        return true;
    }
    if (screen + 1 >= screens_.size() ||
        screens_[screen + 1].lines.empty()) {
        return false;
    }
    ++screen;
    line = 0;
    return true;
}

bool PlainTextLayout::retreat_position(std::size_t& screen,
                                       std::size_t& line,
                                       std::string& error) {
    error.clear();
    if (screen >= screens_.size()) return false;
    if (line != 0) {
        --line;
        return true;
    }
    if (screen == 0) {
        if (!rebuild_previous_region(error)) return false;
        screen = current_screen_;
        line = current_line_;
        if (line != 0) {
            --line;
            return true;
        }
        if (screen == 0) return false;
    }
    --screen;
    if (screens_[screen].lines.empty()) return false;
    line = screens_[screen].lines.size() - 1;
    return true;
}

void PlainTextLayout::trim_screen_window() {
    while (current_screen_ > kPlainTextPreviousScreens) {
        screens_.pop_front();
        --current_screen_;
    }
    while (screens_.size() >
           current_screen_ + 1 + kPlainTextFutureScreens) {
        screens_.pop_back();
    }
    if (pending_screen_active_ &&
        (screens_.empty() ||
         pending_screen_.start_offset != screens_.back().next_offset)) {
        reset_pending_screen();
    }
}

bool PlainTextLayout::move_line(int direction, std::string& error) {
    error.clear();
    if (direction == 0 || screens_.empty()) return false;
    std::size_t screen = current_screen_;
    std::size_t line = current_line_;
    const bool moved = direction > 0
                           ? advance_position(screen, line, error)
                           : retreat_position(screen, line, error);
    if (!moved) return false;
    deferred_forward_pages_ = 0;
    reset_deferred_target();
    current_screen_ = screen;
    current_line_ = line;
    bottom_align_visible_rows_ = direction > 0;
    page_start_offsets_.clear();
    pixel_scroll_remainder_ = 0;
    trim_screen_window();
    return true;
}

bool PlainTextLayout::move_page(int direction, std::string& error) {
    return move_page_now(direction, true, error);
}

bool PlainTextLayout::find_forward_page_target(
    PageTarget& result,
    bool& needs_more_layout) const {
    result = {};
    needs_more_layout = false;
    if (screens_.empty() || current_screen_ >= screens_.size() ||
        current_line_ >= screens_[current_screen_].lines.size()) {
        return false;
    }

    const auto advance_cached =
        [this](std::size_t& screen, std::size_t& line) {
            if (screen >= screens_.size()) return false;
            if (line + 1 < screens_[screen].lines.size()) {
                ++line;
                return true;
            }
            if (screen + 1 >= screens_.size() ||
                screens_[screen + 1].lines.empty()) {
                return false;
            }
            ++screen;
            line = 0;
            return true;
        };

    if (auto_page_fit()) {
        std::size_t screen = current_screen_;
        std::size_t line = current_line_;
        for (std::size_t row = 1; row < auto_rows_per_page_; ++row) {
            if (!advance_cached(screen, line)) {
                needs_more_layout = !screens_.back().eof;
                return false;
            }
        }
        if (!advance_cached(screen, line)) {
            needs_more_layout = !screens_.back().eof;
            return false;
        }
        if (line_offset(screen, line) <= current_source_offset()) return false;
        result = {screen, line};
        return true;
    }

    struct Position {
        std::size_t screen = 0;
        std::size_t line = 0;
        int top = 0;
        int bottom = 0;
    };
    std::size_t screen = current_screen_;
    std::size_t line = current_line_;
    int y = 0;
    Position target;
    bool found_visible = false;
    while (screen < screens_.size()) {
        if (line >= screens_[screen].lines.size()) return false;
        const LayoutLine& item = screens_[screen].lines[line];
        const int advance = line_advance_px(item);
        target = {screen, line, y, y + advance};
        found_visible = true;
        const int ink_bottom = line_ink_bottom_px(item, y);
        y += advance;
        const bool viewport_filled = bottom_align_visible_rows_
                                         ? ink_bottom >= viewport_height_
                                         : y >= viewport_height_;
        if (viewport_filled) break;
        if (!advance_cached(screen, line)) {
            needs_more_layout = !screens_.back().eof;
            if (needs_more_layout) return false;
            break;
        }
    }
    if (!found_visible) return false;

    const int line_height = std::max(1, target.bottom - target.top);
    const int visible_height = bottom_align_visible_rows_
                                   ? line_height
                                   : std::max(0, viewport_height_ - target.top);
    if (visible_height * 100 >= line_height * kMostlyVisiblePercent) {
        std::size_t following_screen = target.screen;
        std::size_t following_line = target.line;
        if (advance_cached(following_screen, following_line)) {
            target.screen = following_screen;
            target.line = following_line;
        } else if (!screens_.back().eof) {
            needs_more_layout = true;
            return false;
        }
    }

    if (line_offset(target.screen, target.line) <=
        current_source_offset()) {
        return false;
    }
    result = {target.screen, target.line};
    return true;
}

bool PlainTextLayout::cached_viewport_ready(
    const PageTarget& target) const {
    if (target.screen >= screens_.size() ||
        target.line >= screens_[target.screen].lines.size()) {
        return false;
    }
    std::size_t screen = target.screen;
    std::size_t line = target.line;
    if (auto_page_fit()) {
        for (std::size_t row = 1; row < auto_rows_per_page_; ++row) {
            if (line + 1 < screens_[screen].lines.size()) {
                ++line;
                continue;
            }
            if (screen + 1 >= screens_.size()) {
                return screens_.back().eof;
            }
            ++screen;
            line = 0;
            if (screens_[screen].lines.empty()) return false;
        }
        return true;
    }

    int height = 0;
    while (screen < screens_.size()) {
        if (line >= screens_[screen].lines.size()) return false;
        height += line_advance_px(screens_[screen].lines[line]);
        if (height >= viewport_height_) return true;
        if (line + 1 < screens_[screen].lines.size()) {
            ++line;
            continue;
        }
        if (screen + 1 >= screens_.size()) {
            return screens_.back().eof;
        }
        ++screen;
        line = 0;
    }
    return screens_.back().eof;
}

bool PlainTextLayout::page_glyphs_preloaded(
    const PageTarget& target) const {
    if (text_ == nullptr ||
        glyph_cache_clear_generation_ !=
            text_->glyph_cache_clear_generation()) {
        return false;
    }
    if (target.screen >= screens_.size() ||
        target.line >= screens_[target.screen].lines.size()) {
        return false;
    }
    const bool atlas_has_evicted =
        text_->cache_stats().evictions != 0;
    const auto resident_if_needed = [&] {
        return !atlas_has_evicted || page_glyphs_cached(target);
    };
    std::size_t screen = target.screen;
    std::size_t line = target.line;
    int height = 0;
    std::size_t rows = 0;
    while (screen < screens_.size()) {
        const Screen& item = screens_[screen];
        if (line >= item.lines.size() ||
            item.next_preload_line <= line) {
            return false;
        }
        height += line_advance_px(item.lines[line]);
        ++rows;
        if (auto_page_fit() ? rows >= auto_rows_per_page_
                            : height >= viewport_height_) {
            return resident_if_needed();
        }
        if (line + 1 < item.lines.size()) {
            ++line;
            continue;
        }
        if (screen + 1 >= screens_.size()) {
            return screens_.back().eof && resident_if_needed();
        }
        ++screen;
        line = 0;
    }
    return screens_.back().eof && resident_if_needed();
}

bool PlainTextLayout::page_glyphs_cached(
    const PageTarget& target) const {
    if (text_ == nullptr) return false;
    if (target.screen >= screens_.size() ||
        target.line >= screens_[target.screen].lines.size()) {
        return false;
    }
    std::size_t screen = target.screen;
    std::size_t line = target.line;
    int height = 0;
    std::size_t rows = 0;
    while (screen < screens_.size()) {
        const Screen& item = screens_[screen];
        if (line >= item.lines.size()) return false;
        const LayoutLine& layout_line = item.lines[line];
        for (const LayoutRun& run : layout_line.runs) {
            if (!text_->run_cached(run.glyphs, run.pixel_size)) {
                return false;
            }
        }
        height += line_advance_px(layout_line);
        ++rows;
        if (auto_page_fit() ? rows >= auto_rows_per_page_
                            : height >= viewport_height_) {
            return true;
        }
        if (line + 1 < item.lines.size()) {
            ++line;
            continue;
        }
        if (screen + 1 >= screens_.size()) {
            return screens_.back().eof;
        }
        ++screen;
        line = 0;
    }
    return screens_.back().eof;
}

void PlainTextLayout::reset_glyph_preload_cursors() {
    for (Screen& screen : screens_) {
        screen.next_preload_line = 0;
        screen.next_preload_run = 0;
        screen.next_preload_glyph = 0;
    }
    reset_deferred_target();
}

void PlainTextLayout::synchronize_glyph_cache_generation() {
    if (text_ == nullptr) return;
    const std::uint64_t generation =
        text_->glyph_cache_clear_generation();
    if (glyph_cache_clear_generation_ == generation) return;
    glyph_cache_clear_generation_ = generation;
    reset_glyph_preload_cursors();
}

void PlainTextLayout::begin_deferred_target(
    const PageTarget& target) {
    deferred_target_ = target;
    deferred_target_valid_ = true;
    deferred_target_glyphs_ready_ =
        page_glyphs_cached(target);
    deferred_warm_retry_count_ = 0;
    rewind_deferred_warm_cursor();
}

void PlainTextLayout::rewind_deferred_warm_cursor() {
    deferred_warm_screen_ = deferred_target_.screen;
    deferred_warm_line_ = deferred_target_.line;
    deferred_warm_run_ = 0;
    deferred_warm_glyph_ = 0;
    deferred_warm_height_ = 0;
    deferred_warm_rows_ = 0;
    deferred_warm_start_evictions_ =
        text_ == nullptr ? 0 : text_->cache_stats().evictions;
    deferred_warm_failed_ = false;
}

void PlainTextLayout::finish_deferred_warm_pass() {
    const std::uint64_t current_evictions =
        text_ == nullptr ? 0 : text_->cache_stats().evictions;
    // A complete pass with no failed glyph and no intervening atlas eviction
    // proves residency without scanning the whole viewport on the input path.
    deferred_target_glyphs_ready_ =
        !deferred_warm_failed_ &&
        current_evictions == deferred_warm_start_evictions_;
    if (!deferred_target_glyphs_ready_) {
        deferred_target_glyphs_ready_ =
            page_glyphs_cached(deferred_target_);
    }
    if (!deferred_target_glyphs_ready_) {
        if (deferred_warm_retry_count_ == 0) {
            ++deferred_warm_retry_count_;
            rewind_deferred_warm_cursor();
        } else {
            // A permanently failing glyph or an atlas smaller than one
            // viewport must not make forward navigation wait forever. The
            // normal renderer retains its existing fallback in this rare
            // heap-pressure case.
            deferred_target_glyphs_ready_ = true;
        }
    }
}

bool PlainTextLayout::move_page_now(int direction,
                                    bool allow_defer,
                                    std::string& error) {
    error.clear();
    if (direction == 0 || screens_.empty()) return false;
    synchronize_glyph_cache_generation();
    if (direction < 0) {
        deferred_forward_pages_ = 0;
        reset_deferred_target();
        if (!page_start_offsets_.empty()) {
            const std::uint32_t target = page_start_offsets_.back();
            page_start_offsets_.pop_back();
            if (!set_cached_position(target) &&
                !rebuild_at(target, error)) {
                return false;
            }
            bottom_align_visible_rows_ = false;
            approximate_page_ = std::max(1, approximate_page_ - 1);
            return true;
        }
        // Line scrolling and percentage jumps intentionally discard page
        // history. Walk one visual viewport backward through the retained
        // screen window instead of treating that as an immovable position.
        // move_line() can rebuild one bounded preceding region when the
        // retained window edge is reached; continuing for a full viewport also
        // avoids leaving only that region's final line at the top of the page.
        const std::uint32_t before = current_source_offset();
        int traversed_height = 0;
        std::size_t traversed_rows = 0;
        bool moved = false;
        while (auto_page_fit()
                   ? traversed_rows < auto_rows_per_page_
                   : traversed_height < viewport_height_) {
            if (!move_line(-1, error)) {
                if (!error.empty()) return false;
                break;
            }
            if (current_screen_ >= screens_.size() ||
                current_line_ >= screens_[current_screen_].lines.size()) {
                break;
            }
            traversed_height += line_advance_px(
                screens_[current_screen_].lines[current_line_]);
            ++traversed_rows;
            moved = true;
        }
        if (!moved || current_source_offset() >= before) return false;
        bottom_align_visible_rows_ = false;
        approximate_page_ = std::max(1, approximate_page_ - 1);
        pixel_scroll_remainder_ = 0;
        trim_screen_window();
        return true;
    }
    if (allow_defer && deferred_forward_pages_ != 0) {
        queue_forward_page();
        return false;
    }

    PageTarget target;
    bool needs_more_layout = false;
    if (!find_forward_page_target(target, needs_more_layout)) {
        if (allow_defer && needs_more_layout) queue_forward_page();
        return false;
    }
    if (!cached_viewport_ready(target)) {
        if (allow_defer) queue_forward_page();
        return false;
    }
    if (allow_defer && !page_glyphs_preloaded(target)) {
        queue_forward_page();
        begin_deferred_target(target);
        return false;
    }
    const std::uint32_t old_start = current_source_offset();
    const std::uint32_t new_start =
        line_offset(target.screen, target.line);
    if (new_start <= old_start) return false;
    if (page_start_offsets_.size() >= kMaximumPageStartOffsets) {
        page_start_offsets_.pop_front();
    }
    page_start_offsets_.push_back(old_start);
    current_screen_ = target.screen;
    current_line_ = target.line;
    // Page Down always starts with a complete top row. Bottom alignment is a
    // one-line Down affordance and must not leak into screen-sized movement.
    bottom_align_visible_rows_ = false;
    ++approximate_page_;
    pixel_scroll_remainder_ = 0;
    trim_screen_window();
    return true;
}

bool PlainTextLayout::scroll_pixels(int pixels, std::string& error) {
    error.clear();
    if (pixels == 0) return false;
    int remainder = pixel_scroll_remainder_ + pixels;
    const int threshold = std::max<int>(
        8, signature_.line_height_px != 0
               ? signature_.line_height_px
               : signature_.body_px + std::max<int>(
                     2, (signature_.body_px + 4) / 5));
    bool changed = false;
    while (remainder >= threshold) {
        if (!move_line(1, error)) break;
        remainder -= threshold;
        changed = true;
    }
    while (remainder <= -threshold) {
        if (!move_line(-1, error)) break;
        remainder += threshold;
        changed = true;
    }
    // move_line() intentionally clears stale key/page gesture state. Preserve
    // the drag's independent sub-line accumulator across those moves so one
    // threshold crossing cannot immediately cancel itself in the opposite
    // loop.
    pixel_scroll_remainder_ = remainder;
    return changed;
}

bool PlainTextLayout::find_cached_position(std::uint32_t offset,
                                           std::size_t& screen,
                                           std::size_t& line) const {
    for (std::size_t screen_index = 0; screen_index < screens_.size();
         ++screen_index) {
        const Screen& item = screens_[screen_index];
        for (std::size_t line_index = 0; line_index < item.lines.size();
             ++line_index) {
            if (item.lines[line_index].source_offset == offset) {
                screen = screen_index;
                line = line_index;
                return true;
            }
        }
    }
    return false;
}

bool PlainTextLayout::set_cached_position(std::uint32_t offset) {
    std::size_t screen = 0;
    std::size_t line = 0;
    if (!find_cached_position(offset, screen, line)) return false;
    current_screen_ = screen;
    current_line_ = line;
    trim_screen_window();
    return true;
}

bool PlainTextLayout::rebuild_at(std::uint32_t offset, std::string& error) {
    error.clear();
    offset = std::min(offset, source_size_);
    screens_.clear();
    reset_pending_screen();
    current_screen_ = 0;
    current_line_ = 0;
    pixel_scroll_remainder_ = 0;
    deferred_forward_pages_ = 0;
    reset_deferred_target();
    prefetch_raw_next_ = false;
    if (source_size_ == 0) return true;
    Screen screen;
    if (!build_screen(offset, screen, error)) return false;
    screens_.push_back(std::move(screen));
    return true;
}

bool PlainTextLayout::rebuild_previous_region(std::string& error) {
    error.clear();
    const std::uint32_t current = current_source_offset();
    if (current == 0) return false;
    const std::uint32_t rough = current > kPreviousRegionProbeBytes
                                    ? current - kPreviousRegionProbeBytes
                                    : 0;
    const std::uint32_t begin = align_to_line_start(rough, error);
    if (!error.empty()) return false;

    std::deque<Screen> rebuilt;
    std::uint32_t cursor = begin;
    std::size_t containing_screen = 0;
    std::size_t containing_line = 0;
    bool found = false;
    for (std::size_t count = 0;
         count < kPlainTextPreviousScreens + 2 && cursor < current;
         ++count) {
        Screen screen;
        if (!build_screen(cursor, screen, error)) return false;
        for (std::size_t line = 0; line < screen.lines.size(); ++line) {
            if (screen.lines[line].source_offset >= current) {
                containing_screen = rebuilt.size();
                containing_line = line;
                found = true;
                break;
            }
        }
        cursor = screen.next_offset;
        rebuilt.push_back(std::move(screen));
        if (found || rebuilt.back().eof) break;
    }
    if (rebuilt.empty()) return false;
    screens_ = std::move(rebuilt);
    if (found) {
        current_screen_ = containing_screen;
        current_line_ = containing_line;
    } else {
        current_screen_ = screens_.size() - 1;
        current_line_ = screens_.back().lines.empty()
                            ? 0
                            : screens_.back().lines.size() - 1;
    }
    trim_screen_window();
    return current_source_offset() < current;
}

std::uint32_t PlainTextLayout::align_to_line_start(
    std::uint32_t offset,
    std::string& error) const {
    error.clear();
    offset = std::min(offset, source_size_);
    if (offset == 0) return 0;
    constexpr std::size_t kScanBytes = 4096;
    std::array<std::uint8_t, kScanBytes> buffer{};
    std::uint32_t cursor = offset;
    while (cursor != 0) {
        const std::uint32_t begin =
            cursor > buffer.size()
                ? cursor - static_cast<std::uint32_t>(buffer.size())
                : 0;
        const std::size_t count = cursor - begin;
        if (!read_logical(begin, buffer.data(), count)) {
            error = "could not align the plain-text jump";
            return offset;
        }
        for (std::size_t index = count; index != 0; --index) {
            const std::uint8_t value = buffer[index - 1];
            if (value == '\n') {
                return begin + static_cast<std::uint32_t>(index);
            }
            if (value == '\r') {
                std::uint32_t result =
                    begin + static_cast<std::uint32_t>(index);
                if (result < source_size_) {
                    std::uint8_t following = 0;
                    if (read_logical(result, &following, 1) &&
                        following == '\n') {
                        ++result;
                    }
                }
                return result;
            }
        }
        cursor = begin;
    }
    return 0;
}

bool PlainTextLayout::seek_source(std::uint32_t source_offset,
                                  std::string& error) {
    error.clear();
    deferred_forward_pages_ = 0;
    reset_deferred_target();
    const std::uint32_t aligned =
        align_to_line_start(std::min(source_offset, source_size_), error);
    if (!error.empty()) return false;
    page_start_offsets_.clear();
    approximate_page_ = std::max(
        1, static_cast<int>(
               static_cast<std::uint64_t>(aligned) * 100 /
               std::max<std::uint32_t>(1, source_size_)));
    const bool moved = set_cached_position(aligned) ||
                       rebuild_at(aligned, error);
    if (moved) bottom_align_visible_rows_ = true;
    return moved;
}

bool PlainTextLayout::seek_percentage(unsigned percentage,
                                      std::string& error) {
    percentage = std::min(100U, percentage);
    if (percentage == 100U && source_size_ != 0) {
        error.clear();
        page_start_offsets_.clear();
        const std::uint32_t rough =
            source_size_ > kPreviousRegionProbeBytes
                ? source_size_ - kPreviousRegionProbeBytes
                : 0;
        const std::uint32_t begin = align_to_line_start(rough, error);
        if (!error.empty() || !rebuild_at(begin, error)) return false;
        while (!screens_.empty() && !screens_.back().eof) {
            if (!ensure_future_screen(error)) {
                if (error.empty()) {
                    error = "could not prepare the final plain-text screen";
                }
                return false;
            }
        }
        if (screens_.empty()) return false;

        std::size_t screen = screens_.size() - 1;
        while (screen != 0 && screens_[screen].lines.empty()) --screen;
        if (screens_[screen].lines.empty()) return false;
        std::size_t line = screens_[screen].lines.size() - 1;
        int visible_height = line_advance_px(screens_[screen].lines[line]);
        std::size_t visible_rows = 1;
        while (auto_page_fit()
                   ? visible_rows < auto_rows_per_page_
                   : visible_height < viewport_height_) {
            std::size_t previous_screen = screen;
            std::size_t previous_line = line;
            if (previous_line != 0) {
                --previous_line;
            } else {
                if (previous_screen == 0) break;
                --previous_screen;
                if (screens_[previous_screen].lines.empty()) break;
                previous_line = screens_[previous_screen].lines.size() - 1;
            }
            const int previous_height =
                line_advance_px(
                    screens_[previous_screen].lines[previous_line]);
            if (!auto_page_fit() &&
                visible_height + previous_height > viewport_height_) {
                break;
            }
            screen = previous_screen;
            line = previous_line;
            visible_height += previous_height;
            ++visible_rows;
        }
        current_screen_ = screen;
        current_line_ = line;
        bottom_align_visible_rows_ = true;
        approximate_page_ = 100;
        pixel_scroll_remainder_ = 0;
        trim_screen_window();
        return true;
    }
    const std::uint32_t approximate = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(source_size_) * percentage / 100U);
    return seek_source(approximate, error);
}

std::vector<PlainTextVisibleLine> PlainTextLayout::visible_lines(
    std::string& error) {
    error.clear();
    std::vector<PlainTextVisibleLine> result;
    if (screens_.empty()) return result;
    std::size_t screen = current_screen_;
    std::size_t line = current_line_;
    if (auto_page_fit()) {
        while (screen < screens_.size() &&
               result.size() < auto_rows_per_page_) {
            if (line >= screens_[screen].lines.size()) {
                ++screen;
                line = 0;
                continue;
            }
            result.push_back({&screens_[screen].lines[line], 0});
            ++line;
        }
        if (result.empty()) return result;

        if (result.size() == auto_rows_per_page_ && result.size() > 1) {
            int total_ink_height = 0;
            for (const PlainTextVisibleLine& positioned : result) {
                total_ink_height += line_ink_height_px(*positioned.line);
            }
            if (total_ink_height <= viewport_height_) {
                const int total_gap = viewport_height_ - total_ink_height;
                const int gap_count = static_cast<int>(result.size() - 1);
                int y = 0;
                int distributed_gap = 0;
                for (std::size_t index = 0; index < result.size(); ++index) {
                    const LayoutLine& item = *result[index].line;
                    result[index].baseline_y = y + fx_ceil(item.ascent);
                    y += line_ink_height_px(item);
                    if (index + 1 < result.size()) {
                        const int next_distributed_gap =
                            static_cast<int>((index + 1) * total_gap /
                                             gap_count);
                        y += next_distributed_gap - distributed_gap;
                        distributed_gap = next_distributed_gap;
                    }
                }
                return result;
            }
        }

        // A short final page is not stretched to the bottom. Keep its natural
        // Auto leading and leave the unavoidable remainder after EOF.
        int y = 0;
        for (PlainTextVisibleLine& positioned : result) {
            positioned.baseline_y =
                y + fx_ceil(positioned.line->ascent);
            y += line_advance_px(*positioned.line);
        }
        return result;
    }

    int y = 0;
    int last_ink_bottom = 0;
    bool viewport_filled = false;
    while (!viewport_filled && screen < screens_.size()) {
        if (line >= screens_[screen].lines.size()) {
            ++screen;
            line = 0;
            continue;
        }
        const LayoutLine& item = screens_[screen].lines[line];
        const int advance = line_advance_px(item);
        const int baseline = y + fx_ceil(item.ascent);
        result.push_back(
            {&item, baseline});
        last_ink_bottom = line_ink_bottom_px(item, y);
        y += advance;
        ++line;
        viewport_filled = bottom_align_visible_rows_
                              ? last_ink_bottom >= viewport_height_
                              : y >= viewport_height_;
    }
    // Bottom-aligned line steps continue until visible text reaches the bottom,
    // then shift by only the ink overflow. This clips the first row as needed
    // while placing the final row's descent on the physical bottom, without
    // exposing its trailing line gap. Page and upward destinations keep the
    // first row at y=0. Both paths remain local to the bounded screen cache.
    const int top_clip = bottom_align_visible_rows_
                             ? std::max(0, last_ink_bottom - viewport_height_)
                             : 0;
    if (top_clip != 0) {
        for (PlainTextVisibleLine& positioned : result) {
            positioned.baseline_y -= top_clip;
        }
    }
    return result;
}

bool PlainTextLayout::prepare_screen_window(std::string& error) {
    error.clear();
    if (!loaded()) return false;
    bool changed = false;
    while (future_screen_count() < kPlainTextFutureScreens &&
           !screens_.empty() && !screens_.back().eof) {
        if (!ensure_future_screen(error)) {
            return changed;
        }
        changed = true;
    }
    return changed;
}

std::size_t PlainTextLayout::preload_deferred_page_glyphs(
    std::size_t maximum_glyphs) {
    synchronize_glyph_cache_generation();
    if (maximum_glyphs == 0 || text_ == nullptr ||
        deferred_forward_pages_ == 0 ||
        !deferred_target_valid_ ||
        deferred_target_glyphs_ready_) {
        return 0;
    }

    std::size_t warmed = 0;
    while (!deferred_target_glyphs_ready_ &&
           warmed < maximum_glyphs) {
        if (deferred_warm_screen_ >= screens_.size()) {
            if (!screens_.empty() && screens_.back().eof) {
                finish_deferred_warm_pass();
            }
            break;
        }
        Screen& screen = screens_[deferred_warm_screen_];
        if (deferred_warm_line_ >= screen.lines.size()) {
            if (deferred_warm_screen_ + 1 >= screens_.size()) {
                if (screens_.back().eof) {
                    finish_deferred_warm_pass();
                }
                break;
            }
            ++deferred_warm_screen_;
            deferred_warm_line_ = 0;
            deferred_warm_run_ = 0;
            deferred_warm_glyph_ = 0;
            continue;
        }

        const LayoutLine& line =
            screen.lines[deferred_warm_line_];
        if (deferred_warm_run_ >= line.runs.size()) {
            deferred_warm_height_ += line_advance_px(line);
            ++deferred_warm_rows_;
            ++deferred_warm_line_;
            deferred_warm_run_ = 0;
            deferred_warm_glyph_ = 0;
            if (auto_page_fit()
                    ? deferred_warm_rows_ >= auto_rows_per_page_
                    : deferred_warm_height_ >= viewport_height_) {
                finish_deferred_warm_pass();
            }
            continue;
        }

        const LayoutRun& run = line.runs[deferred_warm_run_];
        const std::size_t glyph_count = run.glyphs.glyphs.size();
        if (deferred_warm_glyph_ >= glyph_count) {
            ++deferred_warm_run_;
            deferred_warm_glyph_ = 0;
            continue;
        }
        const std::size_t requested = std::min(
            maximum_glyphs - warmed,
            glyph_count - deferred_warm_glyph_);
        const std::size_t cached = text_->cache_run_range(
            run.glyphs, run.pixel_size,
            deferred_warm_glyph_, requested);
        if (cached == 0) {
            deferred_warm_failed_ = true;
            ++deferred_warm_run_;
            deferred_warm_glyph_ = 0;
            // A failed rasterization can still have incurred streamed-font
            // I/O. Yield after the attempt so one bad glyph cannot turn an
            // idle quantum into an unbounded scan across later runs.
            return warmed;
        }
        deferred_warm_glyph_ += cached;
        warmed += cached;
        if (deferred_warm_glyph_ >= glyph_count) {
            ++deferred_warm_run_;
            deferred_warm_glyph_ = 0;
        }
        if (cached < requested) break;
    }
    return warmed;
}

std::size_t PlainTextLayout::preload_next_glyphs(
    std::size_t maximum_glyphs) {
    synchronize_glyph_cache_generation();
    if (screens_.empty() || text_ == nullptr || maximum_glyphs == 0) {
        return 0;
    }
    if (deferred_forward_pages_ != 0) return 0;
    if (future_screen_count() < kPlainTextMinimumReadyScreens &&
        !screens_.back().eof) {
        return 0;
    }
    const std::uint32_t visible_end = visible_source_end();
    const std::size_t last_screen = std::min<std::size_t>(
        screens_.size(),
        current_screen_ + 1 + kPlainTextGlyphWarmFutureScreens);
    std::size_t warmed = 0;
    for (std::size_t screen = current_screen_; screen < last_screen; ++screen) {
        Screen& item = screens_[screen];
        while (item.next_preload_line < item.lines.size() &&
               line_end(screen, item.next_preload_line) <= visible_end) {
            ++item.next_preload_line;
            item.next_preload_run = 0;
            item.next_preload_glyph = 0;
        }
        while (item.next_preload_line < item.lines.size() &&
               warmed < maximum_glyphs) {
            const LayoutLine& line =
                item.lines[item.next_preload_line];
            while (item.next_preload_run < line.runs.size() &&
                   warmed < maximum_glyphs) {
                const LayoutRun& run =
                    line.runs[item.next_preload_run];
                const std::size_t glyph_count =
                    run.glyphs.glyphs.size();
                if (item.next_preload_glyph >= glyph_count) {
                    ++item.next_preload_run;
                    item.next_preload_glyph = 0;
                    continue;
                }
                const std::size_t requested = std::min(
                    maximum_glyphs - warmed,
                    glyph_count - item.next_preload_glyph);
                const std::size_t cached = text_->cache_run_range(
                    run.glyphs, run.pixel_size,
                    item.next_preload_glyph, requested);
                if (cached == 0) {
                    // A missing face or exhausted glyph cache should not pin
                    // the idle scheduler to this run forever. Rendering will
                    // retain its normal fallback behavior.
                    ++item.next_preload_run;
                    item.next_preload_glyph = 0;
                    return warmed;
                }
                item.next_preload_glyph += cached;
                warmed += cached;
                if (item.next_preload_glyph >= glyph_count) {
                    ++item.next_preload_run;
                    item.next_preload_glyph = 0;
                }
                if (cached < requested) return warmed;
            }
            if (item.next_preload_run >= line.runs.size()) {
                ++item.next_preload_line;
                item.next_preload_run = 0;
                item.next_preload_glyph = 0;
            }
        }
    }
    return warmed;
}

bool PlainTextLayout::perform_incremental_work(std::string& error) {
    error.clear();
    if (!loaded()) return false;
    if (deferred_forward_pages_ != 0) {
        if (!deferred_target_valid_) {
            PageTarget target;
            bool needs_more_layout = false;
            if (find_forward_page_target(
                    target, needs_more_layout) &&
                cached_viewport_ready(target)) {
                begin_deferred_target(target);
                return true;
            }
            if (!screens_.empty() && !screens_.back().eof) {
                return build_future_screen_step(error);
            }
            deferred_forward_pages_ = 0;
            reset_deferred_target();
            return false;
        }
        if (!deferred_target_glyphs_ready_) {
            return false;
        }
        if (move_page_now(1, false, error)) {
            --deferred_forward_pages_;
            reset_deferred_target();
            return true;
        }
        if (!error.empty()) return false;
        if (at_end()) {
            deferred_forward_pages_ = 0;
            reset_deferred_target();
            return false;
        }
        reset_deferred_target();
        return true;
    }
    // Maintain a small hard reserve before any farther speculation. Ordinary
    // touchpad bursts can then turn pages without shaping on the input path.
    if (future_screen_count() < kPlainTextMinimumReadyScreens &&
        !screens_.empty() &&
        !screens_.back().eof) {
        const bool advanced = build_future_screen_step(error);
        return advanced;
    }
    if (prefetch_raw_next_) {
        if (prefetch_raw_cache(error)) {
            prefetch_raw_next_ = false;
            return true;
        }
        if (!error.empty()) return false;
    }
    if (future_screen_count() < kPlainTextFutureScreens &&
        !screens_.empty() && !screens_.back().eof) {
        const bool advanced = build_future_screen_step(error);
        prefetch_raw_next_ = true;
        return advanced;
    }
    return prefetch_raw_cache(error);
}

std::vector<SearchMatch> PlainTextLayout::search(
    std::string_view query,
    SearchMode mode,
    std::size_t maximum_results,
    std::string& error) const {
    error.clear();
    std::vector<SearchMatch> results;
    if (!loaded() || query.empty() || maximum_results == 0) return results;

    std::vector<std::uint8_t> bytes;
    bytes.resize(std::min<std::size_t>(kSearchChunkBytes, source_size_));
    std::string tail;
    std::uint32_t offset = 0;
    while (offset < source_size_ && results.size() < maximum_results) {
        const std::size_t count = std::min<std::size_t>(
            bytes.size(), source_size_ - offset);
        if (!read_logical(offset, bytes.data(), count)) {
            error = "could not scan the plain-text source";
            results.clear();
            return results;
        }
        std::string window = tail;
        window.append(reinterpret_cast<const char*>(bytes.data()), count);
        const std::uint32_t window_offset =
            offset - static_cast<std::uint32_t>(tail.size());
        std::vector<SearchMatch> part = search_source(
            window, query, mode, maximum_results - results.size());
        for (SearchMatch& match : part) {
            const std::uint32_t global_offset =
                window_offset + match.source_offset;
            const std::uint64_t global_end =
                static_cast<std::uint64_t>(global_offset) +
                match.source_length;
            if (offset != 0 && global_end <= offset) continue;
            match.source_offset = global_offset;
            if (!results.empty() &&
                results.back().source_offset == match.source_offset &&
                results.back().source_length == match.source_length) {
                continue;
            }
            results.push_back(std::move(match));
            if (results.size() == maximum_results) break;
        }
        const std::size_t keep =
            std::min<std::size_t>(kSearchBoundaryOverlap, window.size());
        std::size_t tail_begin = window.size() - keep;
        while (tail_begin < window.size() &&
               continuation(static_cast<std::uint8_t>(
                   window[tail_begin]))) {
            ++tail_begin;
        }
        tail.assign(window.data() + tail_begin,
                    window.size() - tail_begin);
        offset += static_cast<std::uint32_t>(count);
    }
    return results;
}

std::string PlainTextLayout::snippet(std::uint32_t source_offset,
                                     std::string& error) const {
    error.clear();
    if (!loaded() || source_size_ == 0) return "Empty document";
    const std::uint32_t center = std::min(source_offset, source_size_);
    const std::uint32_t begin = center > 24 ? center - 24 : 0;
    const std::size_t count =
        std::min<std::size_t>(112, source_size_ - begin);
    std::array<std::uint8_t, 112> bytes{};
    if (!read_logical(begin, bytes.data(), count)) {
        error = "could not read plain-text context";
        return {};
    }
    std::size_t safe_begin = 0;
    while (safe_begin < count && continuation(bytes[safe_begin])) ++safe_begin;
    const std::string_view view(
        reinterpret_cast<const char*>(bytes.data() + safe_begin),
        complete_utf8_prefix(std::string_view(
            reinterpret_cast<const char*>(bytes.data() + safe_begin),
            count - safe_begin)));
    std::string result(view);
    for (char& value : result) {
        if (value == '\r' || value == '\n' || value == '\t') value = ' ';
    }
    if (begin + safe_begin != 0) result.insert(0, u8"… ");
    if (begin + count < source_size_) result += u8" …";
    return result;
}

bool PlainTextLayout::initial_cache_contains_cjk() const {
    const std::uint8_t* bytes = source_memory_;
    std::size_t size = source_memory_ != nullptr
                           ? source_size_
                           : raw_cache_valid_size_;
    std::vector<std::uint8_t> linear;
    if (source_memory_ == nullptr && size != 0) {
        const std::size_t scan_size =
            std::min(size, kPlainTextInitialCacheBytes);
        try {
            linear.resize(scan_size);
        } catch (const std::bad_alloc&) {
            return false;
        }
        copy_cached_bytes(0, linear.data(), scan_size);
        bytes = linear.data();
        size = scan_size;
    }
    if (bytes == nullptr || size == 0) return false;
    const std::size_t safe = complete_utf8_prefix(std::string_view(
        reinterpret_cast<const char*>(bytes), size));
    std::size_t offset = 0;
    while (offset < safe) {
        const DecodedCodepoint decoded = utf8_next(
            bytes, safe, static_cast<std::uint32_t>(offset));
        const std::uint32_t value = decoded.value;
        if ((value >= 0x3400U && value <= 0x9FFFU) ||
            (value >= 0x3040U && value <= 0x30FFU) ||
            (value >= 0xAC00U && value <= 0xD7AFU)) {
            return true;
        }
        offset += decoded.byte_length == 0 ? 1 : decoded.byte_length;
    }
    return false;
}

}  // namespace nmarkdown
