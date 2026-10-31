#ifndef NMARKDOWN_DOCUMENT_UNICODE_H
#define NMARKDOWN_DOCUMENT_UNICODE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nmarkdown/document/utf8.h"

namespace nmarkdown {

enum class UnicodeNormalizationForm : std::uint8_t {
    Nfd,
    Nfc,
};

const char* unicode_data_version();
std::uint8_t unicode_canonical_combining_class(std::uint32_t codepoint);
void unicode_case_fold(std::uint32_t codepoint,
                       std::vector<std::uint32_t>& output);
void unicode_canonical_decompose(std::uint32_t codepoint,
                                 std::vector<std::uint32_t>& output);
bool unicode_canonical_compose(std::uint32_t first,
                               std::uint32_t second,
                               std::uint32_t& composed);
Utf8ValidationResult unicode_normalize_utf8(std::string_view input,
                                            UnicodeNormalizationForm form,
                                            std::string& output);
// True when the UTF-8 text contains at least one CJK codepoint (Han, kana,
// Hangul, compatibility ideographs, or the supplementary ideographic
// planes). Invalid sequences are skipped rather than treated as matches.
bool contains_cjk_text(std::string_view utf8);

}  // namespace nmarkdown

#endif
