#include "nmarkdown/document/markdown.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nmarkdown/document/entity.h"
#include "nmarkdown/document/utf8.h"

extern "C" {
#include "md4c.h"
}

namespace nmarkdown {

std::size_t MarkdownDocument::source_size() const {
    if (!source_segments.empty()) {
        return static_cast<std::size_t>(source_segment_offsets.back()) +
               source_segments.back().size();
    }
    return source.size();
}

std::size_t MarkdownDocument::source_chunk_count() const {
    return source_segments.empty() ? (source.empty() ? 0U : 1U)
                                   : source_segments.size();
}

std::string_view MarkdownDocument::source_chunk(std::size_t index) const {
    if (!source_segments.empty()) {
        return index < source_segments.size()
                   ? std::string_view(source_segments[index])
                   : std::string_view();
    }
    return index == 0 ? std::string_view(source) : std::string_view();
}

std::uint32_t MarkdownDocument::source_chunk_offset(std::size_t index) const {
    if (!source_segments.empty()) {
        return index < source_segment_offsets.size()
                   ? source_segment_offsets[index]
                   : static_cast<std::uint32_t>(source_size());
    }
    return 0;
}

std::string_view MarkdownDocument::text(TextRef ref) const {
    if (ref.storage != TextStorageKind::SegmentedSource) {
        return ir.text(ref, source);
    }
    if (ref.length == 0 || source_segments.empty()) return {};
    const auto following = std::upper_bound(source_segment_offsets.begin(),
                                            source_segment_offsets.end(),
                                            ref.offset);
    if (following == source_segment_offsets.begin()) return {};
    const std::size_t index = static_cast<std::size_t>(
        following - source_segment_offsets.begin() - 1);
    const std::size_t local = ref.offset - source_segment_offsets[index];
    if (local > source_segments[index].size() ||
        ref.length > source_segments[index].size() - local) {
        return {};
    }
    return std::string_view(source_segments[index]).substr(local, ref.length);
}
namespace {

class MarkdownBuilder {
public:
    MarkdownBuilder(MarkdownDocument& document, std::string& error)
        : document_(document), error_(error) {}

    int enter_block(MD_BLOCKTYPE type, void* detail) {
        if (type == MD_BLOCK_DOC) {
            block_stack_.push_back(kInvalidNode);
            return 0;
        }

        BlockRecord record;
        record.kind = block_kind(type);
        record.depth = static_cast<std::uint8_t>(
            std::min<std::size_t>(255, block_stack_.empty() ? 0 : block_stack_.size() - 1));
        record.parent = block_stack_.empty() ? kInvalidNode : block_stack_.back();
        record.first_token = static_cast<TokenId>(document_.ir.tokens.size());
        record.estimated_height_26_6 = estimated_height(record.kind);
        apply_block_detail(type, detail, record);

        const NodeId node = static_cast<NodeId>(document_.ir.blocks.size());
        document_.ir.blocks.push_back(record);
        last_child_.push_back(kInvalidNode);
        if (record.parent == kInvalidNode) {
            if (document_.ir.first_block == kInvalidNode) {
                document_.ir.first_block = node;
            } else {
                document_.ir.blocks[root_last_].next_sibling = node;
            }
            root_last_ = node;
        } else {
            BlockRecord& parent = document_.ir.blocks[record.parent];
            if (parent.first_child == kInvalidNode) {
                parent.first_child = node;
            } else {
                document_.ir.blocks[last_child_[record.parent]].next_sibling = node;
            }
            last_child_[record.parent] = node;
        }
        block_stack_.push_back(node);
        return 0;
    }

