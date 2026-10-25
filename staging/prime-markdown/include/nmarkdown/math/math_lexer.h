#ifndef NMARKDOWN_MATH_MATH_LEXER_H
#define NMARKDOWN_MATH_MATH_LEXER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nmarkdown {

enum class MathTokenKind : std::uint8_t {
    End,
    Character,
    ControlSequence,
    BeginGroup,
    EndGroup,
    Superscript,
    Subscript,
    AlignmentTab,
    RowBreak,
    OptionalBegin,
    OptionalEnd,
    Whitespace,
};

struct MathToken {
    MathTokenKind kind = MathTokenKind::End;
    std::uint32_t codepoint = 0;
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
};

struct MathLexResult {
    std::vector<MathToken> tokens;
    std::string error;
    std::uint32_t error_offset = 0;
    std::size_t invalid_utf8_count = 0;

    bool ok() const { return error.empty(); }
};

constexpr std::size_t kMaximumFormulaBytes = 16U * 1024U;
constexpr std::size_t kMaximumMathTokens = 8192;

bool lex_math(std::string_view source, MathLexResult& result);
std::string_view math_token_text(std::string_view source, const MathToken& token);

}  // namespace nmarkdown

#endif
