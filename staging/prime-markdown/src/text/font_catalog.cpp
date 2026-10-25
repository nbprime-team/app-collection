#include "nmarkdown/text/font_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nmarkdown {
namespace {

constexpr std::size_t kMaximumTables = 256;
constexpr std::size_t kMaximumNameRecords = 1024;
constexpr std::size_t kMaximumNameBytes = 4096;
constexpr std::size_t kMaximumCmapBytes = 1024U * 1024U;

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0]) << 8U |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) << 24U |
           static_cast<std::uint32_t>(data[1]) << 16U |
           static_cast<std::uint32_t>(data[2]) << 8U |
           static_cast<std::uint32_t>(data[3]);
}

bool checked_range(std::uint64_t offset,
                   std::uint64_t size,
                   std::uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

bool read_bytes(FileSystem& files,
                const std::string& path,
                std::uint64_t file_size,
                std::uint64_t offset,
                std::size_t size,
                std::vector<std::uint8_t>& bytes,
                std::string& error) {
    if (!checked_range(offset, size, file_size)) {
        error = "font metadata is truncated";
        return false;
    }
    bytes.assign(size, 0);
    if (size == 0) return true;
    return files.read_range(path.c_str(), offset, bytes.data(), size, error);
}

struct TableRecord {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    bool found = false;
};

bool tag_equals(const std::uint8_t* tag, const char* expected) {
    return tag[0] == static_cast<std::uint8_t>(expected[0]) &&
           tag[1] == static_cast<std::uint8_t>(expected[1]) &&
           tag[2] == static_cast<std::uint8_t>(expected[2]) &&
           tag[3] == static_cast<std::uint8_t>(expected[3]);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

void append_utf8(std::uint32_t codepoint, std::string& output) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

std::string decode_utf16be(const std::vector<std::uint8_t>& bytes) {
    std::string output;
    for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
        std::uint32_t codepoint = read_u16(bytes.data() + index);
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU &&
            index + 3 < bytes.size()) {
            const std::uint32_t low = read_u16(bytes.data() + index + 2);
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                            (low - 0xDC00U);
                index += 2;
            }
        }
        if (codepoint != 0) append_utf8(codepoint, output);
    }
    return output;
}

std::string decode_name(std::uint16_t platform,
                        const std::vector<std::uint8_t>& bytes) {
    if (platform == 0 || platform == 3) return decode_utf16be(bytes);
    std::string output;
    output.reserve(bytes.size());
    for (std::uint8_t byte : bytes) {
        if (byte >= 0x20U && byte != 0x7FU) {
            output.push_back(static_cast<char>(byte));
        }
    }
    return output;
}

struct NameCandidate {
    std::uint16_t platform = 0;
    std::uint16_t language = 0;
    std::uint16_t length = 0;
    std::uint16_t offset = 0;
    int score = -1;
};

int name_score(std::uint16_t platform, std::uint16_t language) {
    int score = platform == 3 ? 30 : (platform == 0 ? 20 : 10);
    if (language == 0x0409U || language == 0U) score += 5;
    return score;
}

bool extract_name(FileSystem& files,
                  const std::string& path,
                  std::uint64_t file_size,
                  const TableRecord& table,
                  std::uint16_t preferred_name_id,
                  std::uint16_t fallback_name_id,
                  std::string& output,
                  std::string& error) {
    output.clear();
    if (!table.found || table.length < 6U) return false;
    std::vector<std::uint8_t> header;
    if (!read_bytes(files, path, file_size, table.offset, 6, header, error)) {
        return false;
    }
    const std::size_t count = read_u16(header.data() + 2);
    const std::size_t string_offset = read_u16(header.data() + 4);
    if (count > kMaximumNameRecords || string_offset > table.length ||
        count > (table.length - 6U) / 12U) {
        error = "font name table is invalid";
        return false;
    }
    std::vector<std::uint8_t> records;
    if (!read_bytes(files, path, file_size, table.offset + 6U, count * 12U,
                    records, error)) {
        return false;
    }
    NameCandidate candidate;
    int selected_id_score = -1;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t* record = records.data() + index * 12U;
        const std::uint16_t platform = read_u16(record);
        const std::uint16_t language = read_u16(record + 4);
        const std::uint16_t name_id = read_u16(record + 6);
        const int id_score = name_id == preferred_name_id
                                 ? 2
                                 : (name_id == fallback_name_id ? 1 : -1);
        if (id_score < 0) continue;
        const std::uint16_t length = read_u16(record + 8);
        const std::uint16_t offset = read_u16(record + 10);
        if (length == 0 || length > kMaximumNameBytes ||
            offset > table.length - string_offset ||
            length > table.length - string_offset - offset) {
            continue;
        }
        const int score = name_score(platform, language);
        if (id_score > selected_id_score ||
            (id_score == selected_id_score && score > candidate.score)) {
            selected_id_score = id_score;
            candidate = {platform, language, length, offset, score};
        }
    }
    if (selected_id_score < 0) return false;
    std::vector<std::uint8_t> bytes;
    if (!read_bytes(files, path, file_size,
                    static_cast<std::uint64_t>(table.offset) + string_offset +
                        candidate.offset,
                    candidate.length, bytes, error)) {
        return false;
    }
    output = decode_name(candidate.platform, bytes);
    return !output.empty();
}

