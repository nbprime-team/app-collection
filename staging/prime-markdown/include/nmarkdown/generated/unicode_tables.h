#ifndef NMARKDOWN_GENERATED_UNICODE_TABLES_H
#define NMARKDOWN_GENERATED_UNICODE_TABLES_H

#include <cstddef>
#include <cstdint>

namespace nmarkdown {

struct UnicodeMappingRecord {
    std::uint32_t codepoint;
    std::uint32_t offset;
    std::uint8_t length;
};

struct UnicodeClassRecord {
    std::uint32_t codepoint;
    std::uint8_t value;
};

struct UnicodeCompositionRecord {
    std::uint32_t first;
    std::uint32_t second;
    std::uint32_t composed;
};

extern const char kUnicodeDataVersion[];
extern const UnicodeMappingRecord kUnicodeCaseFoldRecords[];
extern const std::size_t kUnicodeCaseFoldRecordsCount;
extern const std::uint32_t kUnicodeCaseFoldValues[];
extern const std::size_t kUnicodeCaseFoldValuesCount;
extern const UnicodeMappingRecord kUnicodeDecompositionRecords[];
extern const std::size_t kUnicodeDecompositionRecordsCount;
extern const std::uint32_t kUnicodeDecompositionValues[];
extern const std::size_t kUnicodeDecompositionValuesCount;
extern const UnicodeClassRecord kUnicodeCombiningClasses[];
extern const std::size_t kUnicodeCombiningClassesCount;
extern const UnicodeCompositionRecord kUnicodeCompositions[];
extern const std::size_t kUnicodeCompositionsCount;

}  // namespace nmarkdown

#endif