    int leave_block(MD_BLOCKTYPE type, void* detail) {
        (void)detail;
        if (type == MD_BLOCK_DOC) {
            if (!block_stack_.empty()) {
                block_stack_.pop_back();
            }
            return 0;
        }
        if (block_stack_.empty() || block_stack_.back() == kInvalidNode) {
            error_ = "MD4C emitted an unbalanced block callback";
            return 1;
        }

        const NodeId node = block_stack_.back();
        block_stack_.pop_back();
        BlockRecord& block = document_.ir.blocks[node];
        block.token_count = static_cast<std::uint32_t>(document_.ir.tokens.size() -
                                                       block.first_token);
        finish_source_range(block);
        if (block.kind == BlockKind::Heading) {
            std::string title;
            const std::size_t token_end = std::min<std::size_t>(
                document_.ir.tokens.size(),
                static_cast<std::size_t>(block.first_token) + block.token_count);
            for (std::size_t index = block.first_token; index < token_end; ++index) {
                const InlineToken& token = document_.ir.tokens[index];
                if (!token.text.empty()) {
                    const std::string_view text = document_.text(token.text);
                    title.append(text.data(), text.size());
                } else if ((token.kind == InlineKind::SoftBreak ||
                            token.kind == InlineKind::HardBreak) &&
                           !title.empty() && title.back() != ' ') {
                    title.push_back(' ');
                }
            }
            document_.ir.headings.push_back(
                {block.source_offset,
                 node,
                 static_cast<std::uint8_t>(std::max<std::uint32_t>(1, block.aux)),
                 block.first_token,
                 block.token_count,
                 document_.ir.own(title)});
        }
        return 0;
    }

    int enter_span(MD_SPANTYPE type, void* detail) {
        switch (type) {
        case MD_SPAN_EM:
            add_boundary(InlineKind::EmphasisStart);
            ++emphasis_depth_;
            break;
        case MD_SPAN_STRONG:
            add_boundary(InlineKind::StrongStart);
            ++strong_depth_;
            break;
        case MD_SPAN_DEL:
            add_boundary(InlineKind::StrikethroughStart);
            ++strikethrough_depth_;
            break;
        case MD_SPAN_CODE:
            add_boundary(InlineKind::CodeStart);
            ++code_depth_;
            break;
        case MD_SPAN_A: {
            const auto* link = static_cast<const MD_SPAN_A_DETAIL*>(detail);
            const std::uint32_t id = add_link(link->href, link->title, false,
                                              link->is_autolink != 0);
            link_stack_.push_back(id);
            InlineToken token;
            token.kind = InlineKind::LinkStart;
            token.aux = id;
            document_.ir.tokens.push_back(token);
            ++link_depth_;
            break;
        }
        case MD_SPAN_IMG: {
            const auto* image = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
            const std::uint32_t id = add_link(image->src, image->title, true, false);
            image_stack_.push_back(id);
            InlineToken token;
            token.kind = InlineKind::ImageStart;
            token.aux = id;
            document_.ir.tokens.push_back(token);
            break;
        }
        case MD_SPAN_LATEXMATH:
            if (inline_math_depth_ == 0 && display_math_depth_ == 0) {
                begin_math(InlineKind::InlineMath);
            }
            ++inline_math_depth_;
            break;
        case MD_SPAN_LATEXMATH_DISPLAY:
            if (display_math_depth_ == 0 && inline_math_depth_ == 0) {
                begin_math(InlineKind::DisplayMath);
            }
            ++display_math_depth_;
            break;
        default:
            break;
        }
        return 0;
    }

    int leave_span(MD_SPANTYPE type, void* detail) {
        (void)detail;
        switch (type) {
        case MD_SPAN_EM:
            if (emphasis_depth_ > 0) --emphasis_depth_;
            add_boundary(InlineKind::EmphasisEnd);
            break;
        case MD_SPAN_STRONG:
            if (strong_depth_ > 0) --strong_depth_;
            add_boundary(InlineKind::StrongEnd);
            break;
        case MD_SPAN_DEL:
            if (strikethrough_depth_ > 0) --strikethrough_depth_;
            add_boundary(InlineKind::StrikethroughEnd);
            break;
        case MD_SPAN_CODE:
            if (code_depth_ > 0) --code_depth_;
            add_boundary(InlineKind::CodeEnd);
            break;
        case MD_SPAN_A: {
            if (link_depth_ > 0) --link_depth_;
            InlineToken token;
            token.kind = InlineKind::LinkEnd;
            if (!link_stack_.empty()) {
                token.aux = link_stack_.back();
                link_stack_.pop_back();
            }
            document_.ir.tokens.push_back(token);
            break;
        }
        case MD_SPAN_IMG: {
            InlineToken token;
            token.kind = InlineKind::ImageEnd;
            if (!image_stack_.empty()) {
                token.aux = image_stack_.back();
                image_stack_.pop_back();
            }
            document_.ir.tokens.push_back(token);
            break;
        }
        case MD_SPAN_LATEXMATH:
            if (inline_math_depth_ > 0) {
                --inline_math_depth_;
                if (inline_math_depth_ == 0) flush_math();
            }
            break;
        case MD_SPAN_LATEXMATH_DISPLAY:
            if (display_math_depth_ > 0) {
                --display_math_depth_;
                if (display_math_depth_ == 0) flush_math();
            }
            break;
        default:
            break;
        }
        return 0;
    }

