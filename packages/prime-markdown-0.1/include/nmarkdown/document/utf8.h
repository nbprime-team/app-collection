#ifndef NMARKDOWN_DOCUMENT_UTF8_H
#define NMARKDOWN_DOCUMENT_UTF8_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace nmarkdown {

constexpr std::uint32_t kReplacementCodepoint = 0xFFFDU;

struct DecodedCodepoint {
    std::uint32_t value = kReplacementCodepoint;
    std::uint32_t byte_offset = 0;
    std::uint8_t byte_length = 0;
    bool valid = false;
};

struct Utf8ValidationResult {
    std::size_t codepoint_count = 0;
    std::size_t invalid_sequence_count = 0;
    bool had_bom = false;

    bool valid() const { return invalid_sequence_count == 0; }
};

bool unicode_scalar_valid(std::uint32_t codepoint);

DecodedCodepoint utf8_next(const std::uint8_t* bytes,
                           std::size_t size,
                           std::uint32_t offset);

Utf8ValidationResult utf8_validate(const std::uint8_t* bytes,
                                   std::size_t size,
                                   bool detect_bom = true);

Utf8ValidationResult utf8_sanitize(const std::uint8_t* bytes,
                                   std::size_t size,
                                   std::string& output,
                                   bool strip_bom = true);

void utf8_append(std::uint32_t codepoint, std::string& output);

}  // namespace nmarkdown

#endif

