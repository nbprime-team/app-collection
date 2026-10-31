#include "nmarkdown/math/math_parser.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "nmarkdown/document/utf8.h"
#include "nmarkdown/math/math_lexer.h"
#include "nmarkdown/math/math_macros.h"

namespace nmarkdown {
namespace {

struct SymbolDefinition {
    const char* name;
    const char* text;
    AtomClass atom_class;
    bool large_glyph;
    bool movable_limits;
};

constexpr SymbolDefinition kSymbols[] = {
#include "math_symbol_table.inc"
};

enum StopFlags : unsigned {
    StopNone = 0,
    StopGroup = 1U << 0U,
    StopOptional = 1U << 1U,
    StopCell = 1U << 2U,
    StopRight = 1U << 3U,
    StopEnvironment = 1U << 4U,
};

std::uint32_t typographic_math_codepoint(std::uint32_t codepoint) {
    // U+002D is punctuation and is intentionally narrower in proportional
    // text fonts. Mathematical subtraction uses U+2212, whose advance and
    // stroke are designed to match the plus sign.
    return codepoint == '-' ? 0x2212U : codepoint;
}

AtomClass character_class(std::uint32_t codepoint) {
    switch (codepoint) {
    case '+': case '-': case '*': case '/': case 0x00B1: case 0x00D7:
    case 0x2212:
        return AtomClass::Binary;
    case '=': case '<': case '>': return AtomClass::Relation;
    case '(': case '[': case '{': return AtomClass::Opening;
    case ')': case ']': case '}': return AtomClass::Closing;
    case ',': case ';': case ':': return AtomClass::Punctuation;
    default: return AtomClass::Ordinary;
    }
}

bool body_text_codepoint(std::uint32_t codepoint) {
    return (codepoint >= 0x2E80U && codepoint <= 0x9FFFU) ||
           (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
           (codepoint >= 0x20000U && codepoint <= 0x323AFU);
}

bool ascii_control_letter(char value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

bool ascii_control_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f';
}

// \text arguments intentionally remain prose instead of being reparsed as
// mathematical atoms. Decode the small set of supported LaTeX text symbols
// here so their control words do not leak into the rendered annotation.
std::string normalize_text_symbols(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (text[offset] != '\\') {
            normalized.push_back(text[offset++]);
            continue;
        }

        const std::size_t command_begin = offset++;
        const std::size_t name_begin = offset;
        while (offset < text.size() && ascii_control_letter(text[offset])) {
            ++offset;
        }
        if (offset == name_begin) {
            // Preserve an unsupported control symbol byte-for-byte. Consuming
            // its second byte also prevents `\\textvisiblespace` from being
            // mistaken for the requested control word on the next iteration.
            normalized.push_back('\\');
            if (offset < text.size()) normalized.push_back(text[offset++]);
            continue;
        }

        const std::string_view name =
            text.substr(name_begin, offset - name_begin);
        if (name != "textvisiblespace") {
            normalized.append(text.data() + command_begin,
                              offset - command_begin);
            continue;
        }

        normalized += u8"␣";
        // A space after a TeX control word delimits the name and produces no
        // output. An immediately following empty group is another conventional
        // delimiter and likewise has no visible braces.
        while (offset < text.size() && ascii_control_space(text[offset])) {
            ++offset;
        }
        if (offset + 1 < text.size() && text[offset] == '{' &&
            text[offset + 1] == '}') {
            offset += 2;
        }
    }
    return normalized;
}

const SymbolDefinition* find_symbol(std::string_view name) {
    const auto found = std::lower_bound(
        std::begin(kSymbols), std::end(kSymbols), name,
        [](const SymbolDefinition& symbol, std::string_view value) {
            return std::string_view(symbol.name) < value;
        });
    return found != std::end(kSymbols) && name == found->name ? &*found
                                                               : nullptr;
}

class Parser {
public:
    Parser(std::string_view source,
           const std::vector<MathToken>& tokens,
           MathTree& tree)
        : source_(source), tokens_(tokens), tree_(tree) {}