    int text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size) {
        InlineKind kind = InlineKind::Text;
        switch (type) {
        case MD_TEXT_BR:
            add_break(InlineKind::HardBreak, text, size);
            return 0;
        case MD_TEXT_SOFTBR:
            add_break(InlineKind::SoftBreak, text, size);
            return 0;
        case MD_TEXT_NULLCHAR: {
            std::string replacement;
            utf8_append(kReplacementCodepoint, replacement);
            add_text(kind, text, size, document_.ir.own(replacement));
            return 0;
        }
        case MD_TEXT_ENTITY: {
            std::string decoded;
            const std::string_view source(text, size);
            if (decode_html_entity(source, decoded)) {
                add_text(kind, text, size, document_.ir.own(decoded));
            } else {
                add_text(kind, text, size, store_text(text, size));
            }
            return 0;
        }
        case MD_TEXT_CODE:
            kind = InlineKind::Code;
            break;
        case MD_TEXT_LATEXMATH:
            if (inline_math_depth_ > 0 || display_math_depth_ > 0) {
                append_math_text(text, size);
                return 0;
            }
            kind = InlineKind::InlineMath;
            break;
        case MD_TEXT_NORMAL:
        case MD_TEXT_HTML:
            break;
        }
        add_text(kind, text, size, store_text(text, size));
        return 0;
    }

    void debug(const char* message) {
        if (message != nullptr && error_.empty()) {
            error_ = message;
        }
    }

private:
    static BlockKind block_kind(MD_BLOCKTYPE type) {
        switch (type) {
        case MD_BLOCK_QUOTE: return BlockKind::Quote;
        case MD_BLOCK_UL: return BlockKind::UnorderedList;
        case MD_BLOCK_OL: return BlockKind::OrderedList;
        case MD_BLOCK_LI: return BlockKind::ListItem;
        case MD_BLOCK_HR: return BlockKind::HorizontalRule;
        case MD_BLOCK_H: return BlockKind::Heading;
        case MD_BLOCK_CODE: return BlockKind::CodeBlock;
        case MD_BLOCK_TABLE: return BlockKind::Table;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY: return BlockKind::TableSection;
        case MD_BLOCK_TR: return BlockKind::TableRow;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: return BlockKind::TableCell;
        case MD_BLOCK_P:
        case MD_BLOCK_HTML:
        default: return BlockKind::Paragraph;
        }
    }

    static Fx estimated_height(BlockKind kind) {
        switch (kind) {
        case BlockKind::Heading: return fx_from_int(34);
        case BlockKind::CodeBlock: return fx_from_int(54);
        case BlockKind::HorizontalRule: return fx_from_int(14);
        case BlockKind::Table: return fx_from_int(72);
        case BlockKind::Paragraph: return fx_from_int(25);
        default: return fx_from_int(18);
        }
    }

    void apply_block_detail(MD_BLOCKTYPE type, void* detail, BlockRecord& block) {
        if (detail == nullptr) return;
        switch (type) {
        case MD_BLOCK_UL: {
            const auto* value = static_cast<const MD_BLOCK_UL_DETAIL*>(detail);
            if (value->is_tight) block.flags |= BlockFlagTight;
            block.aux = static_cast<unsigned char>(value->mark);
            break;
        }
        case MD_BLOCK_OL: {
            const auto* value = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
            if (value->is_tight) block.flags |= BlockFlagTight;
            block.aux = value->start;
            break;
        }
        case MD_BLOCK_LI: {
            const auto* value = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
            if (value->is_task) {
                block.flags |= BlockFlagTask;
                if (value->task_mark == 'x' || value->task_mark == 'X') {
                    block.flags |= BlockFlagChecked;
                }
            }
            break;
        }
        case MD_BLOCK_H:
            block.aux = static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level;
            break;
        case MD_BLOCK_CODE: {
            const auto* value = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
            block.metadata = store_attribute(value->lang);
            break;
        }
        case MD_BLOCK_TABLE:
            block.aux = static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail)->col_count;
            break;
        case MD_BLOCK_TH:
            block.flags |= BlockFlagTableHeader;
            apply_alignment(static_cast<const MD_BLOCK_TD_DETAIL*>(detail)->align, block);
            break;
        case MD_BLOCK_TD:
            apply_alignment(static_cast<const MD_BLOCK_TD_DETAIL*>(detail)->align, block);
            break;
        default:
            break;
        }
    }

