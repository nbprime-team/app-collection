#ifndef NMARKDOWN_MATH_MATH_ATOMS_H
#define NMARKDOWN_MATH_MATH_ATOMS_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nmarkdown {

using MathNodeId = std::uint32_t;
constexpr MathNodeId kInvalidMathNode = 0xFFFFFFFFU;

enum class AtomClass : std::uint8_t {
    Ordinary,
    Operator,
    Binary,
    Relation,
    Opening,
    Closing,
    Punctuation,
    Inner,
};

enum class MathNodeKind : std::uint8_t {
    Row,
    Symbol,
    Text,
    // An equation number introduced by \tag. Array layout keeps this node in
    // a right-side lane instead of treating it as equation-cell contents.
    Tag,
    Space,
    Fraction,
    Radical,
    Scripts,
    Delimited,
    Accent,
    Styled,
    StyleChange,
    Array,
    Error,
};

enum MathNodeFlags : std::uint16_t {
    MathNodeFlagNone = 0,
    MathNodeFlagHasSubscript = 1U << 0U,
    MathNodeFlagHasSuperscript = 1U << 1U,
    MathNodeFlagLargeOperator = 1U << 2U,
    MathNodeFlagMovableLimits = 1U << 3U,
};

enum class MathAccent : std::uint8_t {
    Hat,
    Acute,
    Grave,
    Breve,
    Check,
    Tilde,
    Ring,
    Bar,
    Vector,
    Dot,
    DoubleDot,
    Overline,
    Underline,
    OverLeftArrow,
    OverRightArrow,
    OverLeftRightArrow,
    UnderLeftArrow,
    UnderRightArrow,
    UnderLeftRightArrow,
    OverBrace,
    UnderBrace,
};

enum class MathVariant : std::uint8_t {
    Normal,
    Roman,
    Bold,
    BoldItalic,
    Italic,
    Blackboard,
    Calligraphic,
    SansSerif,
    Monospace,
    Fraktur,
};

struct MathNode {
    MathNodeKind kind = MathNodeKind::Row;
    AtomClass atom_class = AtomClass::Ordinary;
    std::uint16_t flags = MathNodeFlagNone;
    std::uint32_t first_child = 0;
    std::uint16_t child_count = 0;
    std::uint16_t aux = 0;
    std::int32_t value = 0;
    // Kind-specific auxiliary data that does not fit in aux/value. Array nodes
    // use one bit per row boundary to retain \hline directives.
    std::uint32_t metadata = 0;
    std::uint32_t text_offset = 0;
    std::uint32_t text_length = 0;
};

struct MathTree {
    std::vector<MathNode> nodes;
    std::vector<MathNodeId> children;
    std::string strings;
    MathNodeId root = kInvalidMathNode;
    bool recovered_error = false;
    std::string diagnostic;
    std::uint32_t diagnostic_offset = 0;

    void clear();
    std::string_view text(const MathNode& node) const;
};

constexpr std::size_t kMaximumMathNesting = 64;
constexpr std::size_t kMaximumMathBoxes = 16384;
constexpr std::size_t kMaximumMatrixDimension = 32;

}  // namespace nmarkdown

#endif
