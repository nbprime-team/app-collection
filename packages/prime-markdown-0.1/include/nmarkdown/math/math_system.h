#ifndef NMARKDOWN_MATH_MATH_SYSTEM_H
#define NMARKDOWN_MATH_MATH_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nmarkdown/math/math_layout.h"
#include "nmarkdown/render/surface565.h"

namespace nmarkdown {

class TextSystem;

struct FormulaCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::size_t entries = 0;
};

class MathSystem {
public:
    explicit MathSystem(TextSystem& text);

    bool layout(std::string_view latex,
                MathStyle style,
                Fx pixel_size,
                Fx maximum_width,
                std::shared_ptr<const MathLayoutResult>& result);
    bool draw(const Surface565& surface,
              const MathLayoutResult& layout,
              int origin_x,
              int baseline_y,
              int pan_x,
              std::uint16_t foreground,
              std::uint16_t background,
              bool dark_theme,
              Rect clip);
    void clear_cache();
    FormulaCacheStats cache_stats() const;

private:
    struct CacheEntry {
        std::string source;
        MathStyle style = MathStyle::Text;
        Fx pixel_size = 0;
        Fx maximum_width = 0;
        std::uint64_t last_used = 0;
        std::shared_ptr<MathLayoutResult> result;
    };

    TextSystem& text_;
    MathFontConstants constants_{};
    std::vector<CacheEntry> cache_;
    std::uint64_t use_clock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
};

}  // namespace nmarkdown

#endif