    static void apply_alignment(MD_ALIGN align, BlockRecord& block) {
        if (align == MD_ALIGN_LEFT) block.flags |= BlockFlagAlignLeft;
        if (align == MD_ALIGN_CENTER) block.flags |= BlockFlagAlignCenter;
        if (align == MD_ALIGN_RIGHT) block.flags |= BlockFlagAlignRight;
    }

    bool source_span(const char* text,
                     std::size_t size,
                     std::uint32_t& offset) const {
        if (text == nullptr || document_.source.empty()) return false;
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(document_.source.data());
        const std::uintptr_t pointer = reinterpret_cast<std::uintptr_t>(text);
        if (pointer < base || pointer - base > document_.source.size()) return false;
        const std::size_t relative = static_cast<std::size_t>(pointer - base);
        if (size > document_.source.size() - relative) return false;
        offset = static_cast<std::uint32_t>(relative);
        return true;
    }

    TextRef store_text(const char* text, std::size_t size) {
        if (size == 0) return {};
        std::uint32_t offset = 0;
        if (source_span(text, size, offset)) {
            return {offset, static_cast<std::uint32_t>(size), TextStorageKind::Source};
        }
        return document_.ir.own(std::string_view(text, size));
    }

    TextRef store_attribute(const MD_ATTRIBUTE& attribute) {
        if (attribute.text == nullptr || attribute.size == 0) return {};
        bool needs_decode = false;
        if (attribute.substr_types != nullptr && attribute.substr_offsets != nullptr) {
            for (MD_SIZE index = 0; attribute.substr_offsets[index] < attribute.size; ++index) {
                if (attribute.substr_types[index] != MD_TEXT_NORMAL) {
                    needs_decode = true;
                    break;
                }
            }
        }
        if (!needs_decode) {
            return store_text(attribute.text, attribute.size);
        }

        std::string decoded;
        for (MD_SIZE index = 0; attribute.substr_offsets[index] < attribute.size; ++index) {
            const MD_OFFSET begin = attribute.substr_offsets[index];
            const MD_OFFSET end = attribute.substr_offsets[index + 1];
            const std::string_view part(attribute.text + begin, end - begin);
            if (attribute.substr_types[index] == MD_TEXT_ENTITY) {
                std::string entity;
                if (decode_html_entity(part, entity)) decoded += entity;
                else decoded.append(part.data(), part.size());
            } else if (attribute.substr_types[index] == MD_TEXT_NULLCHAR) {
                utf8_append(kReplacementCodepoint, decoded);
            } else {
                decoded.append(part.data(), part.size());
            }
        }
        return document_.ir.own(decoded);
    }

    std::uint8_t style_flags() const {
        std::uint8_t result = InlineStyleNone;
        if (emphasis_depth_ > 0) result |= InlineStyleEmphasis;
        if (strong_depth_ > 0) result |= InlineStyleStrong;
        if (strikethrough_depth_ > 0) result |= InlineStyleStrikethrough;
        if (code_depth_ > 0) result |= InlineStyleCode;
        if (link_depth_ > 0) result |= InlineStyleLink;
        return result;
    }

    void begin_math(InlineKind kind) {
        pending_math_kind_ = kind;
        pending_math_style_ = style_flags();
        pending_math_.clear();
        pending_math_first_ = UINT_MAX;
        pending_math_last_ = 0;
        pending_math_active_ = true;
    }