    bool run() {
        std::vector<std::vector<MathNodeId>> rows;
        std::vector<MathNodeId> row;
        bool array = false;
        while (!hard_failure_) {
            row.push_back(parse_row(StopCell));
            if (current().kind == MathTokenKind::AlignmentTab) {
                array = true;
                ++position_;
                continue;
            }
            rows.push_back(std::move(row));
            row.clear();
            if (current().kind == MathTokenKind::RowBreak) {
                array = true;
                ++position_;
                continue;
            }
            break;
        }
        if (!array && !rows.empty() && !rows.front().empty()) {
            tree_.root = rows.front().front();
        } else {
            std::size_t columns = 0;
            for (const auto& current_row : rows) {
                columns = std::max(columns, current_row.size());
            }
            if (rows.size() > kMaximumMatrixDimension ||
                columns > kMaximumMatrixDimension) {
                hard_failure_ = true;
                recover("aligned formula exceeds the 32 by 32 limit", 0);
            } else {
                const MathNodeId empty = add(MathNode{});
                std::vector<MathNodeId> cells;
                for (auto& current_row : rows) {
                    while (current_row.size() < columns) current_row.push_back(empty);
                    cells.insert(cells.end(), current_row.begin(), current_row.end());
                }
                MathNode aligned;
                aligned.kind = MathNodeKind::Array;
                aligned.atom_class = AtomClass::Inner;
                aligned.aux = static_cast<std::uint16_t>(rows.size());
                aligned.value = static_cast<std::int32_t>(columns);
                const auto stored = store("aligned");
                aligned.text_offset = stored.first;
                aligned.text_length = stored.second;
                tree_.root = add(aligned, cells);
            }
        }
        if (hard_failure_) return false;
        if (current().kind != MathTokenKind::End) {
            recover("unexpected closing token", current().source_offset);
        }
        return true;
    }

private:
    const MathToken& current() const { return tokens_[std::min(position_, tokens_.size() - 1)]; }

    void skip_whitespace() {
        while (current().kind == MathTokenKind::Whitespace) ++position_;
    }

    bool control(std::string_view name) const {
        return current().kind == MathTokenKind::ControlSequence &&
               current().codepoint == 0 && math_token_text(source_, current()) == name;
    }

    void recover(const char* message, std::uint32_t offset) {
        if (!tree_.recovered_error) {
            tree_.recovered_error = true;
            tree_.diagnostic = message;
            tree_.diagnostic_offset = offset;
        }
    }

    std::pair<std::uint32_t, std::uint32_t> store(std::string_view text) {
        const std::uint32_t offset = static_cast<std::uint32_t>(tree_.strings.size());
        tree_.strings.append(text.data(), text.size());
        return {offset, static_cast<std::uint32_t>(text.size())};
    }

    MathNodeId add(MathNode node, const std::vector<MathNodeId>& children = {}) {
        if (tree_.nodes.size() >= kMaximumMathBoxes) {
            hard_failure_ = true;
            recover("formula exceeds the generated-box limit", current().source_offset);
            return kInvalidMathNode;
        }
        node.first_child = static_cast<std::uint32_t>(tree_.children.size());
        node.child_count = static_cast<std::uint16_t>(
            std::min<std::size_t>(children.size(), 0xFFFFU));
        tree_.children.insert(tree_.children.end(), children.begin(), children.end());
        tree_.nodes.push_back(node);
        return static_cast<MathNodeId>(tree_.nodes.size() - 1);
    }

    MathNodeId text_node(MathNodeKind kind,
                         std::string_view text,
                         AtomClass atom_class = AtomClass::Ordinary,
                         std::uint16_t flags = 0) {
        MathNode node;
        node.kind = kind;
        node.atom_class = atom_class;
        node.flags = flags;
        const auto stored = store(text);
        node.text_offset = stored.first;
        node.text_length = stored.second;
        return add(node);
    }

