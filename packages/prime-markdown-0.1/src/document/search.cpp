#include "nmarkdown/document/search.h"

#include <algorithm>
#include <limits>

#include "nmarkdown/document/unicode.h"
#include "nmarkdown/document/utf8.h"

namespace nmarkdown {
namespace {

unsigned char ascii_fold(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + 32)
                                        : value;
}

bool continuation(unsigned char value) {
    return (value & 0xC0U) == 0x80U;
}

bool snippet_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::string readable_markdown_snippet(std::string_view source) {
    std::string output;
    output.reserve(source.size());
    bool pending_space = false;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char value = source[index];
        if (snippet_space(value) || value == '|') {
            pending_space = !output.empty();
            continue;
        }
        if (value == '!' && index + 1 < source.size() && source[index + 1] == '[') {
            continue;
        }
        if (value == '[') {
            const std::size_t close = source.find(']', index + 1);
            if (close != std::string_view::npos && close + 1 < source.size() &&
                source[close + 1] == '(') {
                const std::size_t target_end = source.find(')', close + 2);
                if (target_end != std::string_view::npos) {
                    if (pending_space && !output.empty()) output.push_back(' ');
                    output.append(source.substr(index + 1, close - index - 1));
                    const std::string_view target = source.substr(
                        close + 2, target_end - close - 2);
                    if (!target.empty()) {
                        output += " - ";
                        output.append(target);
                    }
                    pending_space = false;
                    index = target_end;
                    continue;
                }
            }
        }
        if (value == '#' || value == '*' || value == '_' || value == '`' ||
            value == '~') {
            continue;
        }
        if (pending_space && !output.empty()) output.push_back(' ');
        pending_space = false;
        output.push_back(value);
    }
    while (!output.empty() && output.back() == ' ') output.pop_back();
    return output;
}

bool equal_at(std::string_view source,
              std::string_view query,
              std::size_t offset,
              SearchMode mode) {
    if (offset > source.size() || query.size() > source.size() - offset) return false;
    for (std::size_t index = 0; index < query.size(); ++index) {
        unsigned char left = static_cast<unsigned char>(source[offset + index]);
        unsigned char right = static_cast<unsigned char>(query[index]);
        if (mode == SearchMode::AsciiCaseInsensitive && left < 0x80U && right < 0x80U) {
            left = ascii_fold(left);
            right = ascii_fold(right);
        }
        if (left != right) return false;
    }
    return true;
}

SearchMatch make_match(std::string_view source,
                       std::size_t offset,
                       std::size_t length) {
    constexpr std::size_t kContextBefore = 18;
    constexpr std::size_t kContextAfter = 42;
    std::size_t begin = offset > kContextBefore ? offset - kContextBefore : 0;
    while (begin < offset && continuation(static_cast<unsigned char>(source[begin]))) ++begin;
    if (begin != 0) {
        while (begin < offset && !snippet_space(source[begin])) ++begin;
        while (begin < offset && snippet_space(source[begin])) ++begin;
    }
    std::size_t end = std::min(source.size(), offset + length + kContextAfter);
    while (end > offset + length && end < source.size() &&
           continuation(static_cast<unsigned char>(source[end]))) {
        --end;
    }
    if (end != source.size()) {
        while (end > offset + length && !snippet_space(source[end - 1])) --end;
    }
    SearchMatch match;
    match.source_offset = static_cast<std::uint32_t>(offset);
    match.source_length = static_cast<std::uint32_t>(length);
    const std::string matched_source(source.substr(offset, length));
    match.snippet = readable_markdown_snippet(source.substr(begin, end - begin));
    if (begin != 0) match.snippet.insert(0, u8"… ");
    if (end != source.size()) match.snippet += u8" …";
    const std::size_t rendered_match = match.snippet.find(matched_source);
    match.snippet_match_offset = static_cast<std::uint32_t>(
        rendered_match == std::string::npos ? 0 : rendered_match);
    return match;
}

struct TransformedUnit {
    std::uint32_t codepoint = 0;
    std::uint32_t source_begin = 0;
    std::uint32_t source_end = 0;
};

