#ifndef NMARKDOWN_DOCUMENT_DOCUMENT_IR_H
#define NMARKDOWN_DOCUMENT_DOCUMENT_IR_H

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "nmarkdown/layout/fixed.h"

namespace nmarkdown {

using NodeId = std::uint32_t;
using TokenId = std::uint32_t;
constexpr NodeId kInvalidNode = 0xFFFFFFFFU;
constexpr TokenId kInvalidToken = 0xFFFFFFFFU;

enum class BlockKind : std::uint8_t {
    Paragraph,
    Heading,
    CodeBlock,
    Quote,
    UnorderedList,
    OrderedList,
    ListItem,
    HorizontalRule,
    Table,
    TableSection,
    TableRow,
    TableCell,
    DisplayMath,
};

enum BlockFlags : std::uint16_t {
    BlockFlagNone = 0,
    BlockFlagTight = 1U << 0U,
    BlockFlagTask = 1U << 1U,
    BlockFlagChecked = 1U << 2U,
    BlockFlagTableHeader = 1U << 3U,
    BlockFlagAlignLeft = 1U << 4U,
    BlockFlagAlignCenter = 1U << 5U,
    BlockFlagAlignRight = 1U << 6U,
    // Literal-text layout fixtures can split a source into small virtual
    // quanta. Runtime TXT bypasses DocumentIR and does not use these flags.
    BlockFlagPlainText = 1U << 7U,
    BlockFlagTextContinuation = 1U << 8U,
};

enum class TextStorageKind : std::uint8_t {
    Empty,
    Source,
    SegmentedSource,
    Owned,
};

struct TextRef {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    TextStorageKind storage = TextStorageKind::Empty;

    bool empty() const { return length == 0; }
};

struct BlockRecord {
    BlockKind kind = BlockKind::Paragraph;
    std::uint8_t depth = 0;
    std::uint16_t flags = BlockFlagNone;
    NodeId parent = kInvalidNode;
    NodeId first_child = kInvalidNode;
    NodeId next_sibling = kInvalidNode;
    TokenId first_token = 0;
    std::uint32_t token_count = 0;
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
    Fx estimated_height_26_6 = 0;
    Fx measured_height_26_6 = 0;
    std::uint32_t aux = 0;
    TextRef metadata;
};

enum class InlineKind : std::uint8_t {
    Text,
    SoftBreak,
    HardBreak,
    EmphasisStart,
    EmphasisEnd,
    StrongStart,
    StrongEnd,
    StrikethroughStart,
    StrikethroughEnd,
    CodeStart,
    CodeEnd,
    Code,
    LinkStart,
    LinkEnd,
    InlineMath,
    DisplayMath,
    ImageStart,
    ImageEnd,
};

enum InlineStyleFlags : std::uint8_t {
    InlineStyleNone = 0,
    InlineStyleEmphasis = 1U << 0U,
    InlineStyleStrong = 1U << 1U,
    InlineStyleStrikethrough = 1U << 2U,
    InlineStyleCode = 1U << 3U,
    InlineStyleLink = 1U << 4U,
};

struct InlineToken {
    InlineKind kind = InlineKind::Text;
    std::uint8_t style_flags = InlineStyleNone;
    std::uint16_t reserved = 0;
    std::uint32_t aux = 0;
    TextRef text;
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
};

struct HeadingEntry {
    std::uint32_t source_offset = 0;
    NodeId block = kInvalidNode;
    std::uint8_t level = 1;
    TokenId first_token = 0;
    std::uint32_t token_count = 0;
    TextRef title;
};

struct LinkRecord {
    TextRef target;
    TextRef title;
    bool image = false;
    bool autolink = false;
};

struct DocumentIR {
    // These collections can exceed the calculator heap's largest contiguous
    // hole for a long TXT file. deque preserves indexed access and stable
    // NodeId/TokenId semantics while growing in small fixed-size blocks.
    std::deque<BlockRecord> blocks;
    std::deque<InlineToken> tokens;
    std::vector<HeadingEntry> headings;
    std::vector<LinkRecord> links;
    std::string string_arena;
    NodeId first_block = kInvalidNode;

    void clear();
    TextRef own(std::string_view text);
    std::string_view text(TextRef ref, std::string_view source) const;
};

}  // namespace nmarkdown

#endif