bool intersects_cjk(std::uint32_t first, std::uint32_t last) {
    static constexpr std::array<std::array<std::uint32_t, 2>, 7> ranges{{
        {{0x3040U, 0x30FFU}},  // Hiragana and katakana
        {{0x31F0U, 0x31FFU}},  // Katakana phonetic extensions
        {{0x3400U, 0x4DBFU}},  // CJK extension A
        {{0x4E00U, 0x9FFFU}},  // Unified ideographs
        {{0xAC00U, 0xD7AFU}},  // Hangul syllables
        {{0xF900U, 0xFAFFU}},  // Compatibility ideographs
        {{0x20000U, 0x3134FU}},
    }};
    for (const auto& range : ranges) {
        if (first <= range[1] && last >= range[0]) return true;
    }
    return false;
}

struct CoverageFlags {
    bool latin = false;
    bool cjk = false;
};

void add_coverage(std::uint32_t first,
                  std::uint32_t last,
                  CoverageFlags& coverage) {
    // Treat a face as a Latin text face only when it contains basic Latin
    // letters. Punctuation-only/CJK subsets often retain a few Latin-1 code
    // points and must not be suggested for Body or Monospace.
    coverage.latin = coverage.latin ||
                     (first <= 0x005AU && last >= 0x0041U) ||
                     (first <= 0x007AU && last >= 0x0061U);
    coverage.cjk = coverage.cjk || intersects_cjk(first, last);
}

bool cmap_subtable_coverage(const std::vector<std::uint8_t>& bytes,
                            CoverageFlags& coverage) {
    if (bytes.size() < 4U) return false;
    const std::uint16_t format = read_u16(bytes.data());
    if (format == 4) {
        if (bytes.size() < 16U) return false;
        const std::size_t segment_count = read_u16(bytes.data() + 6) / 2U;
        if (segment_count == 0 || 16U + segment_count * 4U > bytes.size()) {
            return false;
        }
        const std::size_t end_offset = 14U;
        const std::size_t start_offset = end_offset + segment_count * 2U + 2U;
        if (start_offset + segment_count * 2U > bytes.size()) return false;
        for (std::size_t index = 0; index < segment_count; ++index) {
            const std::uint32_t last =
                read_u16(bytes.data() + end_offset + index * 2U);
            const std::uint32_t first =
                read_u16(bytes.data() + start_offset + index * 2U);
            if (first <= last) add_coverage(first, last, coverage);
        }
    } else if (format == 12 || format == 13) {
        if (bytes.size() < 16U) return false;
        const std::size_t group_count = read_u32(bytes.data() + 12);
        if (group_count > (bytes.size() - 16U) / 12U) return false;
        for (std::size_t index = 0; index < group_count; ++index) {
            const std::uint8_t* group = bytes.data() + 16U + index * 12U;
            add_coverage(read_u32(group), read_u32(group + 4), coverage);
        }
    }
    return coverage.latin || coverage.cjk;
}

CoverageFlags cmap_coverage(FileSystem& files,
                            const std::string& path,
                            std::uint64_t file_size,
                            const TableRecord& table,
                            std::string& error) {
    CoverageFlags coverage;
    if (!table.found || table.length < 4U) return coverage;
    std::vector<std::uint8_t> header;
    if (!read_bytes(files, path, file_size, table.offset, 4, header, error)) {
        return coverage;
    }
    const std::size_t count = read_u16(header.data() + 2);
    if (count > kMaximumTables || count > (table.length - 4U) / 8U) {
        return coverage;
    }
    std::vector<std::uint8_t> records;
    if (!read_bytes(files, path, file_size, table.offset + 4U, count * 8U,
                    records, error)) {
        return coverage;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t relative =
            read_u32(records.data() + index * 8U + 4U);
        if (relative > table.length - 4U) continue;
        std::vector<std::uint8_t> format_bytes;
        if (!read_bytes(files, path, file_size, table.offset + relative, 4,
                        format_bytes, error)) {
            continue;
        }
        const std::uint16_t format = read_u16(format_bytes.data());
        std::uint32_t length = 0;
        if (format == 4) {
            length = read_u16(format_bytes.data() + 2);
        } else if (format == 12 || format == 13) {
            std::vector<std::uint8_t> length_bytes;
            if (!read_bytes(files, path, file_size, table.offset + relative,
                            8, length_bytes, error)) {
                continue;
            }
            length = read_u32(length_bytes.data() + 4);
        } else {
            continue;
        }
        if (length < 4U || length > kMaximumCmapBytes ||
            relative > table.length || length > table.length - relative) {
            continue;
        }
        std::vector<std::uint8_t> subtable;
        if (!read_bytes(files, path, file_size, table.offset + relative,
                        length, subtable, error)) {
            continue;
        }
        cmap_subtable_coverage(subtable, coverage);
        if (coverage.latin && coverage.cjk) break;
    }
    return coverage;
}

}  // namespace

