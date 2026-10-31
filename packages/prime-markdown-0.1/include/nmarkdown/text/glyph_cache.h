#ifndef NMARKDOWN_TEXT_GLYPH_CACHE_H
#define NMARKDOWN_TEXT_GLYPH_CACHE_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "nmarkdown/text/font.h"

namespace nmarkdown {

struct GlyphKey {
    FontFaceId face = 0;
    GlyphId glyph = 0;
    std::uint16_t pixel_size_26_6 = 0;
    std::uint8_t x_phase = 0;
    std::uint8_t render_flags = 0;

    bool operator==(const GlyphKey& other) const {
        return face == other.face && glyph == other.glyph &&
               pixel_size_26_6 == other.pixel_size_26_6 &&
               x_phase == other.x_phase && render_flags == other.render_flags;
    }
};

struct GlyphCacheHandle {
    static constexpr std::uint16_t kNoPage = 0xFFFFU;

    std::uint16_t page = kNoPage;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint32_t page_epoch = 0;
    GlyphMetrics metrics;

    bool drawable() const {
        return page != kNoPage && metrics.width != 0 && metrics.height != 0;
    }
};

struct GlyphCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t entries = 0;
    std::size_t pages = 0;
};

class GlyphCache {
public:
    static constexpr int kAtlasWidth = 256;
    static constexpr int kAtlasHeight = 256;

    // Up to 128 lazily allocated A8 pages retain 8 MiB of rasterized glyph
    // coverage. The pages consume no bitmap RAM until used; the ceiling is
    // large enough for common CJK glyphs across quarter-pixel phases without
    // constant atlas eviction while scrolling.
    explicit GlyphCache(std::size_t page_count = 128);

    bool get(const FontFace& face,
             GlyphId glyph,
             Fx pixel_size,
             std::uint8_t x_phase,
             std::uint8_t render_flags,
             GlyphCacheHandle& handle);
    bool contains(const FontFace& face,
                  GlyphId glyph,
                  Fx pixel_size,
                  std::uint8_t x_phase,
                  std::uint8_t render_flags) const;

    const std::uint8_t* coverage(const GlyphCacheHandle& handle) const;
    int coverage_stride() const { return kAtlasWidth; }
    GlyphCacheStats stats() const;
    std::uint64_t clear_generation() const {
        return clear_generation_;
    }
    void clear();

private:
    struct GlyphKeyHash {
        std::size_t operator()(const GlyphKey& key) const;
    };

    struct Entry {
        GlyphCacheHandle handle;
        std::uint64_t last_used = 0;
    };

    struct Page {
        std::vector<std::uint8_t> pixels;
        int cursor_x = 0;
        int cursor_y = 0;
        int row_height = 0;
        std::uint64_t last_used = 0;
        std::uint32_t epoch = 1;
    };

    bool allocate(std::uint16_t width,
                  std::uint16_t height,
                  std::uint16_t& page,
                  std::uint16_t& x,
                  std::uint16_t& y);
    static bool try_allocate(Page& page,
                             std::uint16_t width,
                             std::uint16_t height,
                             std::uint16_t& x,
                             std::uint16_t& y);
    void evict_page(std::size_t page);

    std::vector<Page> pages_;
    std::unordered_map<GlyphKey, Entry, GlyphKeyHash> entries_;
    std::uint64_t access_generation_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
    std::uint64_t clear_generation_ = 1;
};

}  // namespace nmarkdown

#endif