template <typename Emit>
bool stream_transformed(std::string_view input, bool case_fold, Emit emit) {
    std::vector<TransformedUnit> segment;
    std::vector<std::uint32_t> mapped;
    std::vector<std::uint32_t> decomposed;
    const auto flush = [&]() {
        std::stable_sort(segment.begin(), segment.end(),
                         [](const TransformedUnit& left,
                            const TransformedUnit& right) {
            return unicode_canonical_combining_class(left.codepoint) <
                   unicode_canonical_combining_class(right.codepoint);
        });
        for (const TransformedUnit& unit : segment) {
            if (!emit(unit)) return false;
        }
        segment.clear();
        return true;
    };

    std::size_t offset = 0;
    while (offset < input.size()) {
        const DecodedCodepoint decoded = utf8_next(
            reinterpret_cast<const std::uint8_t*>(input.data()), input.size(),
            static_cast<std::uint32_t>(offset));
        const std::size_t length = decoded.byte_length == 0 ? 1 : decoded.byte_length;
        mapped.clear();
        if (case_fold) unicode_case_fold(decoded.value, mapped);
        else mapped.push_back(decoded.value);
        for (std::uint32_t value : mapped) {
            decomposed.clear();
            unicode_canonical_decompose(value, decomposed);
            for (std::uint32_t normalized : decomposed) {
                if (unicode_canonical_combining_class(normalized) == 0 &&
                    !segment.empty() && !flush()) return false;
                segment.push_back({normalized, static_cast<std::uint32_t>(offset),
                                   static_cast<std::uint32_t>(offset + length)});
            }
        }
        offset += length;
    }
    return flush();
}

std::vector<std::uint32_t> transformed_query(std::string_view query,
                                             bool case_fold) {
    std::vector<std::uint32_t> result;
    stream_transformed(query, case_fold, [&](const TransformedUnit& unit) {
        result.push_back(unit.codepoint);
        return true;
    });
    return result;
}

std::vector<SearchMatch> normalized_search(std::string_view source,
                                           std::string_view query,
                                           bool case_fold,
                                           std::size_t maximum_results) {
    std::vector<SearchMatch> results;
    const std::vector<std::uint32_t> needle = transformed_query(query, case_fold);
    if (needle.empty() || source.size() > std::numeric_limits<std::uint32_t>::max()) {
        return results;
    }
    std::vector<std::size_t> prefix(needle.size(), 0);
    for (std::size_t index = 1, matched = 0; index < needle.size(); ++index) {
        while (matched != 0 && needle[index] != needle[matched]) {
            matched = prefix[matched - 1];
        }
        if (needle[index] == needle[matched]) ++matched;
        prefix[index] = matched;
    }
    std::vector<TransformedUnit> ring(needle.size());
    std::size_t seen = 0;
    std::size_t matched = 0;
    results.reserve(std::min<std::size_t>(maximum_results, 32));
    stream_transformed(source, case_fold, [&](const TransformedUnit& unit) {
        ring[seen % ring.size()] = unit;
        ++seen;
        while (matched != 0 && unit.codepoint != needle[matched]) {
            matched = prefix[matched - 1];
        }
        if (unit.codepoint == needle[matched]) ++matched;
        if (matched != needle.size()) return true;

        std::uint32_t begin = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t end = 0;
        for (std::size_t index = seen - needle.size(); index < seen; ++index) {
            const TransformedUnit& matched_unit = ring[index % ring.size()];
            begin = std::min(begin, matched_unit.source_begin);
            end = std::max(end, matched_unit.source_end);
        }
        if (results.empty() || results.back().source_offset != begin ||
            results.back().source_length != end - begin) {
            results.push_back(make_match(source, begin, end - begin));
        }
        matched = prefix[matched - 1];
        return results.size() < maximum_results;
    });
    return results;
}

}  // namespace

std::vector<SearchMatch> search_source(std::string_view source,
                                       std::string_view query,
                                       SearchMode mode,
                                       std::size_t maximum_results) {
    std::vector<SearchMatch> results;
    if (query.empty() || maximum_results == 0) return results;
    if (mode == SearchMode::CanonicalUtf8 ||
        mode == SearchMode::UnicodeCaseInsensitive) {
        return normalized_search(source, query,
                                 mode == SearchMode::UnicodeCaseInsensitive,
                                 maximum_results);
    }
    if (query.size() > source.size()) return results;
    results.reserve(std::min<std::size_t>(maximum_results, 32));
    std::size_t offset = 0;
    while (offset + query.size() <= source.size() && results.size() < maximum_results) {
        if (!continuation(static_cast<unsigned char>(source[offset])) &&
            equal_at(source, query, offset, mode)) {
            results.push_back(make_match(source, offset, query.size()));
            offset += query.size();
        } else {
            ++offset;
        }
    }
    return results;
}

}  // namespace nmarkdown
