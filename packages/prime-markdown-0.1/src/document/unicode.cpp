#include "nmarkdown/document/unicode.h"

#include <algorithm>
#include <limits>

#include "nmarkdown/generated/unicode_tables.h"

namespace nmarkdown {
namespace {

constexpr std::uint32_t kSBase = 0xAC00;
constexpr std::uint32_t kLBase = 0x1100;
constexpr std::uint32_t kVBase = 0x1161;
constexpr std::uint32_t kTBase = 0x11A7;
constexpr std::uint32_t kLCount = 19;
constexpr std::uint32_t kVCount = 21;
constexpr std::uint32_t kTCount = 28;
constexpr std::uint32_t kNCount = kVCount * kTCount;
constexpr std::uint32_t kSCount = kLCount * kNCount;

const UnicodeMappingRecord* find_mapping(const UnicodeMappingRecord* records,
                                         std::size_t count,
                                         std::uint32_t codepoint) {
    const UnicodeMappingRecord* end = records + count;
    const UnicodeMappingRecord* found = std::lower_bound(
        records, end, codepoint,
        [](const UnicodeMappingRecord& record, std::uint32_t value) {
            return record.codepoint < value;
        });
    return found != end && found->codepoint == codepoint ? found : nullptr;
}

void decompose_recursive(std::uint32_t codepoint,
                         std::vector<std::uint32_t>& output,
                         unsigned depth) {
    if (depth > 8) {
        output.push_back(codepoint);
        return;
    }
    if (codepoint >= kSBase && codepoint < kSBase + kSCount) {
        const std::uint32_t index = codepoint - kSBase;
        output.push_back(kLBase + index / kNCount);
        output.push_back(kVBase + (index % kNCount) / kTCount);
        const std::uint32_t trailing = index % kTCount;
        if (trailing != 0) output.push_back(kTBase + trailing);
        return;
    }
    const UnicodeMappingRecord* mapping = find_mapping(
        kUnicodeDecompositionRecords, kUnicodeDecompositionRecordsCount, codepoint);
    if (mapping == nullptr ||
        mapping->offset > kUnicodeDecompositionValuesCount ||
        mapping->length > kUnicodeDecompositionValuesCount - mapping->offset) {
        output.push_back(codepoint);
        return;
    }
    for (std::size_t index = 0; index < mapping->length; ++index) {
        decompose_recursive(kUnicodeDecompositionValues[mapping->offset + index],
                            output, depth + 1);
    }
}

void canonical_order(std::vector<std::uint32_t>& codepoints) {
    std::size_t marks_begin = 0;
    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        if (unicode_canonical_combining_class(codepoints[index]) != 0) continue;
        std::stable_sort(codepoints.begin() + marks_begin, codepoints.begin() + index,
                         [](std::uint32_t left, std::uint32_t right) {
            return unicode_canonical_combining_class(left) <
                   unicode_canonical_combining_class(right);
        });
        marks_begin = index + 1;
    }
    std::stable_sort(codepoints.begin() + marks_begin, codepoints.end(),
                     [](std::uint32_t left, std::uint32_t right) {
        return unicode_canonical_combining_class(left) <
               unicode_canonical_combining_class(right);
    });
}

void compose_nfc(std::vector<std::uint32_t>& codepoints) {
    if (codepoints.empty()) return;
    std::vector<std::uint32_t> composed;
    composed.reserve(codepoints.size());
    composed.push_back(codepoints.front());
    std::size_t starter = unicode_canonical_combining_class(codepoints.front()) == 0
                              ? 0 : std::numeric_limits<std::size_t>::max();
    std::uint8_t previous_class = unicode_canonical_combining_class(codepoints.front());
    for (std::size_t index = 1; index < codepoints.size(); ++index) {
        const std::uint32_t current = codepoints[index];
        const std::uint8_t current_class = unicode_canonical_combining_class(current);
        std::uint32_t combined = 0;
        if (starter != std::numeric_limits<std::size_t>::max() &&
            (previous_class == 0 || previous_class < current_class) &&
            unicode_canonical_compose(composed[starter], current, combined)) {
            composed[starter] = combined;
            continue;
        }
        composed.push_back(current);
        if (current_class == 0) starter = composed.size() - 1;
        previous_class = current_class;
    }
    codepoints.swap(composed);
}

}  // namespace

const char* unicode_data_version() {
    return kUnicodeDataVersion;
}