    void append_math_text(const char* source, std::size_t size) {
        if (!pending_math_active_) {
            begin_math(display_math_depth_ > 0 ? InlineKind::DisplayMath
                                               : InlineKind::InlineMath);
        }
        if (source != nullptr && size != 0) {
            pending_math_.append(source, size);
        }
        std::uint32_t offset = 0;
        if (source_span(source, size, offset)) {
            pending_math_first_ = std::min(pending_math_first_, offset);
            pending_math_last_ = std::max(
                pending_math_last_, offset + static_cast<std::uint32_t>(size));
        }
    }

    void flush_math() {
        if (!pending_math_active_) return;
        if (!pending_math_.empty()) {
            InlineToken token;
            token.kind = pending_math_kind_;
            token.style_flags = pending_math_style_;
            token.text = document_.ir.own(pending_math_);
            if (pending_math_first_ != UINT_MAX) {
                token.source_offset = pending_math_first_;
                token.source_length = pending_math_last_ - pending_math_first_;
            }
            document_.ir.tokens.push_back(std::move(token));
        }
        pending_math_.clear();
        pending_math_first_ = UINT_MAX;
        pending_math_last_ = 0;
        pending_math_active_ = false;
    }

    void add_text(InlineKind kind,
                  const char* source,
                  std::size_t source_size,
                  TextRef text) {
        if (source_size == 0 && text.empty()) return;
        InlineToken token;
        token.kind = kind;
        token.style_flags = style_flags();
        if (!link_stack_.empty()) token.aux = link_stack_.back();
        else if (!image_stack_.empty()) token.aux = image_stack_.back();
        token.text = text;
        if (source_span(source, source_size, token.source_offset)) {
            token.source_length = static_cast<std::uint32_t>(source_size);
        }
        document_.ir.tokens.push_back(token);
    }

    void add_break(InlineKind kind, const char* source, std::size_t source_size) {
        InlineToken token;
        token.kind = kind;
        token.style_flags = style_flags();
        if (!link_stack_.empty()) token.aux = link_stack_.back();
        else if (!image_stack_.empty()) token.aux = image_stack_.back();
        if (source_span(source, source_size, token.source_offset)) {
            token.source_length = static_cast<std::uint32_t>(source_size);
        }
        document_.ir.tokens.push_back(token);
    }

    void add_boundary(InlineKind kind) {
        InlineToken token;
        token.kind = kind;
        token.style_flags = style_flags();
        document_.ir.tokens.push_back(token);
    }

    std::uint32_t add_link(const MD_ATTRIBUTE& target,
                           const MD_ATTRIBUTE& title,
                           bool image,
                           bool autolink) {
        LinkRecord link;
        link.target = store_attribute(target);
        link.title = store_attribute(title);
        link.image = image;
        link.autolink = autolink;
        const std::uint32_t id = static_cast<std::uint32_t>(document_.ir.links.size());
        document_.ir.links.push_back(link);
        return id;
    }

    void finish_source_range(BlockRecord& block) {
        std::uint32_t first = UINT_MAX;
        std::uint32_t last = 0;
        const std::size_t end = static_cast<std::size_t>(block.first_token) +
                                block.token_count;
        for (std::size_t index = block.first_token;
             index < end && index < document_.ir.tokens.size();
             ++index) {
            const InlineToken& token = document_.ir.tokens[index];
            if (token.source_length == 0) continue;
            first = std::min(first, token.source_offset);
            last = std::max(last, token.source_offset + token.source_length);
        }
        if (first != UINT_MAX) {
            block.source_offset = first;
            block.source_length = last - first;
        }
    }

    MarkdownDocument& document_;
    std::string& error_;
    std::vector<NodeId> block_stack_;
    std::vector<NodeId> last_child_;
    NodeId root_last_ = kInvalidNode;
    std::vector<std::uint32_t> link_stack_;
    std::vector<std::uint32_t> image_stack_;
    unsigned emphasis_depth_ = 0;
    unsigned strong_depth_ = 0;
    unsigned strikethrough_depth_ = 0;
    unsigned code_depth_ = 0;
    unsigned link_depth_ = 0;
    unsigned inline_math_depth_ = 0;
    unsigned display_math_depth_ = 0;
    InlineKind pending_math_kind_ = InlineKind::InlineMath;
    std::uint8_t pending_math_style_ = InlineStyleNone;
    std::string pending_math_;
    std::uint32_t pending_math_first_ = UINT_MAX;
    std::uint32_t pending_math_last_ = 0;
    bool pending_math_active_ = false;
};