    MathNodeId semantic_atom(MathNodeId id) const {
        for (unsigned depth = 0; depth < kMaximumMathNesting; ++depth) {
            if (id >= tree_.nodes.size()) return kInvalidMathNode;
            const MathNode& node = tree_.nodes[id];
            if ((node.kind != MathNodeKind::Row &&
                 node.kind != MathNodeKind::Styled) ||
                node.child_count != 1) {
                return id;
            }
            if (node.first_child >= tree_.children.size()) {
                return kInvalidMathNode;
            }
            id = tree_.children[node.first_child];
        }
        return kInvalidMathNode;
    }

    MathNodeId styled_node(MathVariant variant, MathNodeId argument) {
        MathNode node;
        node.kind = MathNodeKind::Styled;
        node.aux = static_cast<std::uint16_t>(variant);
        const MathNodeId semantic = semantic_atom(argument);
        if (semantic < tree_.nodes.size()) {
            node.atom_class = tree_.nodes[semantic].atom_class;
            node.flags = tree_.nodes[semantic].flags &
                         static_cast<std::uint16_t>(
                             MathNodeFlagLargeOperator |
                             MathNodeFlagMovableLimits);
        }
        return add(node, {argument});
    }

    bool stops(unsigned flags) const {
        const MathTokenKind kind = current().kind;
        if (kind == MathTokenKind::End) return true;
        if ((flags & StopGroup) != 0 && kind == MathTokenKind::EndGroup) return true;
        if ((flags & StopOptional) != 0 && kind == MathTokenKind::OptionalEnd) return true;
        if ((flags & StopCell) != 0 &&
            (kind == MathTokenKind::AlignmentTab || kind == MathTokenKind::RowBreak)) return true;
        if ((flags & StopRight) != 0 && control("right")) return true;
        if ((flags & StopEnvironment) != 0 && control("end")) return true;
        return false;
    }

    MathNodeId parse_row(unsigned flags) {
        std::vector<MathNodeId> atoms;
        while (!hard_failure_ && !stops(flags)) {
            if (current().kind == MathTokenKind::Whitespace) {
                ++position_;
                continue;
            }
            const std::size_t before = position_;
            const MathNodeId atom = parse_atom();
            if (atom != kInvalidMathNode) atoms.push_back(atom);
            if (position_ == before) {
                recover("math parser could not make progress", current().source_offset);
                ++position_;
            }
        }
        MathNode row;
        row.kind = MathNodeKind::Row;
        row.atom_class = AtomClass::Inner;
        return add(row, atoms);
    }

    MathNodeId parse_group() {
        skip_whitespace();
        if (current().kind != MathTokenKind::BeginGroup) {
            recover("expected a braced group", current().source_offset);
            return parse_primary();
        }
        const std::uint32_t begin = current().source_offset;
        ++position_;
        if (++nesting_ > kMaximumMathNesting) {
            hard_failure_ = true;
            recover("formula exceeds the 64-level nesting limit", begin);
            return kInvalidMathNode;
        }
        const MathNodeId result = parse_row(StopGroup);
        // A braced subformula is an ordinary noad at its surrounding level.
        // Its internal row still performs normal atom spacing, but braces do
        // not turn `a{b}` into an Ord-Inner-Ord sequence.
        if (result < tree_.nodes.size()) {
            tree_.nodes[result].atom_class = AtomClass::Ordinary;
        }
        if (current().kind == MathTokenKind::EndGroup) ++position_;
        else recover("unclosed braced group", begin);
        --nesting_;
        return result;
    }

