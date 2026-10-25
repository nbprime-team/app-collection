#include "nmarkdown/text/glyph_cache.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace nmarkdown {

GlyphCache::GlyphCache(std::size_t page_count) {
    page_count = std::max<std::size_t>(1, page_count);
    pages_.resize(page_count);
}

std::size_t GlyphCache::GlyphKeyHash::operator()(const GlyphKey& key) const {
    std::size_t hash = static_cast<std::size_t>(key.face) * 0x9E3779B1U;
    hash ^= static_cast<std::size_t>(key.glyph) + 0x9E3779B9U + (hash << 6U) +
            (hash >> 2U);
    hash ^= static_cast<std::size_t>(key.pixel_size_26_6) << 7U;
    hash ^= static_cast<std::size_t>(key.x_phase) << 3U;
    hash ^= key.render_flags;
    return hash;
}

bool GlyphCache::try_allocate(Page& page,
                              std::uint16_t width,
                              std::uint16_t height,
                              std::uint16_t& x,
                              std::uint16_t& y) {
    constexpr int kPadding = 1;
    if (width > kAtlasWidth || height > kAtlasHeight) {
        return false;
    }
    if (page.pixels.empty()) {
        try {
            page.pixels.assign(kAtlasWidth * kAtlasHeight, 0);
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    int cursor_x = page.cursor_x;
    int cursor_y = page.cursor_y;
    int row_height = page.row_height;
    if (cursor_x + width > kAtlasWidth) {
        cursor_x = 0;
        cursor_y += row_height;
        row_height = 0;
    }
    if (cursor_y + height > kAtlasHeight) {
        return false;
    }

    x = static_cast<std::uint16_t>(cursor_x);
    y = static_cast<std::uint16_t>(cursor_y);
    page.cursor_x = cursor_x + width + kPadding;
    page.cursor_y = cursor_y;
    page.row_height = std::max(row_height, static_cast<int>(height) + kPadding);
    return true;
}

void GlyphCache::evict_page(std::size_t page_index) {
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->second.handle.page == page_index) {
            iterator = entries_.erase(iterator);
            ++evictions_;
        } else {
            ++iterator;
        }
    }

    Page& page = pages_[page_index];
    page.cursor_x = 0;
    page.cursor_y = 0;
    page.row_height = 0;
    page.last_used = 0;
    ++page.epoch;
    if (page.epoch == 0) {
        page.epoch = 1;
    }
}

