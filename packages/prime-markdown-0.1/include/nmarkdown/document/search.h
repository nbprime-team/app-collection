#ifndef NMARKDOWN_DOCUMENT_SEARCH_H
#define NMARKDOWN_DOCUMENT_SEARCH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nmarkdown {

enum class SearchMode : std::uint8_t {
    ExactUtf8,
    AsciiCaseInsensitive,
    CanonicalUtf8,
    UnicodeCaseInsensitive,
};

struct SearchMatch {
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
    std::uint32_t snippet_match_offset = 0;
    std::string snippet;
};

std::vector<SearchMatch> search_source(std::string_view source,
                                       std::string_view query,
                                       SearchMode mode,
                                       std::size_t maximum_results = 128);

}  // namespace nmarkdown

#endif
