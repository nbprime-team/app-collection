#include "nmarkdown/document/utf8.h"

#include <limits>

namespace nmarkdown {
namespace {

constexpr bool continuation(std::uint8_t byte) {
    return (byte & 0xC0U) == 0x80U;
}

bool has_bom(const std::uint8_t* bytes, std::size_t size) {
    return size >= 3 && bytes[0] == 0xEFU && bytes[1] == 0xBBU &&
           bytes[2] == 0xBFU;
}

DecodedCodepoint malformed(std::uint32_t offset, std::size_t length) {
    const std::size_t bounded = length == 0 ? 1 : length;
    return {kReplacementCodepoint,
            offset,
            static_cast<std::uint8_t>(bounded > 255 ? 255 : bounded),
            false};
}

}  // namespace

bool unicode_scalar_valid(std::uint32_t codepoint) {
    return codepoint <= 0x10FFFFU &&
           !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
}

DecodedCodepoint utf8_next(const std::uint8_t* bytes,
                           std::size_t size,
                           std::uint32_t offset) {
    if (bytes == nullptr || offset >= size) {
        return {kReplacementCodepoint, offset, 0, false};
    }

    const std::uint8_t first = bytes[offset];
    if (first <= 0x7FU) {
        return {first, offset, 1, true};
    }

    std::uint8_t expected = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        expected = 2;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        expected = 3;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        expected = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return malformed(offset, 1);
    }

    std::size_t consumed = 1;
    for (std::uint8_t index = 1; index < expected; ++index) {
        if (static_cast<std::size_t>(offset) + index >= size) {
            return malformed(offset, consumed);
        }
        const std::uint8_t next = bytes[offset + index];
        if (!continuation(next)) {
            return malformed(offset, consumed);
        }
        value = (value << 6U) | (next & 0x3FU);
        ++consumed;
    }

    if (value < minimum || !unicode_scalar_valid(value)) {
        return malformed(offset, expected);
    }

    return {value, offset, expected, true};
}

Utf8ValidationResult utf8_validate(const std::uint8_t* bytes,
                                   std::size_t size,
                                   bool detect_bom) {
    Utf8ValidationResult result;
    result.had_bom = detect_bom && bytes != nullptr && has_bom(bytes, size);

    std::size_t offset = 0;
    while (offset < size) {
        const DecodedCodepoint decoded =
            utf8_next(bytes, size, static_cast<std::uint32_t>(offset));
        const std::size_t advance = decoded.byte_length == 0 ? 1 : decoded.byte_length;
        offset += advance;
        ++result.codepoint_count;
        if (!decoded.valid) {
            ++result.invalid_sequence_count;
        }
    }
    return result;
}

void utf8_append(std::uint32_t codepoint, std::string& output) {
    if (!unicode_scalar_valid(codepoint)) {
        codepoint = kReplacementCodepoint;
    }

    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

Utf8ValidationResult utf8_sanitize(const std::uint8_t* bytes,
                                   std::size_t size,
                                   std::string& output,
                                   bool strip_bom) {
    output.clear();
    if (size <= output.max_size()) {
        output.reserve(size);
    }

    Utf8ValidationResult result;
    result.had_bom = bytes != nullptr && has_bom(bytes, size);
    std::size_t offset = strip_bom && result.had_bom ? 3 : 0;
    while (offset < size) {
        const DecodedCodepoint decoded =
            utf8_next(bytes, size, static_cast<std::uint32_t>(offset));
        const std::size_t advance = decoded.byte_length == 0 ? 1 : decoded.byte_length;
        offset += advance;
        ++result.codepoint_count;
        if (!decoded.valid) {
            ++result.invalid_sequence_count;
        }
        utf8_append(decoded.value, output);
    }
    return result;
}

}  // namespace nmarkdown