bool inspect_font_face(FileSystem& files,
                       const std::string& path,
                       FontFaceCatalogEntry& face,
                       std::string& error) {
    face = {};
    error.clear();
    DocumentProbe probe;
    if (!files.probe(path.c_str(), probe, error)) return false;
    if (probe.size < 12U) {
        error = "font file is too small";
        return false;
    }
    std::vector<std::uint8_t> header;
    if (!read_bytes(files, path, probe.size, 0, 12, header, error)) return false;
    const bool sfnt = read_u32(header.data()) == 0x00010000U ||
                      tag_equals(header.data(), "OTTO") ||
                      tag_equals(header.data(), "true") ||
                      tag_equals(header.data(), "typ1");
    if (!sfnt) {
        error = tag_equals(header.data(), "ttcf")
                    ? "font collections are not supported"
                    : "font is not a TrueType or OpenType face";
        return false;
    }
    const std::size_t table_count = read_u16(header.data() + 4);
    if (table_count == 0 || table_count > kMaximumTables ||
        table_count > (probe.size - 12U) / 16U) {
        error = "font table directory is invalid";
        return false;
    }
    std::vector<std::uint8_t> directory;
    if (!read_bytes(files, path, probe.size, 12, table_count * 16U,
                    directory, error)) {
        return false;
    }
    TableRecord name;
    TableRecord os2;
    TableRecord post;
    TableRecord cmap;
    TableRecord fvar;
    for (std::size_t index = 0; index < table_count; ++index) {
        const std::uint8_t* record = directory.data() + index * 16U;
        TableRecord* target = nullptr;
        if (tag_equals(record, "name")) target = &name;
        if (tag_equals(record, "OS/2")) target = &os2;
        if (tag_equals(record, "post")) target = &post;
        if (tag_equals(record, "cmap")) target = &cmap;
        if (tag_equals(record, "fvar")) target = &fvar;
        if (target == nullptr) continue;
        target->offset = read_u32(record + 8);
        target->length = read_u32(record + 12);
        target->found = checked_range(target->offset, target->length, probe.size);
    }
    std::string family;
    if (!extract_name(files, path, probe.size, name, 16, 1, family, error)) {
        if (error.empty()) error = "font family name is missing";
        return false;
    }
    std::string subfamily;
    std::string optional_error;
    extract_name(files, path, probe.size, name, 17, 2, subfamily,
                 optional_error);

    std::uint16_t weight = 400;
    std::uint16_t width = 5;
    std::uint16_t selection = 0;
    bool fixed_pitch = false;
    if (os2.found && os2.length >= 64U) {
        std::vector<std::uint8_t> style;
        if (read_bytes(files, path, probe.size, os2.offset, 64, style,
                       optional_error)) {
            weight = read_u16(style.data() + 4);
            width = read_u16(style.data() + 6);
            selection = read_u16(style.data() + 62);
            fixed_pitch = style[35] == 9U;
        }
    }
    std::int32_t italic_angle = 0;
    if (post.found && post.length >= 16U) {
        std::vector<std::uint8_t> style;
        if (read_bytes(files, path, probe.size, post.offset, 16, style,
                       optional_error)) {
            italic_angle = static_cast<std::int32_t>(read_u32(style.data() + 4));
            fixed_pitch = fixed_pitch || read_u32(style.data() + 12) != 0;
        }
    }
    const std::string lower_subfamily = lower_ascii(subfamily);
    const bool italic = (selection & 0x0001U) != 0 ||
                        (selection & 0x0200U) != 0 || italic_angle != 0 ||
                        lower_subfamily.find("italic") != std::string::npos ||
                        lower_subfamily.find("oblique") != std::string::npos;
    const bool bold = (selection & 0x0020U) != 0 || weight >= 600U ||
                      lower_subfamily.find("bold") != std::string::npos;
    const CoverageFlags coverage =
        cmap_coverage(files, path, probe.size, cmap, optional_error);
    face.path = path;
    face.family = std::move(family);
    face.subfamily = std::move(subfamily);
    face.weight = weight == 0 ? 400 : weight;
    face.width = width == 0 ? 5 : width;
    face.italic = italic;
    face.bold = bold;
    face.fixed_pitch = fixed_pitch;
    face.has_latin = coverage.latin;
    face.has_cjk = coverage.cjk;
    face.variable = fvar.found;
    return true;
}

}  // namespace nmarkdown