int enter_block_callback(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<MarkdownBuilder*>(userdata)->enter_block(type, detail);
}
int leave_block_callback(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<MarkdownBuilder*>(userdata)->leave_block(type, detail);
}
int enter_span_callback(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<MarkdownBuilder*>(userdata)->enter_span(type, detail);
}
int leave_span_callback(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<MarkdownBuilder*>(userdata)->leave_span(type, detail);
}
int text_callback(MD_TEXTTYPE type,
                  const MD_CHAR* text,
                  MD_SIZE size,
                  void* userdata) {
    return static_cast<MarkdownBuilder*>(userdata)->text(type, text, size);
}
void debug_callback(const char* message, void* userdata) {
    static_cast<MarkdownBuilder*>(userdata)->debug(message);
}

}  // namespace

void MarkdownDocument::clear() {
    source.clear();
    source_segments.clear();
    source_segment_offsets.clear();
    ir.clear();
    utf8 = {};
}

unsigned markdown_parser_flags() {
    return MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH |
           MD_FLAG_LATEXMATHSPANS | MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS;
}

bool parse_markdown(const std::uint8_t* bytes,
                    std::size_t size,
                    MarkdownDocument& document,
                    std::string& error) {
    document.clear();
    error.clear();
    if (bytes == nullptr && size != 0) {
        error = "Markdown source pointer is null";
        return false;
    }
    if (size > static_cast<std::size_t>(UINT_MAX)) {
        error = "Markdown source is too large for MD4C";
        return false;
    }

    document.utf8 = utf8_sanitize(bytes, size, document.source, true);
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = markdown_parser_flags();
    parser.enter_block = enter_block_callback;
    parser.leave_block = leave_block_callback;
    parser.enter_span = enter_span_callback;
    parser.leave_span = leave_span_callback;
    parser.text = text_callback;
    parser.debug_log = debug_callback;

    MarkdownBuilder builder(document, error);
    const int result = md_parse(document.source.data(),
                                static_cast<MD_SIZE>(document.source.size()),
                                &parser,
                                &builder);
    if (result != 0) {
        if (error.empty()) error = "MD4C could not parse the document";
        document.clear();
        return false;
    }
    return true;
}

