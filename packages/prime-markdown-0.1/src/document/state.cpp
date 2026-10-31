#include "nmarkdown/document/state.h"

#include <algorithm>

namespace nmarkdown {
namespace {

constexpr std::uint8_t kMagic[] = {'N', 'M', 'S', '1'};
constexpr std::uint16_t kVersion = 7;
constexpr std::size_t kVersion1FixedSizeWithoutChecksum = 38;
constexpr std::size_t kVersion2FixedSizeWithoutChecksum = 40;
constexpr std::size_t kVersion3FixedSizeWithoutChecksum = 41;
constexpr std::size_t kFixedSizeWithoutChecksum = 41;
constexpr std::size_t kMaximumBookmarks = 256;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

bool read_u16(const std::uint8_t* bytes,
              std::size_t size,
              std::size_t& offset,
              std::uint16_t& value) {
    if (offset > size || size - offset < 2) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8U;
    offset += 2;
    return true;
}

bool read_u32(const std::uint8_t* bytes,
              std::size_t size,
              std::size_t& offset,
              std::uint32_t& value) {
    if (offset > size || size - offset < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return true;
}

bool read_u64(const std::uint8_t* bytes,
              std::size_t size,
              std::size_t& offset,
              std::uint64_t& value) {
    if (offset > size || size - offset < 8) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    }
    return true;
}

std::uint32_t checksum(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t hash = 2166136261U;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

}  // namespace

std::uint64_t document_identity(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr && size != 0) return 0;
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<std::uint64_t>(size);
    hash *= 1099511628211ULL;
    return hash;
}

std::uint64_t document_identity(const std::deque<std::string>& segments,
                                std::size_t total_size) {
    std::uint64_t hash = 14695981039346656037ULL;
    std::size_t observed_size = 0;
    for (const std::string& segment : segments) {
        for (const unsigned char byte : segment) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        observed_size += segment.size();
    }
    if (observed_size != total_size) return 0;
    hash ^= static_cast<std::uint64_t>(total_size);
    hash *= 1099511628211ULL;
    return hash;
}

bool encode_reader_state(const ReaderState& state,
                         std::vector<std::uint8_t>& bytes,
                         std::string& error) {
    bytes.clear();
    error.clear();
    if (state.bookmarks.size() > kMaximumBookmarks) {
        error = "reader state contains too many bookmarks";
        return false;
    }
    bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
    append_u16(bytes, kVersion);
    append_u16(bytes, static_cast<std::uint16_t>(kFixedSizeWithoutChecksum));
    append_u64(bytes, state.position.document_identity);
    append_u32(bytes, state.position.source_offset);
    append_u32(bytes, state.position.nearest_block);
    append_u16(bytes, state.position.relative_position_0_65535);
    bytes.push_back(state.dark_theme ? 1 : 0);
    bytes.push_back(std::max<std::uint8_t>(6, state.font_size));
    bytes.push_back(state.code_wrap ? 1 : 0);
    bytes.push_back(state.table_mode);
    bytes.push_back(state.line_gap < 0
                        ? 0U
                        : state.line_gap == 0
                              ? 255U
                              : static_cast<std::uint8_t>(std::max(
                                    1, std::min(10, state.line_gap))));
    bytes.push_back(std::max<std::uint8_t>(2,
        std::min<std::uint8_t>(18, state.side_margin)));
    bytes.push_back(state.reading_mode == ReadingMode::HorizontalScroll ? 1 : 0);
    append_u32(bytes, state.last_selected_heading);
    append_u16(bytes, static_cast<std::uint16_t>(state.bookmarks.size()));
    const std::uint16_t flags =
        (state.high_contrast ? 1U : 0U) |
        (state.natural_scrolling ? 2U : 0U) |
        (static_cast<std::uint16_t>(
             clamp_render_sharpness(state.render_sharpness))
         << 2U) |
        (state.natural_swiping ? 0x0040U : 0U);
    append_u16(bytes, flags);
    for (std::uint32_t bookmark : state.bookmarks) append_u32(bytes, bookmark);
    append_u32(bytes, checksum(bytes.data(), bytes.size()));
    return true;
}

bool decode_reader_state(const std::uint8_t* bytes,
                         std::size_t size,
                         ReaderState& state,
                         std::string& error) {
    state = {};
    error.clear();
    if (bytes == nullptr || size < kVersion1FixedSizeWithoutChecksum + 4) {
        error = "reader state is truncated";
        return false;
    }
    if (!std::equal(std::begin(kMagic), std::end(kMagic), bytes)) {
        error = "reader state has an invalid signature";
        return false;
    }
    std::size_t offset = 4;
    std::uint16_t version = 0;
    std::uint16_t header_size = 0;
    if (!read_u16(bytes, size, offset, version) ||
        !read_u16(bytes, size, offset, header_size) ||
        !((version == 1 && header_size == kVersion1FixedSizeWithoutChecksum) ||
          (version == 2 && header_size == kVersion2FixedSizeWithoutChecksum) ||
          (version == 3 && header_size == kVersion3FixedSizeWithoutChecksum) ||
          (version == 4 && header_size == kFixedSizeWithoutChecksum) ||
          (version == 5 && header_size == kFixedSizeWithoutChecksum) ||
          (version == 6 && header_size == kFixedSizeWithoutChecksum) ||
          (version == kVersion && header_size == kFixedSizeWithoutChecksum))) {
        error = "reader state version is unsupported";
        return false;
    }
    if (!read_u64(bytes, size, offset, state.position.document_identity) ||
        !read_u32(bytes, size, offset, state.position.source_offset) ||
        !read_u32(bytes, size, offset, state.position.nearest_block) ||
        !read_u16(bytes, size, offset, state.position.relative_position_0_65535) ||
        offset > size || size - offset < 4) {
        error = "reader state header is truncated";
        return false;
    }
    state.dark_theme = bytes[offset++] != 0;
    state.font_size = bytes[offset++];
    state.code_wrap = bytes[offset++] != 0;
    state.table_mode = bytes[offset++];
    if (version >= 2) {
        if (offset > size || size - offset < 2) {
            error = "reader state layout settings are truncated";
            return false;
        }
        const std::uint8_t stored_gap = bytes[offset++];
        state.line_gap = stored_gap == 0 ? -1
                         : stored_gap == 255U ? 0
                                              : static_cast<int>(stored_gap);
        state.side_margin = bytes[offset++];
    } else {
        // Version 1 predates the setting and used a four-pixel fixed gap.
        state.line_gap = 4;
    }
    if (version >= 3) {
        if (offset >= size || bytes[offset] > 1) {
            error = "reader state has an invalid reading mode";
            return false;
        }
        state.reading_mode = bytes[offset++] == 0
                                 ? ReadingMode::VerticalScroll
                                 : ReadingMode::HorizontalScroll;
    }
    std::uint16_t bookmark_count = 0;
    std::uint16_t reserved = 0;
    if (!read_u32(bytes, size, offset, state.last_selected_heading) ||
        !read_u16(bytes, size, offset, bookmark_count) ||
        !read_u16(bytes, size, offset, reserved) ||
        bookmark_count > kMaximumBookmarks ||
        (reserved & ~(version >= 7 ? 0x007FU
                                  : (version >= 6 ? 0x003FU
                                  : (version >= 5 ? 7U
                                                  : (version >= 4 ? 3U
                                                                  : 1U))))) != 0) {
        error = "reader state bookmark table is invalid";
        return false;
    }
    state.high_contrast = (reserved & 1U) != 0;
    if (version >= 4) state.natural_scrolling = (reserved & 2U) != 0;
    if (version >= 7) state.natural_swiping = (reserved & 0x0040U) != 0;
    if (version >= 6) {
        const std::uint16_t sharpness = (reserved >> 2U) & 0x000FU;
        if (sharpness > kMaximumRenderSharpness) {
            error = "reader state has an invalid text sharpness";
            return false;
        }
        state.render_sharpness = static_cast<RenderSharpness>(sharpness);
    } else if (version == 5) {
        state.render_sharpness = (reserved & 4U) != 0
                                     ? kMinimumRenderSharpness
                                     : kMaximumRenderSharpness;
    } else {
        // Versions 1-4 predate this setting and rendered with the old Sharp
        // endpoint. Preserve their appearance rather than treating them as
        // newly created documents.
        state.render_sharpness = kMaximumRenderSharpness;
    }
    const std::size_t expected = header_size +
                                 static_cast<std::size_t>(bookmark_count) * 4 + 4;
    if (size != expected) {
        error = "reader state size is inconsistent";
        return false;
    }
    state.bookmarks.resize(bookmark_count);
    for (std::uint32_t& bookmark : state.bookmarks) {
        if (!read_u32(bytes, size, offset, bookmark)) {
            error = "reader state bookmark table is truncated";
            return false;
        }
    }
    std::uint32_t stored_checksum = 0;
    if (!read_u32(bytes, size, offset, stored_checksum) ||
        stored_checksum != checksum(bytes, size - 4)) {
        error = "reader state checksum does not match";
        return false;
    }
    state.font_size = std::max<std::uint8_t>(6, state.font_size);
    state.line_gap = state.line_gap < 0 ? -1 : std::min(10, state.line_gap);
    state.side_margin = std::max<std::uint8_t>(2,
        std::min<std::uint8_t>(18, state.side_margin));
    return true;
}

}  // namespace nmarkdown