    std::string parse_raw_group() {
        skip_whitespace();
        if (current().kind != MathTokenKind::BeginGroup) {
            recover("expected a braced text argument", current().source_offset);
            return {};
        }
        const std::uint32_t content_begin = current().source_offset + current().source_length;
        ++position_;
        unsigned depth = 1;
        std::uint32_t content_end = content_begin;
        while (current().kind != MathTokenKind::End && depth != 0) {
            if (current().kind == MathTokenKind::BeginGroup) ++depth;
            if (current().kind == MathTokenKind::EndGroup) {
                --depth;
                if (depth == 0) {
                    content_end = current().source_offset;
                    ++position_;
                    break;
                }
            }
            if (depth != 0) ++position_;
        }
        if (depth != 0) {
            recover("unclosed braced text argument", content_begin);
            content_end = static_cast<std::uint32_t>(source_.size());
        }
        if (content_end < content_begin || content_begin > source_.size()) return {};
        return std::string(source_.substr(content_begin, content_end - content_begin));
    }

    MathNodeId parse_script_argument() {
        if (current().kind == MathTokenKind::BeginGroup) return parse_group();
        return parse_primary();
    }

    MathNodeId parse_atom() {
        MathNodeId base = parse_primary();
        MathNodeId subscript = kInvalidMathNode;
        MathNodeId superscript = kInvalidMathNode;
        skip_whitespace();
        while (current().kind == MathTokenKind::Subscript ||
               current().kind == MathTokenKind::Superscript) {
            const bool sub = current().kind == MathTokenKind::Subscript;
            const std::uint32_t marker = current().source_offset;
            ++position_;
            skip_whitespace();
            MathNodeId argument = parse_script_argument();
            if (sub) {
                if (subscript != kInvalidMathNode) recover("duplicate subscript", marker);
                subscript = argument;
            } else {
                if (superscript != kInvalidMathNode) recover("duplicate superscript", marker);
                superscript = argument;
            }
        }
        if (subscript == kInvalidMathNode && superscript == kInvalidMathNode) return base;
        MathNode scripts;
        scripts.kind = MathNodeKind::Scripts;
        scripts.atom_class = base < tree_.nodes.size()
                                 ? tree_.nodes[base].atom_class
                                 : AtomClass::Ordinary;
        std::vector<MathNodeId> children{base};
        if (subscript != kInvalidMathNode) {
            scripts.flags |= MathNodeFlagHasSubscript;
            children.push_back(subscript);
        }
        if (superscript != kInvalidMathNode) {
            scripts.flags |= MathNodeFlagHasSuperscript;
            children.push_back(superscript);
        }
        return add(scripts, children);
    }

    std::string delimiter() {
        if (current().kind == MathTokenKind::Character ||
            current().kind == MathTokenKind::OptionalBegin ||
            current().kind == MathTokenKind::OptionalEnd) {
            const std::uint32_t offset = current().source_offset;
            const std::uint32_t codepoint = current().codepoint;
            const std::string value(math_token_text(source_, current()));
            ++position_;
            switch (codepoint) {
            case '.': return {};
            case '(':
            case ')':
            case '[':
            case ']':
            case '<':
            case '>':
            case '/':
            case '|': return value;
            default:
                recover("expected a delimiter after left or right", offset);
                return {};
            }
        }
        if (current().kind == MathTokenKind::ControlSequence) {
            const std::uint32_t offset = current().source_offset;
            const std::string_view name = math_token_text(source_, current());
            const std::uint32_t symbol = current().codepoint;
            ++position_;
            if (symbol != 0) {
                // TeX's control symbol `\\|` denotes a double vertical
                // delimiter. A raw `|` remains a single vertical delimiter.
                if (symbol == '|') return u8"‖";
                if (symbol == '{' || symbol == '}') {
                    std::string value;
                    utf8_append(symbol, value);
                    return value;
                }
                recover("expected a delimiter after left or right", offset);
                return {};
            }
            if (name == "langle") return u8"⟨";
            if (name == "rangle") return u8"⟩";
            if (name == "lbrace") return "{";
            if (name == "rbrace") return "}";
            if (name == "lfloor") return u8"⌊";
            if (name == "rfloor") return u8"⌋";
            if (name == "lceil") return u8"⌈";
            if (name == "rceil") return u8"⌉";
            if (name == "backslash") return "\\";
            if (name == "vert" || name == "lvert" || name == "rvert") {
                return "|";
            }
            if (name == "Vert" || name == "lVert" || name == "rVert") {
                return u8"‖";
            }
            if (name == "uparrow") return u8"↑";
            if (name == "downarrow") return u8"↓";
            if (name == "updownarrow") return u8"↕";
            if (name == "Uparrow") return u8"⇑";
            if (name == "Downarrow") return u8"⇓";
            if (name == "Updownarrow") return u8"⇕";
            if (const SymbolDefinition* symbol = find_symbol(name)) {
                if (symbol->atom_class == AtomClass::Opening ||
                    symbol->atom_class == AtomClass::Closing) {
                    return symbol->text;
                }
            }
            recover("expected a delimiter after left or right", offset);
            return {};
        }
        recover("expected a delimiter after left or right", current().source_offset);
        return {};
    }

