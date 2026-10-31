#ifndef NMARKDOWN_TEXT_FONT_PACK_H
#define NMARKDOWN_TEXT_FONT_PACK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nmarkdown {

enum class FontRole : std::uint32_t {
    BodySans = 1,
    BodySerif = 2,
    Monospace = 3,
    Math = 4,
    Cjk = 5,
    // A virtual shaping role: use Latin Modern Math first, then allow the
    // normal CJK/body fallback chain. It is never serialized into a font pack.
    MathText = 6,
    // Optional native companion face for emphasized Body text. Body stays in
    // BodySans and uses outline synthesis when this external role is empty.
    BodySansItalic = 7,
    BodySansBold = 8,
    BodySansBoldItalic = 9,
    // Optional companion for emphasized code/monospace text. It falls back to
    // the regular Monospace assignment plus outline oblique synthesis.
    MonospaceItalic = 10,
    // Runtime-only identity for a loaded external resource. Its actual uses
    // live in FontRoleBindings, so one face can satisfy several roles.
    External = 254,
    Replacement = 255,
};

enum class FontStyle : std::uint8_t {
    Regular = 0,
    Italic = 1U << 0U,
    Bold = 1U << 1U,
    BoldItalic = Italic | Bold,
};

struct CodepointRange {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
};

struct FontPackFace {
    std::uint32_t id = 0;
    FontRole role = FontRole::BodySans;
    const std::uint8_t* font_data = nullptr;
    std::size_t font_size = 0;
    std::string_view name;
    std::string_view license;
    std::vector<CodepointRange> ranges;

    bool declares_codepoint(std::uint32_t codepoint) const;
};

class FontPack {
public:
    static constexpr std::uint16_t kFormatVersion = 1;

    bool load_from_memory(const std::uint8_t* data,
                          std::size_t size,
                          std::string& error);
    bool load_from_file(const char* path, std::string& error);
    void reset();

    bool valid() const { return data_ != nullptr && !faces_.empty(); }
    std::size_t face_count() const { return faces_.size(); }
    const FontPackFace* face(std::size_t index) const;
    const FontPackFace* face_by_id(std::uint32_t id) const;
    std::uint32_t checksum() const { return checksum_; }

private:
    bool parse(const std::uint8_t* data, std::size_t size, std::string& error);

    std::vector<std::uint8_t> owned_data_;
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::uint32_t checksum_ = 0;
    std::vector<FontPackFace> faces_;
};

}  // namespace nmarkdown

#endif