namespace {

bool build_plain_text_ir(MarkdownDocument& document) {
    if (document.source.empty()) return true;

    // A single multi-megabyte paragraph would defeat the virtual layout's
    // incremental work budget. Keep each unit close to one calculator
    // viewport so a newly reached TXT chunk cannot shape hundreds of lines in
    // one input frame. Layout recognizes the continuation flags and removes
    // artificial padding between chunks, so this does not alter typesetting.
    constexpr std::size_t kMaximumLinesPerBlock = 8;
    std::size_t offset = 0;
    NodeId previous = kInvalidNode;
    while (offset < document.source.size()) {
        BlockRecord block;
        block.kind = BlockKind::Paragraph;
        block.flags = static_cast<std::uint16_t>(
            BlockFlagPlainText |
            (previous == kInvalidNode ? BlockFlagNone
                                      : BlockFlagTextContinuation));
        block.first_token = static_cast<TokenId>(document.ir.tokens.size());
        block.source_offset = static_cast<std::uint32_t>(offset);
        block.estimated_height_26_6 = fx_from_int(25);
        const std::size_t block_begin = offset;
        std::size_t lines = 0;

        while (offset < document.source.size() &&
               lines < kMaximumLinesPerBlock) {
            const std::size_t line_begin = offset;
            while (offset < document.source.size() &&
                   document.source[offset] != '\r' &&
                   document.source[offset] != '\n') {
                ++offset;
            }
            if (offset != line_begin) {
                InlineToken text;
                text.kind = InlineKind::Text;
                text.text = {static_cast<std::uint32_t>(line_begin),
                             static_cast<std::uint32_t>(offset - line_begin),
                             TextStorageKind::Source};
                text.source_offset = static_cast<std::uint32_t>(line_begin);
                text.source_length =
                    static_cast<std::uint32_t>(offset - line_begin);
                document.ir.tokens.push_back(text);
            }
            if (offset == document.source.size()) {
                ++lines;
                break;
            }

            const std::size_t newline_begin = offset;
            if (document.source[offset] == '\r' &&
                offset + 1 < document.source.size() &&
                document.source[offset + 1] == '\n') {
                offset += 2;
            } else {
                ++offset;
            }
            ++lines;
            // A block boundary itself advances to the next physical line. Do
            // not also append a hard break at an internal chunk boundary.
            if (lines == kMaximumLinesPerBlock &&
                offset < document.source.size()) {
                break;
            }
            InlineToken newline;
            newline.kind = InlineKind::HardBreak;
            newline.source_offset = static_cast<std::uint32_t>(newline_begin);
            newline.source_length =
                static_cast<std::uint32_t>(offset - newline_begin);
            document.ir.tokens.push_back(newline);
        }

        block.token_count = static_cast<std::uint32_t>(
            document.ir.tokens.size() - block.first_token);
        block.source_length = static_cast<std::uint32_t>(offset - block_begin);
        block.aux = static_cast<std::uint32_t>(lines);
        const NodeId node = static_cast<NodeId>(document.ir.blocks.size());
        document.ir.blocks.push_back(block);
        if (document.ir.first_block == kInvalidNode) {
            document.ir.first_block = node;
        }
        if (previous != kInvalidNode) {
            document.ir.blocks[previous].next_sibling = node;
        }
        previous = node;
    }
    return true;
}

bool build_segmented_plain_text_ir(MarkdownDocument& document) {
    const std::size_t total_size = document.source_size();
    if (total_size == 0) return true;

    constexpr std::size_t kMaximumLinesPerBlock = 8;
    std::size_t segment_index = 0;
    std::size_t local_offset = 0;
    std::size_t global_offset = 0;
    NodeId previous = kInvalidNode;

    const auto normalize_cursor = [&]() {
        while (segment_index < document.source_segments.size() &&
               local_offset == document.source_segments[segment_index].size()) {
            ++segment_index;
            local_offset = 0;
        }
    };
    const auto current_byte = [&]() -> char {
        return document.source_segments[segment_index][local_offset];
    };
    const auto peek_following = [&](char& value) -> bool {
        std::size_t peek_segment = segment_index;
        std::size_t peek_local = local_offset + 1;
        while (peek_segment < document.source_segments.size() &&
               peek_local == document.source_segments[peek_segment].size()) {
            ++peek_segment;
            peek_local = 0;
        }
        if (peek_segment >= document.source_segments.size()) return false;
        value = document.source_segments[peek_segment][peek_local];
        return true;
    };
    const auto advance = [&]() {
        ++local_offset;
        ++global_offset;
        normalize_cursor();
    };

    normalize_cursor();
    while (global_offset < total_size) {
        BlockRecord block;
        block.kind = BlockKind::Paragraph;
        block.flags = static_cast<std::uint16_t>(
            BlockFlagPlainText |
            (previous == kInvalidNode ? BlockFlagNone
                                      : BlockFlagTextContinuation));
        block.first_token = static_cast<TokenId>(document.ir.tokens.size());
        block.source_offset = static_cast<std::uint32_t>(global_offset);
        block.estimated_height_26_6 = fx_from_int(25);
        const std::size_t block_begin = global_offset;
        std::size_t lines = 0;

        while (global_offset < total_size &&
               lines < kMaximumLinesPerBlock) {
            // A text token never crosses a segment edge. Adjacent text tokens
            // do not imply a break, so this cap is invisible to layout.
            const std::size_t text_begin = global_offset;
            const std::size_t text_local_begin = local_offset;
            const std::size_t text_segment = segment_index;
            while (global_offset < total_size &&
                   segment_index == text_segment &&
                   current_byte() != '\r' && current_byte() != '\n') {
                advance();
            }
            if (global_offset != text_begin) {
                InlineToken text;
                text.kind = InlineKind::Text;
                text.text = {static_cast<std::uint32_t>(text_begin),
                             static_cast<std::uint32_t>(
                                 local_offset >= text_local_begin
                                     ? local_offset - text_local_begin
                                     : document.source_segments[text_segment].size() -
                                           text_local_begin),
                             TextStorageKind::SegmentedSource};
                // Crossing the segment edge normalizes local_offset to zero.
                if (segment_index != text_segment) {
                    text.text.length = static_cast<std::uint32_t>(
                        document.source_segments[text_segment].size() -
                        text_local_begin);
                }
                text.source_offset = static_cast<std::uint32_t>(text_begin);
                text.source_length = text.text.length;
                document.ir.tokens.push_back(text);
                // Continue the same physical line in the following segment.
                if (segment_index != text_segment) continue;
            }
            if (global_offset == total_size) {
                ++lines;
                break;
            }

            const std::size_t newline_begin = global_offset;
            if (current_byte() == '\r') {
                char following = 0;
                const bool crlf = peek_following(following) && following == '\n';
                advance();
                if (crlf && global_offset < total_size) advance();
            } else {
                advance();
            }
            ++lines;
            if (lines == kMaximumLinesPerBlock &&
                global_offset < total_size) {
                break;
            }
            InlineToken newline;
            newline.kind = InlineKind::HardBreak;
            newline.source_offset = static_cast<std::uint32_t>(newline_begin);
            newline.source_length = static_cast<std::uint32_t>(
                global_offset - newline_begin);
            document.ir.tokens.push_back(newline);
        }

        block.token_count = static_cast<std::uint32_t>(
            document.ir.tokens.size() - block.first_token);
        block.source_length = static_cast<std::uint32_t>(
            global_offset - block_begin);
        block.aux = static_cast<std::uint32_t>(lines);
        const NodeId node = static_cast<NodeId>(document.ir.blocks.size());
        document.ir.blocks.push_back(block);
        if (document.ir.first_block == kInvalidNode) {
            document.ir.first_block = node;
        }
        if (previous != kInvalidNode) {
            document.ir.blocks[previous].next_sibling = node;
        }
        previous = node;
    }
    return true;
}

}  // namespace