    MathNodeId parse_delimited() {
        const std::uint32_t begin = current().source_offset;
        ++position_;  // left
        const std::string opening = delimiter();
        const MathNodeId inner = parse_row(StopRight);
        std::string closing;
        if (control("right")) {
            ++position_;
            closing = delimiter();
        } else {
            recover("left delimiter has no matching right", begin);
        }
        MathNode node;
        node.kind = MathNodeKind::Delimited;
        node.atom_class = AtomClass::Inner;
        const auto stored = store(opening + std::string("\0", 1) + closing);
        node.text_offset = stored.first;
        node.text_length = stored.second;
        node.aux = static_cast<std::uint16_t>(opening.size());
        return add(node, {inner});
    }

    MathNodeId parse_environment() {
        const std::uint32_t begin = current().source_offset;
        ++position_;  // begin
        const std::string environment = parse_raw_group();
        const bool supported = environment == "matrix" || environment == "pmatrix" ||
                               environment == "bmatrix" || environment == "cases" ||
                               environment == "Bmatrix" || environment == "vmatrix" ||
                               environment == "Vmatrix" || environment == "aligned" ||
                               environment == "align" || environment == "align*" ||
                               environment == "array";
        if (!supported) recover("unknown math environment", begin);

        std::string preamble;
        if (environment == "array") {
            skip_whitespace();
            preamble = parse_raw_group();
        }

        std::vector<std::vector<MathNodeId>> rows;
        std::vector<MathNodeId> row;
        std::uint32_t horizontal_lines = 0;
        while (current().kind != MathTokenKind::End && !hard_failure_) {
            skip_whitespace();
            if (control("end")) break;
            if (control("hline")) {
                ++position_;
                if (rows.size() < 32) {
                    horizontal_lines |= 1U << static_cast<unsigned>(rows.size());
                }
                continue;
            }
            row.push_back(parse_row(StopCell | StopEnvironment));
            if (current().kind == MathTokenKind::AlignmentTab) {
                ++position_;
                continue;
            }
            rows.push_back(std::move(row));
            row.clear();
            if (current().kind == MathTokenKind::RowBreak) ++position_;
            else if (!control("end") && current().kind != MathTokenKind::End) {
                recover("invalid matrix row separator", current().source_offset);
                ++position_;
            }
        }
        if (!row.empty()) rows.push_back(std::move(row));

        if (control("end")) {
            ++position_;
            const std::string closing = parse_raw_group();
            if (closing != environment) recover("mismatched math environment end", begin);
        } else {
            recover("unclosed math environment", begin);
        }

        if (rows.empty()) rows.push_back({add(MathNode{})});
        std::size_t columns = 0;
        for (const auto& item : rows) columns = std::max(columns, item.size());
        if (rows.size() > kMaximumMatrixDimension ||
            columns > kMaximumMatrixDimension) {
            hard_failure_ = true;
            recover("matrix exceeds the 32 by 32 limit", begin);
            return kInvalidMathNode;
        }
        const MathNodeId empty = add(MathNode{});
        std::vector<MathNodeId> cells;
        for (auto& item : rows) {
            while (item.size() < columns) item.push_back(empty);
            cells.insert(cells.end(), item.begin(), item.end());
        }
        MathNode array;
        array.kind = MathNodeKind::Array;
        array.atom_class = AtomClass::Inner;
        array.aux = static_cast<std::uint16_t>(rows.size());
        array.value = static_cast<std::int32_t>(columns);
        array.metadata = horizontal_lines;
        const std::string descriptor = preamble.empty()
                                           ? environment
                                           : environment + std::string("\0", 1) + preamble;
        const auto stored = store(descriptor);
        array.text_offset = stored.first;
        array.text_length = stored.second;
        return add(array, cells);
    }

