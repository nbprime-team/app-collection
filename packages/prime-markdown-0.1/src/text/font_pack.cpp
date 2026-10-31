#include "nmarkdown/text/font_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace nmarkdown {
namespace {

constexpr std::uint8_t kMagic[8] = {'N', 'M', 'F', 'O', 'N', 'T', '1', 0};
constexpr std::size_t kHeaderSize = 52;
constexpr std::size_t kFaceRecordSize = 40;
constexpr std::size_t kRangeRecordSize = 8;
constexpr std::size_t kChecksumOffset = 44;

std::uint16_t read_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1] << 8U);
}

std::uint32_t read_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool span_valid(std::uint32_t offset,
                std::uint32_t count,
                std::size_t item_size,
                std::size_t limit) {
    if (offset > limit || item_size == 0) {
        return false;
    }
    return count <= (limit - offset) / item_size;
}

std::uint32_t pack_checksum(const std::uint8_t* data, std::size_t size) {
    std::uint32_t hash = 2166136261U;
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t byte =
            index >= kChecksumOffset && index < kChecksumOffset + 4 ? 0 : data[index];
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

bool role_valid(std::uint32_t role) {
    return role == static_cast<std::uint32_t>(FontRole::BodySans) ||
           role == static_cast<std::uint32_t>(FontRole::BodySerif) ||
           role == static_cast<std::uint32_t>(FontRole::Monospace) ||
           role == static_cast<std::uint32_t>(FontRole::Math) ||
           role == static_cast<std::uint32_t>(FontRole::Cjk) ||
           role == static_cast<std::uint32_t>(FontRole::BodySansItalic) ||
           role == static_cast<std::uint32_t>(FontRole::BodySansBold) ||
           role == static_cast<std::uint32_t>(FontRole::BodySansBoldItalic) ||
           role == static_cast<std::uint32_t>(FontRole::Replacement);
}

}  // namespace

bool FontPackFace::declares_codepoint(std::uint32_t codepoint) const {
    const auto iterator = std::lower_bound(
        ranges.begin(), ranges.end(), codepoint, [](const CodepointRange& range, std::uint32_t value) {
            return range.last < value;
        });
    return iterator != ranges.end() && iterator->first <= codepoint;
}

void FontPack::reset() {
    faces_.clear();
    owned_data_.clear();
    data_ = nullptr;
    size_ = 0;
    checksum_ = 0;
}

bool FontPack::load_from_memory(const std::uint8_t* data,
                                std::size_t size,
                                std::string& error) {
    reset();
    return parse(data, size, error);
}

bool FontPack::load_from_file(const char* path, std::string& error) {
    reset();
    if (path == nullptr || path[0] == '\0') {
        error = "font-pack path is empty";
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = "could not open font pack";
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "could not seek font pack";
        return false;
    }
    const long end = std::ftell(file);
    if (end <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "font pack is empty or unreadable";
        return false;
    }

    owned_data_.resize(static_cast<std::size_t>(end));
    const std::size_t read = std::fread(owned_data_.data(), 1, owned_data_.size(), file);
    const bool close_ok = std::fclose(file) == 0;
    if (read != owned_data_.size() || !close_ok) {
        reset();
        error = "could not read complete font pack";
        return false;
    }

    if (!parse(owned_data_.data(), owned_data_.size(), error)) {
        owned_data_.clear();
        return false;
    }
    return true;
}