bool GlyphCache::allocate(std::uint16_t width,
                          std::uint16_t height,
                          std::uint16_t& page,
                          std::uint16_t& x,
                          std::uint16_t& y) {
    std::size_t first_empty = pages_.size();
    for (std::size_t index = 0; index < pages_.size(); ++index) {
        if (pages_[index].pixels.empty()) {
            if (first_empty == pages_.size()) first_empty = index;
            continue;
        }
        if (try_allocate(pages_[index], width, height, x, y)) {
            page = static_cast<std::uint16_t>(index);
            return true;
        }
    }

    if (first_empty != pages_.size() &&
        try_allocate(pages_[first_empty], width, height, x, y)) {
        page = static_cast<std::uint16_t>(first_empty);
        return true;
    }

    // A lazy page allocation can fail under calculator heap pressure. Reuse
    // the least-recently-used allocated page instead of repeatedly selecting
    // an empty logical page that still cannot obtain its backing bitmap.
    const auto oldest = std::min_element(
        pages_.begin(), pages_.end(), [](const Page& left, const Page& right) {
            if (left.pixels.empty()) return false;
            if (right.pixels.empty()) return true;
            return left.last_used < right.last_used;
        });
    if (oldest == pages_.end() || oldest->pixels.empty()) {
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(oldest - pages_.begin());
    evict_page(index);
    if (!try_allocate(pages_[index], width, height, x, y)) {
        return false;
    }
    page = static_cast<std::uint16_t>(index);
    return true;
}

bool GlyphCache::get(const FontFace& face,
                     GlyphId glyph,
                     Fx pixel_size,
                     std::uint8_t x_phase,
                     std::uint8_t render_flags,
                     GlyphCacheHandle& handle) {
    if (pixel_size <= 0 || pixel_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const GlyphKey key{face.id(),
                       glyph,
                       static_cast<std::uint16_t>(pixel_size),
                       static_cast<std::uint8_t>(x_phase & 3U),
                       render_flags};
    ++access_generation_;
    const auto found = entries_.find(key);
    if (found != entries_.end()) {
        ++hits_;
        found->second.last_used = access_generation_;
        handle = found->second.handle;
        if (handle.page != GlyphCacheHandle::kNoPage) {
            pages_[handle.page].last_used = access_generation_;
        }
        return true;
    }

    ++misses_;
    GlyphBitmap bitmap;
    if (!face.rasterize(glyph, pixel_size, x_phase, render_flags, bitmap)) {
        return false;
    }

    Entry entry;
    entry.handle.metrics = bitmap.metrics;
    entry.last_used = access_generation_;
    if (!bitmap.coverage.empty()) {
        std::uint16_t page = 0;
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        if (!allocate(bitmap.metrics.width, bitmap.metrics.height, page, x, y)) {
            return false;
        }
        Page& destination = pages_[page];
        for (std::uint16_t row = 0; row < bitmap.metrics.height; ++row) {
            std::memcpy(destination.pixels.data() +
                            (static_cast<std::size_t>(y + row) * kAtlasWidth + x),
                        bitmap.coverage.data() +
                            static_cast<std::size_t>(row) * bitmap.metrics.width,
                        bitmap.metrics.width);
        }
        destination.last_used = access_generation_;
        entry.handle.page = page;
        entry.handle.x = x;
        entry.handle.y = y;
        entry.handle.page_epoch = destination.epoch;
    }

    handle = entry.handle;
    entries_.emplace(key, entry);
    return true;
}

bool GlyphCache::contains(const FontFace& face,
                          GlyphId glyph,
                          Fx pixel_size,
                          std::uint8_t x_phase,
                          std::uint8_t render_flags) const {
    if (pixel_size <= 0 ||
        pixel_size > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    const GlyphKey key{face.id(),
                       glyph,
                       static_cast<std::uint16_t>(pixel_size),
                       static_cast<std::uint8_t>(x_phase & 3U),
                       render_flags};
    const auto found = entries_.find(key);
    if (found == entries_.end()) return false;
    const GlyphCacheHandle& handle = found->second.handle;
    return handle.page == GlyphCacheHandle::kNoPage ||
           (handle.page < pages_.size() &&
            pages_[handle.page].epoch == handle.page_epoch);
}

const std::uint8_t* GlyphCache::coverage(const GlyphCacheHandle& handle) const {
    if (!handle.drawable() || handle.page >= pages_.size()) {
        return nullptr;
    }
    const Page& page = pages_[handle.page];
    if (page.epoch != handle.page_epoch || handle.x + handle.metrics.width > kAtlasWidth ||
        handle.y + handle.metrics.height > kAtlasHeight) {
        return nullptr;
    }
    return page.pixels.data() + static_cast<std::size_t>(handle.y) * kAtlasWidth +
           handle.x;
}

GlyphCacheStats GlyphCache::stats() const {
    return {hits_, misses_, evictions_, entries_.size(), pages_.size()};
}

void GlyphCache::clear() {
    entries_.clear();
    for (Page& page : pages_) {
        page.cursor_x = 0;
        page.cursor_y = 0;
        page.row_height = 0;
        page.last_used = 0;
        ++page.epoch;
        if (page.epoch == 0) {
            page.epoch = 1;
        }
    }
    access_generation_ = 0;
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
    ++clear_generation_;
    if (clear_generation_ == 0) {
        clear_generation_ = 1;
    }
}

}  // namespace nmarkdown