bool parse_plain_text(const std::uint8_t* utf8_bytes,
                      std::size_t size,
                      MarkdownDocument& document,
                      std::string& error) {
    document.clear();
    error.clear();
    if (utf8_bytes == nullptr && size != 0) {
        error = "plain-text source pointer is null";
        return false;
    }
    if (size > static_cast<std::size_t>(UINT_MAX)) {
        error = "plain-text source is too large";
        return false;
    }
    document.utf8 = utf8_sanitize(utf8_bytes, size, document.source, true);
    return build_plain_text_ir(document);
}

bool parse_plain_text(std::string&& utf8_source,
                      MarkdownDocument& document,
                      std::string& error) {
    document.clear();
    error.clear();
    if (utf8_source.size() > static_cast<std::size_t>(UINT_MAX)) {
        error = "plain-text source is too large";
        return false;
    }
    document.utf8 = utf8_validate(
        reinterpret_cast<const std::uint8_t*>(utf8_source.data()),
        utf8_source.size(), true);
    if (!document.utf8.valid()) {
        error = "decoded plain text is not valid UTF-8";
        document.clear();
        return false;
    }
    if (document.utf8.had_bom) {
        utf8_source.erase(0, 3);
        if (document.utf8.codepoint_count != 0) {
            --document.utf8.codepoint_count;
        }
    }
    document.source = std::move(utf8_source);
    return build_plain_text_ir(document);
}

bool parse_plain_text_segments(std::deque<std::string>&& utf8_segments,
                               const Utf8ValidationResult& validation,
                               MarkdownDocument& document,
                               std::string& error) {
    document.clear();
    error.clear();
    if (!validation.valid()) {
        error = "segmented plain text is not valid UTF-8";
        return false;
    }

    std::size_t total_size = 0;
    for (const std::string& segment : utf8_segments) {
        if (segment.empty()) continue;
        if (total_size > static_cast<std::size_t>(UINT_MAX) - segment.size()) {
            error = "plain-text source is too large";
            return false;
        }
        total_size += segment.size();
    }
    std::uint32_t offset = 0;
    for (std::string& segment : utf8_segments) {
        if (segment.empty()) continue;
        document.source_segment_offsets.push_back(offset);
        offset += static_cast<std::uint32_t>(segment.size());
        document.source_segments.push_back(std::move(segment));
    }
    document.utf8 = validation;
    return build_segmented_plain_text_ir(document);
}

}  // namespace nmarkdown
