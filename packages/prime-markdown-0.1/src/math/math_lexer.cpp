#include "nmarkdown/math/math_lexer.h"

#include <algorithm>

#include "nmarkdown/document/utf8.h"

namespace nmarkdown {
namespace {

bool ascii_letter(std::uint8_t value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool ascii_space(std::uint8_t value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f';
}

MathTokenKind punctuation_kind(std::uint32_t codepoint) {
    switch (codepoint) {
    case '{': return MathTokenKind::BeginGroup;
    case '}': return MathTokenKind::EndGroup;
    case '^': return MathTokenKind::Superscript;
    case '_': return MathTokenKind::Subscript;
    case '&': return MathTokenKind::AlignmentTab;
    case '[': return MathTokenKind::OptionalBegin;
    case ']': return MathTokenKind::OptionalEnd;
    default: return MathTokenKind::Character;
    }
}

}  // namespace

std::string_view math_token_text(std::string_view source, const MathToken& token) {
    if (token.source_offset > source.size() ||
        token.source_length > source.size() - token.source_offset) {
        return {};
    }
    return source.substr(token.source_offset, token.source_length);
}

bool lex_math(std::string_view source, MathLexResult& result) {
    result = {};
    if (source.size() > kMaximumFormulaBytes) {
        result.error = "formula exceeds the 16 KiB source limit";
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(source.data());
    std::size_t offset = 0;
    while (offset < source.size()) {
        if (result.tokens.size() >= kMaximumMathTokens) {
            result.error = "formula exceeds the 8192-token limit";
            result.error_offset = static_cast<std::uint32_t>(offset);
            return false;
        }
        const std::size_t begin = offset;
        if (bytes[offset] == '\\') {
            ++offset;
            if (offset >= source.size()) {
                result.tokens.push_back({MathTokenKind::ControlSequence, '\\',
                                         static_cast<std::uint32_t>(begin), 1});
                continue;
            }
            if (bytes[offset] == '\\') {
                ++offset;
                result.tokens.push_back({MathTokenKind::RowBreak, 0,
                                         static_cast<std::uint32_t>(begin), 2});
                continue;
            }
            if (ascii_letter(bytes[offset])) {
                const std::size_t name_begin = offset;
                while (offset < source.size() && ascii_letter(bytes[offset])) ++offset;
                result.tokens.push_back(
                    {MathTokenKind::ControlSequence, 0,
                     static_cast<std::uint32_t>(name_begin),
                     static_cast<std::uint32_t>(offset - name_begin)});
                while (offset < source.size() && ascii_space(bytes[offset])) ++offset;
                continue;
            }
            const DecodedCodepoint decoded = utf8_next(
                bytes, source.size(), static_cast<std::uint32_t>(offset));
            const std::size_t length = decoded.byte_length == 0 ? 1 : decoded.byte_length;
            if (!decoded.valid) ++result.invalid_utf8_count;
            result.tokens.push_back(
                {MathTokenKind::ControlSequence, decoded.value,
                 static_cast<std::uint32_t>(offset),
                 static_cast<std::uint32_t>(length)});
            offset += length;
            continue;
        }
        if (ascii_space(bytes[offset])) {
            while (offset < source.size() && ascii_space(bytes[offset])) ++offset;
            result.tokens.push_back(
                {MathTokenKind::Whitespace, 0, static_cast<std::uint32_t>(begin),
                 static_cast<std::uint32_t>(offset - begin)});
            continue;
        }
        const DecodedCodepoint decoded = utf8_next(
            bytes, source.size(), static_cast<std::uint32_t>(offset));
        const std::size_t length = decoded.byte_length == 0 ? 1 : decoded.byte_length;
        if (!decoded.valid) ++result.invalid_utf8_count;
        result.tokens.push_back(
            {punctuation_kind(decoded.value), decoded.value,
             static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(length)});
        offset += length;
    }
    result.tokens.push_back(
        {MathTokenKind::End, 0, static_cast<std::uint32_t>(source.size()), 0});
    return true;
}

}  // namespace nmarkdown