std::uint8_t unicode_canonical_combining_class(std::uint32_t codepoint) {
    const UnicodeClassRecord* begin = kUnicodeCombiningClasses;
    const UnicodeClassRecord* end = begin + kUnicodeCombiningClassesCount;
    const UnicodeClassRecord* found = std::lower_bound(
        begin, end, codepoint,
        [](const UnicodeClassRecord& record, std::uint32_t value) {
            return record.codepoint < value;
        });
    return found != end && found->codepoint == codepoint ? found->value : 0;
}

void unicode_case_fold(std::uint32_t codepoint,
                       std::vector<std::uint32_t>& output) {
    const UnicodeMappingRecord* mapping = find_mapping(
        kUnicodeCaseFoldRecords, kUnicodeCaseFoldRecordsCount, codepoint);
    if (mapping == nullptr || mapping->offset > kUnicodeCaseFoldValuesCount ||
        mapping->length > kUnicodeCaseFoldValuesCount - mapping->offset) {
        output.push_back(codepoint);
        return;
    }
    output.insert(output.end(), kUnicodeCaseFoldValues + mapping->offset,
                  kUnicodeCaseFoldValues + mapping->offset + mapping->length);
}

void unicode_canonical_decompose(std::uint32_t codepoint,
                                 std::vector<std::uint32_t>& output) {
    decompose_recursive(codepoint, output, 0);
}

bool unicode_canonical_compose(std::uint32_t first,
                               std::uint32_t second,
                               std::uint32_t& composed) {
    if (first >= kLBase && first < kLBase + kLCount &&
        second >= kVBase && second < kVBase + kVCount) {
        composed = kSBase + ((first - kLBase) * kVCount +
                             (second - kVBase)) * kTCount;
        return true;
    }
    if (first >= kSBase && first < kSBase + kSCount &&
        (first - kSBase) % kTCount == 0 &&
        second > kTBase && second < kTBase + kTCount) {
        composed = first + second - kTBase;
        return true;
    }
    const UnicodeCompositionRecord key{first, second, 0};
    const UnicodeCompositionRecord* begin = kUnicodeCompositions;
    const UnicodeCompositionRecord* end = begin + kUnicodeCompositionsCount;
    const UnicodeCompositionRecord* found = std::lower_bound(
        begin, end, key,
        [](const UnicodeCompositionRecord& left,
           const UnicodeCompositionRecord& right) {
            return left.first < right.first ||
                   (left.first == right.first && left.second < right.second);
        });
    if (found == end || found->first != first || found->second != second) return false;
    composed = found->composed;
    return true;
}

Utf8ValidationResult unicode_normalize_utf8(std::string_view input,
                                            UnicodeNormalizationForm form,
                                            std::string& output) {
    Utf8ValidationResult validation = utf8_validate(
        reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), false);
    std::vector<std::uint32_t> codepoints;
    codepoints.reserve(validation.codepoint_count + 8);
    std::size_t offset = 0;
    while (offset < input.size()) {
        const DecodedCodepoint decoded = utf8_next(
            reinterpret_cast<const std::uint8_t*>(input.data()), input.size(),
            static_cast<std::uint32_t>(offset));
        unicode_canonical_decompose(decoded.value, codepoints);
        offset += decoded.byte_length == 0 ? 1 : decoded.byte_length;
    }
    canonical_order(codepoints);
    if (form == UnicodeNormalizationForm::Nfc) compose_nfc(codepoints);
    output.clear();
    for (std::uint32_t codepoint : codepoints) utf8_append(codepoint, output);
    return validation;
}

bool contains_cjk_text(std::string_view utf8) {
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        const DecodedCodepoint decoded = utf8_next(
            reinterpret_cast<const std::uint8_t*>(utf8.data()), utf8.size(),
            static_cast<std::uint32_t>(offset));
        offset += decoded.byte_length == 0 ? 1 : decoded.byte_length;
        if (!decoded.valid) continue;
        const std::uint32_t value = decoded.value;
        if ((value >= 0x3000U && value <= 0x30FFU) ||
            (value >= 0x3400U && value <= 0x4DBFU) ||
            (value >= 0x4E00U && value <= 0x9FFFU) ||
            (value >= 0xAC00U && value <= 0xD7AFU) ||
            (value >= 0xF900U && value <= 0xFAFFU) ||
            (value >= 0x20000U && value <= 0x3134FU)) {
            return true;
        }
    }
    return false;
}

}  // namespace nmarkdown