    MathNodeId parse_control() {
        const MathToken token = current();
        const std::string_view name = math_token_text(source_, token);
        if (token.codepoint != 0) {
            ++position_;
            if (token.codepoint == ' ' || token.codepoint == ',' ||
                token.codepoint == ':' || token.codepoint == ';' ||
                token.codepoint == '!' ) {
                MathNode space;
                space.kind = MathNodeKind::Space;
                space.value = token.codepoint == ' ' ? 6 :
                              token.codepoint == ',' ? 3 :
                              token.codepoint == ':' ? 4 :
                              token.codepoint == ';' ? 5 : -3;
                return add(space);
            }
            const std::uint32_t codepoint = token.codepoint == '|'
                                                ? 0x2016U
                                                : typographic_math_codepoint(
                                                      token.codepoint);
            std::string value;
            utf8_append(codepoint, value);
            return text_node(MathNodeKind::Symbol, value,
                             character_class(codepoint));
        }
        if (name == "left") return parse_delimited();
        if (name == "begin") return parse_environment();
        ++position_;
        if (name == "not") {
            // HarfBuzz composes U+0338 with Latin Modern's relation glyphs,
            // selecting a precomposed negated glyph when one exists and a
            // correctly positioned zero-advance overlay otherwise.
            skip_whitespace();
            MathNodeId base = current().kind == MathTokenKind::BeginGroup
                                  ? parse_group()
                                  : parse_primary();
            MathNodeId symbol_id = base;
            if (base < tree_.nodes.size() &&
                tree_.nodes[base].kind == MathNodeKind::Row &&
                tree_.nodes[base].child_count == 1) {
                const std::size_t edge = tree_.nodes[base].first_child;
                if (edge < tree_.children.size()) symbol_id = tree_.children[edge];
            }
            if (symbol_id < tree_.nodes.size() &&
                tree_.nodes[symbol_id].kind == MathNodeKind::Symbol) {
                const MathNode& symbol_node = tree_.nodes[symbol_id];
                const std::string_view symbol_text = tree_.text(symbol_node);
                const DecodedCodepoint decoded = utf8_next(
                    reinterpret_cast<const std::uint8_t*>(symbol_text.data()),
                    symbol_text.size(), 0);
                if (symbol_node.atom_class == AtomClass::Relation &&
                    decoded.valid && decoded.byte_length == symbol_text.size()) {
                    std::string composed(symbol_text);
                    composed += u8"̸";
                    return text_node(MathNodeKind::Symbol, composed,
                                     symbol_node.atom_class, symbol_node.flags);
                }
            }
            recover("not requires one relation symbol", token.source_offset);
            return base;
        }
        if (name == "frac" || name == "cfrac") {
            const MathNodeId numerator = parse_group();
            const MathNodeId denominator = parse_group();
            MathNode node;
            node.kind = MathNodeKind::Fraction;
            node.atom_class = AtomClass::Inner;
            return add(node, {numerator, denominator});
        }
        if (name == "sqrt") {
            MathNodeId index = kInvalidMathNode;
            if (current().kind == MathTokenKind::OptionalBegin) {
                ++position_;
                index = parse_row(StopOptional);
                if (current().kind == MathTokenKind::OptionalEnd) ++position_;
                else recover("unclosed optional root index", token.source_offset);
            }
            const MathNodeId radicand = parse_group();
            MathNode node;
            node.kind = MathNodeKind::Radical;
            node.atom_class = AtomClass::Inner;
            return index == kInvalidMathNode ? add(node, {radicand})
                                             : add(node, {radicand, index});
        }
        const std::array<std::pair<std::string_view, MathAccent>, 21> accents{{
            {"hat", MathAccent::Hat},
            {"acute", MathAccent::Acute}, {"grave", MathAccent::Grave},
            {"breve", MathAccent::Breve}, {"check", MathAccent::Check},
            {"tilde", MathAccent::Tilde},
            {"mathring", MathAccent::Ring}, {"bar", MathAccent::Bar},
            {"vec", MathAccent::Vector}, {"dot", MathAccent::Dot},
            {"ddot", MathAccent::DoubleDot}, {"overline", MathAccent::Overline},
            {"underline", MathAccent::Underline},
            {"overleftarrow", MathAccent::OverLeftArrow},
            {"overrightarrow", MathAccent::OverRightArrow},
            {"overleftrightarrow", MathAccent::OverLeftRightArrow},
            {"underleftarrow", MathAccent::UnderLeftArrow},
            {"underrightarrow", MathAccent::UnderRightArrow},
            {"underleftrightarrow", MathAccent::UnderLeftRightArrow},
            {"overbrace", MathAccent::OverBrace},
            {"underbrace", MathAccent::UnderBrace},
        }};
        for (const auto& accent : accents) {
            if (name == accent.first) {
                MathNode node;
                node.kind = MathNodeKind::Accent;
                node.atom_class = AtomClass::Ordinary;
                node.aux = static_cast<std::uint16_t>(accent.second);
                // TeX macros consume one token when braces are omitted, so
                // both `\\dot{x}` and the conventional shorthand `\\dot x`
                // must produce the same accent node.
                skip_whitespace();
                const MathNodeId argument =
                    current().kind == MathTokenKind::BeginGroup
                        ? parse_group()
                        : parse_primary();
                return add(node, {argument});
            }
        }
        const std::array<std::pair<std::string_view, MathVariant>, 12> variants{{
            {"mathnormal", MathVariant::Normal},
            {"mathrm", MathVariant::Roman}, {"mathbf", MathVariant::Bold},
            {"bm", MathVariant::BoldItalic}, {"bold", MathVariant::Bold},
            {"mathit", MathVariant::Italic}, {"mathbb", MathVariant::Blackboard},
            {"mathcal", MathVariant::Calligraphic},
            {"mathscr", MathVariant::Calligraphic},
            {"mathsf", MathVariant::SansSerif},
            {"mathtt", MathVariant::Monospace},
            {"mathfrak", MathVariant::Fraktur},
        }};
        for (const auto& variant : variants) {
            if (name == variant.first) {
                return styled_node(variant.second, parse_group());
            }
        }
        const std::array<std::pair<std::string_view, MathVariant>, 11>
            declarations{{
                {"rm", MathVariant::Roman}, {"cal", MathVariant::Calligraphic},
                {"it", MathVariant::Italic}, {"Bbb", MathVariant::Blackboard},
                {"bf", MathVariant::Bold}, {"mit", MathVariant::Italic},
                {"sf", MathVariant::SansSerif}, {"scr", MathVariant::Calligraphic},
                {"tt", MathVariant::Monospace}, {"frak", MathVariant::Fraktur},
                {"boldsymbol", MathVariant::BoldItalic},
            }};
        for (const auto& declaration : declarations) {
            if (name == declaration.first) {
                const MathNodeId argument = current().kind == MathTokenKind::BeginGroup
                                                ? parse_group()
                                                : parse_primary();
                return styled_node(declaration.second, argument);
            }
        }
        if (name == "text" || name == "operatorname") {
            std::string value = parse_raw_group();
            if (name == "text") {
                value = normalize_text_symbols(value);
                value.erase(std::remove(value.begin(), value.end(), '$'), value.end());
            }
            return text_node(MathNodeKind::Text, value,
                             name == "operatorname" ? AtomClass::Operator
                                                    : AtomClass::Ordinary);
        }
        if (name == "tag") {
            std::string value;
            // TeX permits whitespace before an unbraced single-token tag,
            // as in `\tag 1`; do not accidentally turn that space into the
            // label while leaving the digit in the equation cell.
            skip_whitespace();
            if (current().kind == MathTokenKind::BeginGroup) {
                value = parse_raw_group();
            } else if (current().kind != MathTokenKind::End) {
                value.assign(math_token_text(source_, current()));
                ++position_;
            }
            value.erase(std::remove(value.begin(), value.end(), '$'), value.end());
            return text_node(MathNodeKind::Tag, "(" + value + ")",
                             AtomClass::Ordinary);
        }
        if (name == "displaystyle" || name == "textstyle" ||
            name == "scriptstyle" || name == "scriptscriptstyle") {
            MathNode style;
            style.kind = MathNodeKind::StyleChange;
            style.aux = name == "displaystyle" ? 0U
                        : name == "textstyle" ? 1U
                        : name == "scriptstyle" ? 2U : 3U;
            return add(style);
        }
        if (name == "hline") {
            return add(MathNode{});
        }
        if (name == "space" || name == "quad" || name == "qquad") {
            MathNode space;
            space.kind = MathNodeKind::Space;
            space.value = name == "space" ? 6 : name == "quad" ? 18 : 36;
            return add(space);
        }
        if (const SymbolDefinition* symbol = find_symbol(name)) {
            const std::uint16_t flags =
                (symbol->large_glyph
                     ? static_cast<std::uint16_t>(MathNodeFlagLargeOperator)
                     : 0U) |
                (symbol->movable_limits
                     ? static_cast<std::uint16_t>(MathNodeFlagMovableLimits)
                     : 0U);
            return text_node(MathNodeKind::Symbol,
                             symbol->text,
                             symbol->atom_class,
                             flags);
        }
        recover("unknown control sequence rendered literally", token.source_offset);
        return text_node(MathNodeKind::Symbol, "\\" + std::string(name));
    }