bool FontPack::parse(const std::uint8_t* data,
                     std::size_t size,
                     std::string& error) {
    error.clear();
    if (data == nullptr || size < kHeaderSize) {
        error = "font pack is smaller than its header";
        return false;
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        error = "font pack has an invalid magic value";
        return false;
    }

    const std::uint16_t version = read_u16(data + 8);
    const std::uint16_t header_size = read_u16(data + 10);
    const std::uint32_t file_size = read_u32(data + 12);
    const std::uint32_t face_count = read_u32(data + 16);
    const std::uint32_t face_table = read_u32(data + 20);
    const std::uint32_t range_table = read_u32(data + 24);
    const std::uint32_t range_count = read_u32(data + 28);
    const std::uint32_t string_table = read_u32(data + 32);
    const std::uint32_t string_size = read_u32(data + 36);
    const std::uint32_t payload_offset = read_u32(data + 40);
    const std::uint32_t expected_checksum = read_u32(data + kChecksumOffset);

    if (version != kFormatVersion || header_size != kHeaderSize) {
        error = "font-pack version is not supported";
        return false;
    }
    if (file_size != size || face_count == 0 || face_count > 64) {
        error = "font-pack size or face count is invalid";
        return false;
    }
    if (!span_valid(face_table, face_count, kFaceRecordSize, size) ||
        !span_valid(range_table, range_count, kRangeRecordSize, size) ||
        !span_valid(string_table, string_size, 1, size) || payload_offset > size) {
        error = "font-pack table lies outside the file";
        return false;
    }
    const std::uint32_t actual_checksum = pack_checksum(data, size);
    if (actual_checksum != expected_checksum) {
        error = "font-pack checksum mismatch";
        return false;
    }

    std::vector<FontPackFace> parsed_faces;
    parsed_faces.reserve(face_count);
    for (std::uint32_t index = 0; index < face_count; ++index) {
        const std::uint8_t* record = data + face_table + index * kFaceRecordSize;
        const std::uint32_t id = read_u32(record);
        const std::uint32_t role = read_u32(record + 4);
        const std::uint32_t font_offset = read_u32(record + 8);
        const std::uint32_t font_size = read_u32(record + 12);
        const std::uint32_t name_offset = read_u32(record + 16);
        const std::uint32_t name_size = read_u32(record + 20);
        const std::uint32_t license_offset = read_u32(record + 24);
        const std::uint32_t license_size = read_u32(record + 28);
        const std::uint32_t first_range = read_u32(record + 32);
        const std::uint32_t face_range_count = read_u32(record + 36);

        if (id == 0 || !role_valid(role) || font_size == 0 || font_offset < payload_offset ||
            !span_valid(font_offset, font_size, 1, size) ||
            !span_valid(name_offset, name_size, 1, string_size) ||
            !span_valid(license_offset, license_size, 1, string_size) ||
            first_range > range_count || face_range_count > range_count - first_range) {
            error = "font-pack face record is invalid";
            return false;
        }
        for (const FontPackFace& existing : parsed_faces) {
            if (existing.id == id) {
                error = "font pack contains duplicate face ids";
                return false;
            }
        }

        FontPackFace face;
        face.id = id;
        face.role = static_cast<FontRole>(role);
        face.font_data = data + font_offset;
        face.font_size = font_size;
        face.name = std::string_view(
            reinterpret_cast<const char*>(data + string_table + name_offset), name_size);
        face.license = std::string_view(
            reinterpret_cast<const char*>(data + string_table + license_offset), license_size);
        face.ranges.reserve(face_range_count);
        std::uint32_t previous_last = 0;
        for (std::uint32_t range_index = 0; range_index < face_range_count; ++range_index) {
            const std::uint8_t* range =
                data + range_table + (first_range + range_index) * kRangeRecordSize;
            const std::uint32_t first = read_u32(range);
            const std::uint32_t last = read_u32(range + 4);
            if (first > last || last > 0x10FFFFU ||
                (range_index != 0 && first <= previous_last)) {
                error = "font pack contains an invalid codepoint range";
                return false;
            }
            face.ranges.push_back({first, last});
            previous_last = last;
        }
        parsed_faces.push_back(std::move(face));
    }

    data_ = data;
    size_ = size;
    checksum_ = expected_checksum;
    faces_ = std::move(parsed_faces);
    return true;
}

const FontPackFace* FontPack::face(std::size_t index) const {
    return index < faces_.size() ? &faces_[index] : nullptr;
}

const FontPackFace* FontPack::face_by_id(std::uint32_t id) const {
    for (const FontPackFace& item : faces_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace nmarkdown