    MathNodeId parse_primary() {
        const MathToken token = current();
        switch (token.kind) {
        case MathTokenKind::BeginGroup:
            return parse_group();
        case MathTokenKind::Character:
        case MathTokenKind::OptionalBegin:
        case MathTokenKind::OptionalEnd: {
            ++position_;
            const std::uint32_t codepoint =
                typographic_math_codepoint(token.codepoint);
            std::string value;
            utf8_append(codepoint, value);
            return text_node(body_text_codepoint(codepoint) ? MathNodeKind::Text
                                                            : MathNodeKind::Symbol,
                             value,
                             character_class(codepoint));
        }
        case MathTokenKind::ControlSequence:
            return parse_control();
        case MathTokenKind::Whitespace:
            ++position_;
            return add(MathNode{});
        default:
            recover("unexpected math token", token.source_offset);
            ++position_;
            return text_node(MathNodeKind::Error, u8"⚠");
        }
    }

    std::string_view source_;
    const std::vector<MathToken>& tokens_;
    MathTree& tree_;
    std::size_t position_ = 0;
    std::size_t nesting_ = 0;
    bool hard_failure_ = false;
};

}  // namespace

bool parse_math(std::string_view source, MathTree& tree) {
    tree.clear();
    MathMacroExpansion expanded;
    if (!expand_safe_math_macros(source, expanded)) {
        tree.recovered_error = true;
        tree.diagnostic = expanded.error;
        return false;
    }
    MathLexResult lexed;
    if (!lex_math(expanded.source, lexed)) {
        tree.recovered_error = true;
        tree.diagnostic = lexed.error;
        tree.diagnostic_offset = lexed.error_offset;
        return false;
    }
    Parser parser(expanded.source, lexed.tokens, tree);
    return parser.run();
}

}  // namespace nmarkdown
