#include "nmarkdown/layout/block_layout.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "nmarkdown/document/utf8.h"
#include "nmarkdown/math/math_system.h"
#include "nmarkdown/text/text_system.h"


namespace nmarkdown {
namespace {

// Five viewports of the smallest standard block (a 14 px rule) fit below
// this cap, so pointers returned for one render window remain resident.
constexpr std::size_t kMaximumCachedBlocks = 96;

bool renderable_kind(BlockKind kind) {
    switch (kind) {
    case BlockKind::Paragraph:
    case BlockKind::Heading:
    case BlockKind::CodeBlock:
    case BlockKind::HorizontalRule:
    case BlockKind::TableRow:
    case BlockKind::DisplayMath:
        return true;
    default:
        return false;
    }
}

bool is_descendant(const DocumentIR& ir, NodeId node, NodeId ancestor) {
    while (node != kInvalidNode && node < ir.blocks.size()) {
        if (node == ancestor) return true;
        node = ir.blocks[node].parent;
    }
    return false;
}

NodeId ancestor_of_kind(const DocumentIR& ir, NodeId node, BlockKind kind) {
    while (node != kInvalidNode && node < ir.blocks.size()) {
        if (ir.blocks[node].kind == kind) return node;
        node = ir.blocks[node].parent;
    }
    return kInvalidNode;
}

std::uint32_t ordered_item_number(const DocumentIR& ir, NodeId item) {
    if (item == kInvalidNode || item >= ir.blocks.size()) return 1;
    const NodeId list = ir.blocks[item].parent;
    if (list == kInvalidNode || list >= ir.blocks.size() ||
        ir.blocks[list].kind != BlockKind::OrderedList) {
        return 1;
    }
    std::uint32_t number = std::max<std::uint32_t>(1, ir.blocks[list].aux);
    for (NodeId child = ir.blocks[list].first_child;
         child != kInvalidNode && child < ir.blocks.size() && child != item;
         child = ir.blocks[child].next_sibling) {
        if (ir.blocks[child].kind == BlockKind::ListItem) ++number;
    }
    return number;
}

Fx heading_pixel_size(const LayoutSignature& signature, std::uint32_t level) {
    static constexpr int additions[] = {0, 9, 6, 3, 2, 1, 0};
    const std::uint32_t bounded = std::min<std::uint32_t>(6, std::max<std::uint32_t>(1, level));
    return fx_from_int(signature.body_px + additions[bounded]);
}

std::size_t next_codepoint_end(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return text.size();
    const DecodedCodepoint decoded = utf8_next(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size(),
        static_cast<std::uint32_t>(offset));
    const std::size_t length = decoded.byte_length == 0 ? 1 : decoded.byte_length;
    return std::min(text.size(), offset + length);
}

std::size_t previous_codepoint_begin(std::string_view text, std::size_t offset) {
    if (offset == 0) return 0;
    std::size_t result = offset - 1;
    while (result > 0 &&
           (static_cast<unsigned char>(text[result]) & 0xC0U) == 0x80U) {
        --result;
    }
    return result;
}

std::uint32_t codepoint_at(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return 0;
    return utf8_next(reinterpret_cast<const std::uint8_t*>(text.data()),
                     text.size(), static_cast<std::uint32_t>(offset)).value;
}

bool prohibited_line_start(std::uint32_t codepoint) {
    switch (codepoint) {
    case '!': case '%': case ')': case ',': case '.': case ':': case ';': case '?':
    case ']': case '}':
    case 0x00BBU: case 0x2019U: case 0x201DU: case 0x2026U:
    case 0x3001U: case 0x3002U: case 0x3009U: case 0x300BU: case 0x300DU:
    case 0x300FU: case 0x3011U: case 0x3015U: case 0x3017U: case 0x3019U:
    case 0x301BU: case 0xFF01U: case 0xFF09U: case 0xFF0CU: case 0xFF0EU:
    case 0xFF1AU: case 0xFF1BU: case 0xFF1FU: case 0xFF3DU: case 0xFF5DU:
        return true;
    default:
        return false;
    }
}

bool prohibited_line_end(std::uint32_t codepoint) {
    switch (codepoint) {
    case '(': case '[': case '{':
    case 0x00ABU: case 0x2018U: case 0x201CU:
    case 0x3008U: case 0x300AU: case 0x300CU: case 0x300EU: case 0x3010U:
    case 0x3014U: case 0x3016U: case 0x3018U: case 0x301AU: case 0xFF08U:
    case 0xFF3BU: case 0xFF5BU:
        return true;
    default:
        return false;
    }
}

std::size_t punctuation_safe_break(std::string_view text,
                                   std::size_t begin,
                                   std::size_t candidate) {
    if (candidate <= begin || candidate >= text.size()) return candidate;
    const std::size_t previous = previous_codepoint_begin(text, candidate);
    if ((prohibited_line_start(codepoint_at(text, candidate)) ||
         prohibited_line_end(codepoint_at(text, previous))) &&
        previous > begin) {
        return previous;
    }
    return candidate;
}

struct SourcePiece {
    std::string text;
    std::uint8_t style = InlineStyleNone;
    InlineKind kind = InlineKind::Text;
    bool hard_break = false;
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
    bool exact_source_mapping = false;
    std::uint32_t link_id = kInvalidToken;
    Fx minimum_x = -1;
};

bool table_row_is_header(const MarkdownDocument& document, const BlockRecord& row) {
    for (NodeId cell = row.first_child;
         cell != kInvalidNode && cell < document.ir.blocks.size();
         cell = document.ir.blocks[cell].next_sibling) {
        const BlockRecord& candidate = document.ir.blocks[cell];
        if (candidate.kind == BlockKind::TableCell) {
            return (candidate.flags & BlockFlagTableHeader) != 0;
        }
    }
    return false;
}

NodeId table_ancestor(const DocumentIR& ir, NodeId node) {
    return ancestor_of_kind(ir, node, BlockKind::Table);
}

NodeId first_table_row(const DocumentIR& ir, NodeId table) {
    if (table == kInvalidNode || table >= ir.blocks.size()) return kInvalidNode;
    for (NodeId node = table + 1; node < ir.blocks.size(); ++node) {
        if (!is_descendant(ir, node, table)) break;
        if (ir.blocks[node].kind == BlockKind::TableRow) return node;
    }
    return kInvalidNode;
}

std::string block_plain_text(const MarkdownDocument& document,
                             const BlockRecord& block) {
    std::string result;
    const std::size_t end = std::min<std::size_t>(
        document.ir.tokens.size(),
        static_cast<std::size_t>(block.first_token) + block.token_count);
    for (std::size_t index = block.first_token; index < end; ++index) {
        const InlineToken& token = document.ir.tokens[index];
        if (!token.text.empty()) {
            const std::string_view value = document.text(token.text);
            result.append(value.data(), value.size());
        } else if ((token.kind == InlineKind::SoftBreak ||
                    token.kind == InlineKind::HardBreak) && !result.empty()) {
            result.push_back(' ');
        }
    }
    return result;
}

std::vector<Fx> table_column_widths(const MarkdownDocument& document,
                                    NodeId row_id,
                                    TextSystem& text,
                                    Fx pixel_size) {
    std::vector<Fx> widths;
    if (row_id >= document.ir.blocks.size()) return widths;
    const NodeId table = table_ancestor(document.ir, row_id);
    if (table == kInvalidNode) return widths;
    for (NodeId node = table + 1; node < document.ir.blocks.size(); ++node) {
        if (!is_descendant(document.ir, node, table)) break;
        const BlockRecord& candidate = document.ir.blocks[node];
        if (candidate.kind != BlockKind::TableRow) continue;
        std::size_t column = 0;
        for (NodeId cell = candidate.first_child;
             cell != kInvalidNode && cell < document.ir.blocks.size();
             cell = document.ir.blocks[cell].next_sibling) {
            if (document.ir.blocks[cell].kind != BlockKind::TableCell) continue;
            if (column >= widths.size()) widths.resize(column + 1, 0);
            const std::string plain = block_plain_text(document,
                                                       document.ir.blocks[cell]);
            GlyphRun glyphs;
            if (text.shape(plain.data(), plain.size(), pixel_size, glyphs)) {
                widths[column] = std::max(widths[column],
                                          glyphs.width + fx_from_int(18));
            }
            ++column;
        }
    }
    return widths;
}

void append_token_pieces(const MarkdownDocument& document,
                         const BlockRecord& block,
                         std::vector<SourcePiece>& pieces) {
    const std::size_t end = std::min<std::size_t>(
        document.ir.tokens.size(),
        static_cast<std::size_t>(block.first_token) + block.token_count);
    for (std::size_t index = block.first_token; index < end; ++index) {
        const InlineToken& token = document.ir.tokens[index];
        if (token.kind == InlineKind::HardBreak) {
            pieces.push_back({{}, token.style_flags, token.kind, true,
                              token.source_offset, token.source_length, false,
                              token.aux});
        } else if (token.kind == InlineKind::SoftBreak) {
            pieces.push_back({" ", token.style_flags, token.kind, false,
                              token.source_offset, token.source_length, false,
                              token.aux});
        } else if (!token.text.empty()) {
            const std::string_view text = document.text(token.text);
            pieces.push_back({std::string(text), token.style_flags, token.kind, false,
                              token.source_offset, token.source_length,
                              token.source_length == text.size(), token.aux});
        }
    }
}

void append_table_pieces(const MarkdownDocument& document,
                         NodeId row_id,
                         std::vector<SourcePiece>& pieces,
                         bool responsive,
                         const std::vector<Fx>* column_widths = nullptr) {
    if (row_id >= document.ir.blocks.size()) return;
    const BlockRecord& row = document.ir.blocks[row_id];
    std::vector<NodeId> headers;
    if (responsive) {
        const NodeId header_row = first_table_row(
            document.ir, table_ancestor(document.ir, row_id));
        if (header_row != kInvalidNode) {
            for (NodeId cell = document.ir.blocks[header_row].first_child;
                 cell != kInvalidNode && cell < document.ir.blocks.size();
                 cell = document.ir.blocks[cell].next_sibling) {
                if (document.ir.blocks[cell].kind == BlockKind::TableCell) {
                    headers.push_back(cell);
                }
            }
        }
    }
    bool first = true;
    std::size_t column = 0;
    Fx column_x = 0;
    for (NodeId cell = row.first_child;
         cell != kInvalidNode && cell < document.ir.blocks.size();
         cell = document.ir.blocks[cell].next_sibling) {
        if (document.ir.blocks[cell].kind != BlockKind::TableCell) continue;
        if (!first) {
            if (responsive) {
                pieces.push_back({{}, InlineStyleNone, InlineKind::HardBreak, true,
                                  0, 0, false});
            } else {
                SourcePiece separator{"│", InlineStyleNone, InlineKind::Text, false,
                                      0, 0, false};
                separator.minimum_x = std::max<Fx>(0, column_x - fx_from_int(9));
                pieces.push_back(std::move(separator));
            }
        }
        if (!responsive && column_widths != nullptr) {
            SourcePiece position;
            position.minimum_x = column_x;
            pieces.push_back(std::move(position));
        }
        if (responsive && column < headers.size()) {
            std::string header = block_plain_text(document,
                                                  document.ir.blocks[headers[column]]);
            if (!header.empty()) {
                header += ": ";
                pieces.push_back({std::move(header), InlineStyleStrong,
                                  InlineKind::Text, false, 0, 0, false});
            }
        }
        append_token_pieces(document, document.ir.blocks[cell], pieces);
        first = false;
        if (!responsive && column_widths != nullptr && column < column_widths->size()) {
            column_x += (*column_widths)[column];
        }
        ++column;
    }
    if (!responsive && column_widths != nullptr && !column_widths->empty()) {
        SourcePiece end;
        end.minimum_x = column_x;
        pieces.push_back(std::move(end));
    }
}

class LineBuilder {
public:
    LineBuilder(TextSystem& text,
                MathSystem* math,
                Fx pixel_size,
                Fx available_width,
                int line_height,
                int top_padding,
                bool wrap,
                bool suppress_bottom_gap,
                BlockLayout& output)
        : text_(text),
          math_(math),
          pixel_size_(pixel_size),
          available_width_(std::max<Fx>(fx_from_int(24), available_width)),
          automatic_spacing_(line_height == 0),
          // A manual line height may request a true zero gap; clamp only far
          // enough that consecutive lines can never overlap.
          line_height_(line_height == 0
                           ? automatic_line_height(pixel_size)
                           : std::max(line_height, fx_ceil(pixel_size))),
          cursor_y_(top_padding),
          wrap_(wrap),
          suppress_bottom_gap_(suppress_bottom_gap),
          output_(output) {}

    bool append(std::string_view text,
                std::uint8_t style,
                InlineKind kind,
                std::uint32_t source_offset,
                std::uint32_t source_length,
                bool exact_source_mapping,
                std::uint32_t link_id) {
        if ((kind == InlineKind::InlineMath || kind == InlineKind::DisplayMath) &&
            math_ != nullptr) {
            return append_math(text, kind == InlineKind::DisplayMath,
                               source_offset, source_length, exact_source_mapping,
                               link_id);
        }
        std::size_t offset = 0;
        while (offset < text.size()) {
            if (text[offset] == '\n' || text[offset] == '\r') {
                const std::size_t newline_begin = offset;
                const char current = text[offset++];
                if (current == '\r' && offset < text.size() && text[offset] == '\n') ++offset;
                include_source(
                    source_offset +
                        static_cast<std::uint32_t>(newline_begin),
                    static_cast<std::uint32_t>(offset - newline_begin));
                pending_space_ = {};
                finish_line(true);
                continue;
            }

            std::size_t end = offset;
            const bool whitespace = text[offset] == ' ' || text[offset] == '\t';
            while (end < text.size() && text[end] != '\n' && text[end] != '\r' &&
                   ((text[end] == ' ' || text[end] == '\t') == whitespace)) {
                ++end;
            }
            if (whitespace) {
                if (line_.runs.empty()) {
                    include_source(
                        source_offset + static_cast<std::uint32_t>(offset),
                        static_cast<std::uint32_t>(end - offset));
                }
                if (!line_.runs.empty()) {
                    const std::string_view space = wrap_ ? std::string_view(" ")
                                                         : text.substr(offset, end - offset);
                    if (wrap_) {
                        pending_space_ = {std::string(space), style, kind, false,
                            source_offset + static_cast<std::uint32_t>(offset),
                            static_cast<std::uint32_t>(end - offset),
                            exact_source_mapping && space.size() == end - offset,
                            link_id};
                    } else if (!append_shaped(space, style, kind,
                                              source_offset + static_cast<std::uint32_t>(offset),
                                              static_cast<std::uint32_t>(end - offset),
                                              exact_source_mapping,
                                              link_id) && !space.empty()) {
                        return false;
                    }
                }
            } else if (!append_word(text.substr(offset, end - offset), style, kind,
                                    source_offset + static_cast<std::uint32_t>(offset),
                                    static_cast<std::uint32_t>(end - offset),
                                    exact_source_mapping, link_id)) {
                return false;
            }
            offset = end;
        }
        return true;
    }

    void append_task_checkbox(bool checked) {
        // Match the browser-native task-list control used by GitHub. Keep the
        // checkbox independent of the document font so changing font size or
        // selecting a CJK face cannot stretch its raster.
        constexpr int size = 13;
        LayoutRun run;
        run.x = line_.width;
        run.pixel_size = pixel_size_;
        run.source_kind = InlineKind::Text;
        run.task_checkbox = true;
        run.task_checked = checked;
        run.glyphs.width = fx_from_int(size + 4);
        run.glyphs.ascent = fx_from_int(size);
        run.glyphs.descent = 0;
        line_.width += run.glyphs.width;
        line_ascent_ = std::max(line_ascent_, run.glyphs.ascent);
        line_.runs.push_back(std::move(run));
    }

    void hard_break() {
        pending_space_ = {};
        finish_line(true);
    }

    void advance_to(Fx position) {
        if (position >= 0) line_.width = std::max(line_.width, position);
    }

    void finish() {
        pending_space_ = {};
        if (!line_.runs.empty() || output_.lines.empty()) finish_line(true);
        const int bottom_gap = suppress_bottom_gap_ ? 0 :
                               (output_.kind == BlockKind::Heading ? 8 :
                                (output_.code_background ? 6 : 7));
        output_.height = fx_from_int(cursor_y_ + bottom_gap);
    }

private:
    static int automatic_leading(Fx pixel_size) {
        // CSS-like `normal` leading: roughly one fifth of the em, bounded for
        // the calculator's small raster sizes.
        return std::max(2, std::min(5, (fx_ceil(pixel_size) + 4) / 5));
    }

    static int automatic_line_height(Fx pixel_size) {
        return fx_ceil(pixel_size) + automatic_leading(pixel_size);
    }

    FontRole font_role(std::uint8_t style) const {
        return output_.code_background || (style & InlineStyleCode) != 0
                   ? FontRole::Monospace
                   : FontRole::BodySans;
    }

    bool shape(std::string_view value, std::uint8_t style, GlyphRun& run) {
        const bool italic = (style & InlineStyleEmphasis) != 0;
        const bool bold = output_.kind == BlockKind::Heading ||
                          (style & InlineStyleStrong) != 0;
        const FontStyle font_style =
            bold && italic ? FontStyle::BoldItalic
            : bold         ? FontStyle::Bold
            : italic       ? FontStyle::Italic
                           : FontStyle::Regular;
        return text_.shape(value.data(), value.size(), pixel_size_, run,
                           font_role(style), TextSpacing::Natural,
                           font_style);
    }

    Fx styled_width(const GlyphRun& run, std::uint8_t style) const {
        const bool bold = output_.kind == BlockKind::Heading ||
                          (style & InlineStyleStrong) != 0;
        return run.width +
               (bold && text_.requires_synthetic_bold(run)
                    ? fx_from_int(1) : 0);
    }

    Fx run_advance(const LayoutRun& run) const {
        return run.math != nullptr
                   ? run.math->metrics.width
                   : styled_width(run.glyphs, run.style_flags);
    }

    Fx space_shrink_capacity(const GlyphRun& glyphs) const {
        if (glyphs.glyphs.empty() || glyphs.width <= fx_from_int(1)) return 0;
        return std::min<Fx>(fx_from_int(2), glyphs.width / 3);
    }

    Fx interword_shrink_capacity() const {
        Fx capacity = 0;
        for (const LayoutRun& run : line_.runs) {
            if (run.collapsible_space) {
                capacity += space_shrink_capacity(run.glyphs);
            }
        }
        return capacity;
    }

    bool shrink_interword_spaces(Fx amount) {
        if (amount <= 0) return true;
        if (amount > interword_shrink_capacity()) return false;
        std::size_t remaining_spaces = 0;
        for (const LayoutRun& run : line_.runs) {
            if (run.collapsible_space && space_shrink_capacity(run.glyphs) > 0) {
                ++remaining_spaces;
            }
        }
        Fx remaining = amount;
        for (LayoutRun& run : line_.runs) {
            if (!run.collapsible_space || run.glyphs.glyphs.empty()) continue;
            const Fx capacity = space_shrink_capacity(run.glyphs);
            if (capacity <= 0) continue;
            const Fx fair_share = remaining_spaces == 0
                                      ? remaining
                                      : (remaining + static_cast<Fx>(remaining_spaces) - 1) /
                                            static_cast<Fx>(remaining_spaces);
            const Fx shrink = std::min(capacity, fair_share);
            run.glyphs.glyphs.back().x_advance -= shrink;
            run.glyphs.width -= shrink;
            remaining -= shrink;
            --remaining_spaces;
        }
        Fx cursor = 0;
        for (LayoutRun& run : line_.runs) {
            run.x = cursor;
            // Math runs deliberately carry no GlyphRun; their advance lives
            // in MathLayoutResult. Treating the empty glyph run as zero here
            // repositions following prose on top of the formula whenever
            // bounded inter-word compression is used.
            cursor += run_advance(run);
        }
        line_.width = cursor;
        return remaining <= 0;
    }

    bool commit_pending_space(Fx following_width) {
        if (pending_space_.text.empty()) return true;
        if (line_.runs.empty()) {
            pending_space_ = {};
            return true;
        }
        GlyphRun glyphs;
        if (!shape(" ", pending_space_.style, glyphs)) return false;
        const Fx overflow = line_.width +
                            styled_width(glyphs, pending_space_.style) +
                            following_width -
                            available_width_;
        const Fx total_capacity = interword_shrink_capacity() +
                                  space_shrink_capacity(glyphs);
        if (overflow > total_capacity) {
            pending_space_ = {};
            finish_line(false);
            return true;
        }
        const SourcePiece pending = std::move(pending_space_);
        pending_space_ = {};
        if (!append_shaped(" ", pending.style, pending.kind,
                           pending.source_offset, pending.source_length,
                           pending.exact_source_mapping, pending.link_id,
                           true, true)) {
            return false;
        }
        return overflow <= 0 || shrink_interword_spaces(overflow);
    }

    bool append_shaped(std::string_view value,
                       std::uint8_t style,
                       InlineKind kind,
                       std::uint32_t source_offset,
                       std::uint32_t source_length,
                       bool exact_source_mapping,
                       std::uint32_t link_id,
                       bool collapsible_space = false,
                       bool suppress_wrap = false) {
        GlyphRun glyphs;
        if (!shape(value, style, glyphs)) return false;
        const Fx width = styled_width(glyphs, style);
        if (!suppress_wrap && wrap_ && !line_.runs.empty() &&
            line_.width + width > available_width_) {
            finish_line(false);
            if (value == " ") return true;
        }
        LayoutRun run;
        run.x = line_.width;
        run.pixel_size = pixel_size_;
        run.style_flags = style;
        run.source_kind = kind;
        run.source_offset = source_offset;
        run.source_length = source_length;
        run.link_id = link_id;
        run.exact_source_mapping = exact_source_mapping;
        run.collapsible_space = collapsible_space;
        run.glyphs = std::move(glyphs);
        line_.width += styled_width(run.glyphs, style);
        line_ascent_ = std::max(line_ascent_, run.glyphs.ascent);
        line_descent_ = std::max(line_descent_,
                                 run.glyphs.descent < 0 ? -run.glyphs.descent
                                                       : run.glyphs.descent);
        line_.runs.push_back(std::move(run));
        return true;
    }

    bool append_word(std::string_view word,
                     std::uint8_t style,
                     InlineKind kind,
                     std::uint32_t source_offset,
                     std::uint32_t source_length,
                     bool exact_source_mapping,
                     std::uint32_t link_id) {
        GlyphRun whole;
        if (!shape(word, style, whole)) return false;
        const Fx whole_width = styled_width(whole, style);
        if (wrap_ && !commit_pending_space(whole_width)) return false;
        if (!wrap_ || whole_width <= available_width_) {
            if (!line_.runs.empty() && wrap_ && line_.width + whole_width > available_width_) {
                finish_line(false);
            }
            LayoutRun run;
            run.x = line_.width;
            run.pixel_size = pixel_size_;
            run.style_flags = style;
            run.source_kind = kind;
            run.source_offset = source_offset;
            run.source_length = source_length;
            run.link_id = link_id;
            run.exact_source_mapping = exact_source_mapping;
            run.glyphs = std::move(whole);
            line_.width += styled_width(run.glyphs, style);
            line_ascent_ = std::max(line_ascent_, run.glyphs.ascent);
            line_descent_ = std::max(line_descent_,
                                     run.glyphs.descent < 0 ? -run.glyphs.descent
                                                           : run.glyphs.descent);
            line_.runs.push_back(std::move(run));
            return true;
        }

        if (!line_.runs.empty()) finish_line(false);
        // HarfBuzz has already shaped the complete word/script run. For the
        // common LTR case, wrap that output at cluster boundaries instead of
        // reshaping every successively longer prefix. This preserves
        // ligatures, kerning, combining marks, and contextual positioning
        // across the admitted source run while removing the old quadratic
        // shaping cost for long CJK lines.
        const bool monotone_clusters = std::adjacent_find(
            whole.glyphs.begin(), whole.glyphs.end(),
            [](const PositionedGlyph& left, const PositionedGlyph& right) {
                return left.source_cluster > right.source_cluster;
            }) == whole.glyphs.end();
        if (monotone_clusters && !whole.glyphs.empty()) {
            std::size_t glyph_begin = 0;
            while (glyph_begin < whole.glyphs.size()) {
                std::size_t glyph_end = glyph_begin;
                Fx part_width = 0;
                while (glyph_end < whole.glyphs.size()) {
                    const std::uint32_t cluster =
                        whole.glyphs[glyph_end].source_cluster;
                    std::size_t cluster_end = glyph_end;
                    Fx cluster_width = 0;
                    while (cluster_end < whole.glyphs.size() &&
                           whole.glyphs[cluster_end].source_cluster ==
                               cluster) {
                        cluster_width +=
                            whole.glyphs[cluster_end].x_advance;
                        ++cluster_end;
                    }
                    if (glyph_end != glyph_begin &&
                        part_width + cluster_width > available_width_) {
                        break;
                    }
                    part_width += cluster_width;
                    glyph_end = cluster_end;
                    if (part_width > available_width_) break;
                }
                if (glyph_end == glyph_begin) {
                    glyph_end = glyph_begin + 1;
                    while (glyph_end < whole.glyphs.size() &&
                           whole.glyphs[glyph_end].source_cluster ==
                               whole.glyphs[glyph_begin].source_cluster) {
                        ++glyph_end;
                    }
                }

                const std::size_t source_begin =
                    std::min<std::size_t>(
                        word.size(),
                        whole.glyphs[glyph_begin].source_cluster);
                std::size_t source_end =
                    glyph_end < whole.glyphs.size()
                        ? std::min<std::size_t>(
                              word.size(),
                              whole.glyphs[glyph_end].source_cluster)
                        : word.size();
                const std::size_t safe_end =
                    punctuation_safe_break(word, source_begin, source_end);
                if (safe_end < source_end) {
                    std::size_t adjusted = glyph_begin;
                    while (adjusted < glyph_end &&
                           whole.glyphs[adjusted].source_cluster <
                               safe_end) {
                        const std::uint32_t cluster =
                            whole.glyphs[adjusted].source_cluster;
                        do {
                            ++adjusted;
                        } while (adjusted < glyph_end &&
                                 whole.glyphs[adjusted].source_cluster ==
                                     cluster);
                    }
                    if (adjusted > glyph_begin) {
                        glyph_end = adjusted;
                        source_end = safe_end;
                    }
                }

                GlyphRun part;
                part.ascent = whole.ascent;
                part.descent = whole.descent;
                part.glyphs.reserve(glyph_end - glyph_begin);
                for (std::size_t index = glyph_begin;
                     index < glyph_end; ++index) {
                    part.width += whole.glyphs[index].x_advance;
                    part.glyphs.push_back(std::move(whole.glyphs[index]));
                }
                LayoutRun run;
                run.x = 0;
                run.pixel_size = pixel_size_;
                run.style_flags = style;
                run.source_kind = kind;
                run.source_offset =
                    source_offset +
                    static_cast<std::uint32_t>(source_begin);
                run.source_length =
                    static_cast<std::uint32_t>(source_end - source_begin);
                run.link_id = link_id;
                run.exact_source_mapping = exact_source_mapping;
                run.glyphs = std::move(part);
                line_.width = styled_width(run.glyphs, style);
                line_ascent_ = std::max(line_ascent_,
                                        run.glyphs.ascent);
                line_descent_ = std::max(
                    line_descent_,
                    run.glyphs.descent < 0 ? -run.glyphs.descent
                                           : run.glyphs.descent);
                line_.runs.push_back(std::move(run));
                glyph_begin = glyph_end;
                if (glyph_begin < whole.glyphs.size()) {
                    finish_line(false);
                }
            }
            return true;
        }

        // Rare RTL/non-monotone runs retain the conservative line-by-line
        // reshaping path until bidi paragraph layout supplies visual-order
        // cluster slicing.
        std::size_t begin = 0;
        while (begin < word.size()) {
            std::size_t best = begin;
            std::size_t cursor = begin;
            GlyphRun best_run;
            while (cursor < word.size()) {
                cursor = next_codepoint_end(word, cursor);
                GlyphRun candidate;
                if (!shape(word.substr(begin, cursor - begin), style, candidate)) return false;
                if (styled_width(candidate, style) > available_width_ && best != begin) break;
                best = cursor;
                best_run = std::move(candidate);
                if (styled_width(best_run, style) > available_width_) break;
            }
            if (best == begin) best = next_codepoint_end(word, begin);
            const std::size_t safe = punctuation_safe_break(word, begin, best);
            if (safe != best) {
                best = safe;
                if (!shape(word.substr(begin, best - begin), style, best_run)) return false;
            }
            if (best_run.glyphs.empty() &&
                !shape(word.substr(begin, best - begin), style, best_run)) return false;
            LayoutRun run;
            run.x = 0;
            run.pixel_size = pixel_size_;
            run.style_flags = style;
            run.source_kind = kind;
            run.source_offset = source_offset + static_cast<std::uint32_t>(begin);
            run.source_length = static_cast<std::uint32_t>(best - begin);
            run.link_id = link_id;
            run.exact_source_mapping = exact_source_mapping;
            run.glyphs = std::move(best_run);
            line_.width = styled_width(run.glyphs, style);
            line_ascent_ = std::max(line_ascent_, run.glyphs.ascent);
            line_descent_ = std::max(line_descent_,
                                     run.glyphs.descent < 0 ? -run.glyphs.descent
                                                           : run.glyphs.descent);
            line_.runs.push_back(std::move(run));
            begin = best;
            if (begin < word.size()) finish_line(false);
        }
        return true;
    }

    bool append_math(std::string_view latex,
                     bool display,
                     std::uint32_t source_offset,
                     std::uint32_t source_length,
                     bool exact_source_mapping,
                     std::uint32_t link_id) {
        if (latex.find_first_not_of(" \t\r\n") == std::string_view::npos) {
            return true;
        }
        std::shared_ptr<const MathLayoutResult> math;
        if (!math_->layout(latex,
                           display ? MathStyle::Display : MathStyle::Text,
                           pixel_size_, available_width_, math) ||
            math == nullptr) {
            return false;
        }
        if (!display && !commit_pending_space(math->metrics.width)) return false;
        if (display) pending_space_ = {};
        if ((display || (wrap_ && !line_.runs.empty() &&
                         line_.width + math->metrics.width > available_width_))) {
            finish_line(false);
        }
        LayoutRun run;
        run.math = std::move(math);
        run.pixel_size = pixel_size_;
        run.source_kind = display ? InlineKind::DisplayMath : InlineKind::InlineMath;
        run.display_math = display;
        run.source_offset = source_offset;
        run.source_length = source_length;
        run.link_id = link_id;
        run.exact_source_mapping = exact_source_mapping;
        run.x = display && run.math->metrics.width < available_width_
                    ? (available_width_ - run.math->metrics.width) / 2
                    : line_.width;
        line_.width = std::max(line_.width, run.x + run.math->metrics.width);
        line_ascent_ = std::max(line_ascent_, run.math->metrics.ascent);
        line_descent_ = std::max(line_descent_, run.math->metrics.descent);
        line_.runs.push_back(std::move(run));
        if (display) finish_line(true);
        return true;
    }

    void finish_line(bool allow_empty) {
        pending_space_ = {};
        if (line_.runs.empty() && !allow_empty) return;
        if (line_ascent_ <= 0) line_ascent_ = pixel_size_;
        line_.baseline_y = fx_from_int(cursor_y_) + line_ascent_;
        line_.ascent = line_ascent_;
        line_.descent = line_descent_;
        const int ink_height = fx_ceil(line_ascent_ + line_descent_);
        const int clearance = automatic_spacing_ ? automatic_leading(pixel_size_) : 2;
        const int advance = std::max(line_height_, ink_height + clearance);
        line_.advance = fx_from_int(advance);
        bool have_source = line_source_valid_;
        std::uint32_t source_begin = line_source_begin_;
        std::uint32_t source_end = line_source_end_;
        for (const LayoutRun& run : line_.runs) {
            if (run.source_length == 0) continue;
            const std::uint32_t run_end =
                run.source_offset + run.source_length;
            if (!have_source) {
                source_begin = run.source_offset;
                source_end = run_end;
                have_source = true;
            } else {
                source_begin = std::min(source_begin, run.source_offset);
                source_end = std::max(source_end, run_end);
            }
        }
        if (have_source) {
            line_.source_offset = source_begin;
            line_.source_length = source_end - source_begin;
        }
        output_.maximum_line_width = std::max(output_.maximum_line_width, line_.width);
        output_.lines.push_back(std::move(line_));
        line_ = {};
        cursor_y_ += advance;
        line_ascent_ = 0;
        line_descent_ = 0;
        line_source_valid_ = false;
        line_source_begin_ = 0;
        line_source_end_ = 0;
    }

    void include_source(std::uint32_t offset, std::uint32_t length) {
        const std::uint32_t end = offset + length;
        if (!line_source_valid_) {
            line_source_begin_ = offset;
            line_source_end_ = end;
            line_source_valid_ = true;
        } else {
            line_source_begin_ = std::min(line_source_begin_, offset);
            line_source_end_ = std::max(line_source_end_, end);
        }
    }

    TextSystem& text_;
    MathSystem* math_;
    Fx pixel_size_;
    Fx available_width_;
    bool automatic_spacing_;
    int line_height_;
    int cursor_y_;
    bool wrap_;
    bool suppress_bottom_gap_;
    BlockLayout& output_;
    LayoutLine line_;
    SourcePiece pending_space_;
    Fx line_ascent_ = 0;
    Fx line_descent_ = 0;
    bool line_source_valid_ = false;
    std::uint32_t line_source_begin_ = 0;
    std::uint32_t line_source_end_ = 0;
};

}  // namespace

bool LayoutSignature::operator==(const LayoutSignature& other) const {
    return content_width == other.content_width && body_px == other.body_px &&
           line_height_px == other.line_height_px &&
           font_pack_version == other.font_pack_version &&
           table_mode == other.table_mode && code_wrap == other.code_wrap;
}

bool layout_plain_text_region(std::string_view utf8,
                              std::uint32_t source_offset,
                              TextSystem& text,
                              const LayoutSignature& signature,
                              Fx target_height,
                              BlockLayout& output,
                              std::size_t& consumed_bytes) {
    output = {};
    consumed_bytes = 0;
    if (!text.ready() || signature.content_width < 24 ||
        signature.body_px < 6 || target_height <= 0) {
        return false;
    }

    output.kind = BlockKind::Paragraph;
    const Fx pixel_size = fx_from_int(signature.body_px);
    LineBuilder builder(text, nullptr, pixel_size,
                        fx_from_int(signature.content_width),
                        signature.line_height_px, 0, true, true, output);

    // End ordinary quanta at a whitespace or physical-line boundary. The old
    // fixed 256-byte cuts could split a word, ligature, or script context into
    // two independent HarfBuzz calls. Exceptionally long unbroken lines remain
    // bounded so corrupt/generated TXT cannot monopolize one input frame.
    constexpr std::size_t kFlowQuantumBytes = 512;
    constexpr std::size_t kMaximumContextBytes = 4096;
    while (consumed_bytes < utf8.size()) {
        std::size_t end = std::min(
            utf8.size(), consumed_bytes + kFlowQuantumBytes);
        if (end < utf8.size()) {
            const std::size_t maximum_end = std::min(
                utf8.size(), consumed_bytes + kMaximumContextBytes);
            std::size_t boundary = end;
            while (boundary < maximum_end &&
                   utf8[boundary] != ' ' && utf8[boundary] != '\t' &&
                   utf8[boundary] != '\r' && utf8[boundary] != '\n') {
                ++boundary;
            }
            if (boundary < maximum_end) {
                end = boundary + 1;
                if (utf8[boundary] == '\r' && end < utf8.size() &&
                    utf8[end] == '\n') {
                    ++end;
                }
            } else {
                end = maximum_end;
                while (end < utf8.size() && end > consumed_bytes &&
                       (static_cast<unsigned char>(utf8[end]) & 0xC0U) ==
                           0x80U) {
                    --end;
                }
            }
            if (end == consumed_bytes) {
                end = next_codepoint_end(utf8, consumed_bytes);
            }
        }
        if (end <= consumed_bytes) break;
        const std::string_view part =
            utf8.substr(consumed_bytes, end - consumed_bytes);
        if (!builder.append(
                part, InlineStyleNone, InlineKind::Text,
                source_offset + static_cast<std::uint32_t>(consumed_bytes),
                static_cast<std::uint32_t>(part.size()), true,
                kInvalidToken)) {
            return false;
        }
        consumed_bytes = end;
        if (!output.lines.empty()) {
            const LayoutLine& last = output.lines.back();
            if (last.baseline_y - last.ascent + last.advance >=
                target_height) {
                break;
            }
        }
    }
    builder.finish();
    return true;
}

void VirtualDocumentLayout::clear() {
    document_ = nullptr;
    text_ = nullptr;
    math_ = nullptr;
    units_.clear();
    measured_.clear();
    heights_.clear();
    cache_.clear();
    unit_by_node_.clear();
    measured_count_ = 0;
    use_clock_ = 0;
    preload_from_y_ = -1;
    preload_end_y_ = -1;
    preload_next_unit_ = 0;
    preload_next_line_ = 0;
}

bool VirtualDocumentLayout::initialize(MarkdownDocument& document,
                                       TextSystem& text,
                                       const LayoutSignature& signature,
                                       std::string& error,
                                       MathSystem* math) {
    clear();
    error.clear();
    if (!text.ready()) {
        error = "text system is unavailable";
        return false;
    }
    if (signature.content_width < 32 || signature.body_px < 6 ||
        (signature.line_height_px != 0 &&
         signature.line_height_px < signature.body_px)) {
        error = "invalid layout signature";
        return false;
    }
    document_ = &document;
    text_ = &text;
    math_ = math;
    signature_ = signature;
    collect_units();
    std::vector<Fx> estimates;
    estimates.reserve(units_.size());
    for (const UnitInfo& unit : units_) estimates.push_back(estimate(unit));
    heights_.build(estimates);
    measured_.assign(units_.size(), false);
    return true;
}

bool VirtualDocumentLayout::reconfigure(const LayoutSignature& signature,
                                        std::string& error) {
    if (document_ == nullptr || text_ == nullptr) {
        error = "layout has no document";
        return false;
    }
    if (signature == signature_) {
        error.clear();
        return true;
    }
    MarkdownDocument* document = document_;
    TextSystem* text = text_;
    return initialize(*document, *text, signature, error, math_);
}

void VirtualDocumentLayout::collect_units() {
    collect_units_from(0);
}

void VirtualDocumentLayout::collect_units_from(NodeId first_node) {
    if (document_ == nullptr) return;
    const DocumentIR& ir = document_->ir;
    // Blocks are stored in document pre-order, so an item's ancestors always
    // precede it. Tracking which list items already emitted a unit, and each
    // ordered list's running number, keeps this pass linear; the old code
    // rescanned all previously collected units per list unit, which made
    // list-heavy documents quadratic to open and to reflow.
    std::unordered_set<NodeId> items_with_units;
    std::unordered_map<NodeId, std::uint32_t> next_ordered_number;
    std::unordered_map<NodeId, std::uint32_t> ordered_number_of_item;
    const auto assign_ordered_number = [&](NodeId item) {
        const NodeId list = ir.blocks[item].parent;
        if (list == kInvalidNode || list >= ir.blocks.size() ||
            ir.blocks[list].kind != BlockKind::OrderedList) {
            return;
        }
        auto counter = next_ordered_number
                           .emplace(list, std::max<std::uint32_t>(
                                              1, ir.blocks[list].aux))
                           .first;
        ordered_number_of_item[item] = counter->second++;
    };
    if (first_node != 0) {
        for (const UnitInfo& previous : units_) {
            for (NodeId cursor = previous.node;
                 cursor != kInvalidNode && cursor < ir.blocks.size();
                 cursor = ir.blocks[cursor].parent) {
                if (ir.blocks[cursor].kind == BlockKind::ListItem) {
                    items_with_units.insert(cursor);
                }
            }
        }
        for (NodeId node = 0; node < first_node && node < ir.blocks.size();
             ++node) {
            if (ir.blocks[node].kind == BlockKind::ListItem) {
                assign_ordered_number(node);
            }
        }
    }

    std::vector<NodeId> item_ancestors;
    for (NodeId node = first_node; node < ir.blocks.size(); ++node) {
        const BlockRecord& block = ir.blocks[node];
        // Ordered numbering follows sibling position even for items that
        // never emit a unit, so every ordered item is counted on arrival.
        if (block.kind == BlockKind::ListItem) assign_ordered_number(node);
        bool render = renderable_kind(block.kind);
        if (render && signature_.table_mode == 0 &&
            block.kind == BlockKind::TableRow &&
            table_row_is_header(*document_, block)) {
            render = false;
        }
        if (block.kind == BlockKind::ListItem) {
            render = true;
            for (NodeId child = block.first_child;
                 child != kInvalidNode && child < ir.blocks.size();
                 child = ir.blocks[child].next_sibling) {
                if (renderable_kind(ir.blocks[child].kind)) {
                    render = false;
                    break;
                }
            }
        }
        if (!render) continue;

        UnitInfo info;
        info.node = node;
        unsigned list_depth = 0;
        unsigned quote_depth = 0;
        item_ancestors.clear();
        for (NodeId cursor = node;
             cursor != kInvalidNode && cursor < ir.blocks.size();
             cursor = ir.blocks[cursor].parent) {
            if (ir.blocks[cursor].kind == BlockKind::ListItem) {
                item_ancestors.push_back(cursor);
                ++list_depth;
            }
            if (ir.blocks[cursor].kind == BlockKind::Quote) ++quote_depth;
        }
        info.quote_depth = static_cast<std::uint8_t>(std::min(quote_depth, 255U));
        info.indent_px = static_cast<std::uint16_t>(
            std::min(240U, list_depth * 14U + quote_depth * 8U));

        const NodeId item = item_ancestors.empty() ? kInvalidNode
                                                   : item_ancestors.front();
        if (item != kInvalidNode) {
            if (items_with_units.count(item) == 0) {
                const BlockRecord& item_block = ir.blocks[item];
                const NodeId list = item_block.parent;
                if ((item_block.flags & BlockFlagTask) != 0) {
                    info.task_checkbox = true;
                    info.task_checked =
                        (item_block.flags & BlockFlagChecked) != 0;
                } else if (list != kInvalidNode && list < ir.blocks.size() &&
                           ir.blocks[list].kind == BlockKind::OrderedList) {
                    const auto numbered = ordered_number_of_item.find(item);
                    const std::uint32_t number =
                        numbered != ordered_number_of_item.end()
                            ? numbered->second
                            : ordered_item_number(ir, item);
                    info.prefix = std::to_string(number) + ". ";
                } else {
                    info.prefix = u8"• ";
                }
            }
        }
        unit_by_node_[node] = units_.size();
        units_.push_back(std::move(info));
        // A unit inside nested items marks every enclosing item as started,
        // so a paragraph following a nested sub-list in the same outer item
        // is not treated as that outer item's first line.
        for (NodeId ancestor : item_ancestors) {
            items_with_units.insert(ancestor);
        }
    }
}

Fx VirtualDocumentLayout::estimate(const UnitInfo& unit) const {
    if (document_ == nullptr || unit.node >= document_->ir.blocks.size()) return 0;
    const BlockRecord& block = document_->ir.blocks[unit.node];
    const int estimated_line_height = signature_.line_height_px == 0
                                          ? signature_.body_px + std::max<int>(
                                                2, (signature_.body_px + 4) / 5)
                                          : signature_.line_height_px;
    int pixels = estimated_line_height + 7;
    if ((block.flags & BlockFlagPlainText) != 0) {
        const int physical_lines = std::max<int>(1, block.aux);
        pixels = estimated_line_height * physical_lines;
        if ((block.flags & BlockFlagTextContinuation) == 0) pixels += 2;
        const NodeId next = block.next_sibling;
        const bool continues = next != kInvalidNode &&
                               next < document_->ir.blocks.size() &&
                               (document_->ir.blocks[next].flags &
                                BlockFlagTextContinuation) != 0;
        if (!continues) pixels += 7;
        return fx_from_int(std::max(8, pixels));
    }
    switch (block.kind) {
    case BlockKind::Heading:
        pixels = signature_.body_px + 18;
        break;
    case BlockKind::CodeBlock:
        pixels = estimated_line_height * 3 + 12;
        break;
    case BlockKind::HorizontalRule:
        pixels = 14;
        break;
    case BlockKind::TableRow:
        pixels = estimated_line_height * 2 + 7;
        break;
    default:
        break;
    }
    return fx_from_int(std::max(8, pixels));
}

NodeId VirtualDocumentLayout::unit_node(std::size_t index) const {
    return index < units_.size() ? units_[index].node : kInvalidNode;
}

std::size_t VirtualDocumentLayout::unit_at(Fx document_y) const {
    if (units_.empty()) return 0;
    if (document_y < 0) document_y = 0;
    const std::size_t result = heights_.lower_bound(document_y);
    return std::min(result, units_.size() - 1);
}

ViewAnchor VirtualDocumentLayout::anchor_at(Fx document_y) const {
    ViewAnchor anchor;
    if (document_ == nullptr || units_.empty()) return anchor;
    const std::size_t index = unit_at(document_y);
    anchor.block = units_[index].node;
    anchor.local_y_26_6 = document_y - heights_.prefix_sum(index);
    anchor.source_offset = document_->ir.blocks[anchor.block].source_offset;
    return anchor;
}

std::size_t VirtualDocumentLayout::find_unit(NodeId node,
                                             std::uint32_t source_offset) const {
    const auto exact = unit_by_node_.find(node);
    if (exact != unit_by_node_.end()) return exact->second;
    if (document_ == nullptr || units_.empty()) return 0;
    std::size_t preceding = 0;
    bool have_preceding = false;
    std::size_t following = units_.size() - 1;
    std::uint32_t following_offset = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0; index < units_.size(); ++index) {
        const BlockRecord& block = document_->ir.blocks[units_[index].node];
        const std::uint64_t end = static_cast<std::uint64_t>(block.source_offset) +
                                  block.source_length;
        if (block.source_length != 0 && block.source_offset <= source_offset &&
            source_offset < end) {
            return index;
        }
        if (block.source_offset <= source_offset) {
            preceding = index;
            have_preceding = true;
        } else if (block.source_offset < following_offset) {
            following = index;
            following_offset = block.source_offset;
        }
    }
    return have_preceding ? preceding : following;
}

Fx VirtualDocumentLayout::position_of(const ViewAnchor& anchor) const {
    if (units_.empty()) return 0;
    const std::size_t index = find_unit(anchor.block, anchor.source_offset);
    const Fx local = std::max<Fx>(0, std::min(anchor.local_y_26_6,
                                              std::max<Fx>(0, heights_.value(index) - 1)));
    return heights_.prefix_sum(index) + local;
}

Fx VirtualDocumentLayout::position_for_source(NodeId block,
                                              std::uint32_t source_offset,
                                              std::uint16_t relative_position) {
    if (units_.empty()) return 0;
    const std::size_t index = find_unit(block, source_offset);
    // A TOC or link jump only needs the block's top. Avoid forcing layout in
    // the input handler; the normal visible-window pass measures it after the
    // overlay has closed and the frame can be presented.
    if (relative_position != 0) layout_unit(index);
    const Fx height = heights_.value(index);
    const Fx local = static_cast<Fx>(
        static_cast<std::int64_t>(height) * relative_position / 65535);
    return heights_.prefix_sum(index) + local;
}

bool VirtualDocumentLayout::make_layout(std::size_t index,
                                        BlockLayout& output,
                                        bool force_unwrapped_code) {
    if (document_ == nullptr || text_ == nullptr || index >= units_.size()) return false;
    const UnitInfo& unit = units_[index];
    const BlockRecord& block = document_->ir.blocks[unit.node];
    output = {};
    output.node = unit.node;
    output.kind = block.kind;
    output.indent_px = unit.indent_px;
    output.quote_depth = unit.quote_depth;
    output.heading_level = block.kind == BlockKind::Heading
                               ? static_cast<std::uint8_t>(std::max<std::uint32_t>(1, block.aux))
                               : 0;
    output.code_background = block.kind == BlockKind::CodeBlock;
    output.horizontal_rule = block.kind == BlockKind::HorizontalRule;
    if (output.horizontal_rule) {
        output.height = fx_from_int(14);
        return true;
    }

    const Fx pixel_size = block.kind == BlockKind::Heading
                              ? heading_pixel_size(signature_, block.aux)
                              : fx_from_int(block.kind == BlockKind::CodeBlock
                                                ? std::max<int>(10, signature_.body_px - 1)
                                                : signature_.body_px);
    std::vector<SourcePiece> pieces;
    if (!unit.prefix.empty()) {
        pieces.push_back({unit.prefix, InlineStyleStrong, InlineKind::Text,
                          false, 0, 0, false});
    }
    if (block.kind == BlockKind::TableRow) {
        const bool responsive = signature_.table_mode == 0;
        const std::vector<Fx> widths = responsive
                                          ? std::vector<Fx>()
                                          : table_column_widths(*document_, unit.node,
                                                                *text_, pixel_size);
        append_table_pieces(*document_, unit.node, pieces, responsive,
                            responsive ? nullptr : &widths);
    }
    else append_token_pieces(*document_, block, pieces);
    const bool plain_text = (block.flags & BlockFlagPlainText) != 0;
    const bool continues_from_previous =
        plain_text && (block.flags & BlockFlagTextContinuation) != 0;
    bool continues_to_next = false;
    if (plain_text && index + 1 < units_.size()) {
        const BlockRecord& next = document_->ir.blocks[units_[index + 1].node];
        continues_to_next =
            (next.flags & BlockFlagTextContinuation) != 0 &&
            static_cast<std::uint64_t>(block.source_offset) +
                    block.source_length ==
                next.source_offset;
    }
    const int vertical_padding = output.code_background ? 6 :
                                 (continues_from_previous ? 0 : 2);
    // Side margins describe the real prose bounds. The previous code also
    // subtracted vertical padding from line width without moving the left
    // origin, turning the entire reserve into a hidden right-only gutter.
    // Only decorated blocks need an additional, explicitly symmetric inset.
    output.horizontal_inset_px = output.code_background
                                     ? 6
                                     : (block.kind == BlockKind::TableRow ? 2 : 0);
    const int usable = std::max<int>(
        24, signature_.content_width - unit.indent_px -
                static_cast<int>(output.horizontal_inset_px) * 2);
    const bool grid_table = block.kind == BlockKind::TableRow &&
                            signature_.table_mode != 0;
    const bool wrap = grid_table ? false
                                 : (!output.code_background ||
                                    (!force_unwrapped_code &&
                                     signature_.code_wrap != 0));
    const int line_height = block.kind == BlockKind::Heading
                                ? fx_ceil(pixel_size) + 4
                                : signature_.line_height_px;
    LineBuilder builder(*text_, math_, pixel_size, fx_from_int(usable), line_height,
                        vertical_padding, wrap, continues_to_next, output);
    if (unit.task_checkbox) {
        builder.append_task_checkbox(unit.task_checked);
    }
    for (const SourcePiece& piece : pieces) {
        builder.advance_to(piece.minimum_x);
        if (piece.hard_break) builder.hard_break();
        else if (!builder.append(piece.text, piece.style, piece.kind,
                                 piece.source_offset, piece.source_length,
                                 piece.exact_source_mapping,
                                 piece.link_id)) return false;
    }
    builder.finish();
    return true;
}

void VirtualDocumentLayout::evict_if_needed(std::size_t protected_index) {
    while (cache_.size() > kMaximumCachedBlocks) {
        auto victim = cache_.end();
        for (auto iterator = cache_.begin(); iterator != cache_.end(); ++iterator) {
            if (iterator->first == protected_index) continue;
            if (victim == cache_.end() ||
                iterator->second.last_used < victim->second.last_used) {
                victim = iterator;
            }
        }
        if (victim == cache_.end()) break;
        cache_.erase(victim);
    }
}

const BlockLayout* VirtualDocumentLayout::layout_unit(std::size_t index) {
    if (index >= units_.size()) return nullptr;
    auto found = cache_.find(index);
    if (found != cache_.end()) {
        found->second.last_used = ++use_clock_;
        return &found->second.layout;
    }

    CacheEntry entry;
    if (!make_layout(index, entry.layout)) return nullptr;
    entry.last_used = ++use_clock_;
    const Fx measured_height = std::max<Fx>(fx_from_int(1), entry.layout.height);
    if (!measured_[index]) {
        measured_[index] = true;
        ++measured_count_;
    }
    heights_.update(index, measured_height);
    if (document_ != nullptr) {
        document_->ir.blocks[units_[index].node].measured_height_26_6 = measured_height;
    }
    auto inserted = cache_.emplace(index, std::move(entry)).first;
    evict_if_needed(index);
    return &inserted->second.layout;
}

bool VirtualDocumentLayout::layout_unwrapped_code_unit(std::size_t index,
                                                       BlockLayout& output) {
    if (document_ == nullptr || index >= units_.size() ||
        units_[index].node >= document_->ir.blocks.size() ||
        document_->ir.blocks[units_[index].node].kind != BlockKind::CodeBlock) {
        return false;
    }
    // This deliberately bypasses the cache and Fenwick height update. Focus is
    // a temporary horizontal inspection layer; it must not reflow surrounding
    // document content or disturb the user's vertical reading position.
    return make_layout(index, output, true);
}

std::vector<VisibleBlock> VirtualDocumentLayout::layout_window(
    Fx top,
    Fx viewport_height,
    unsigned overscan_viewports) {
    std::vector<VisibleBlock> result;
    if (units_.empty() || viewport_height <= 0) return result;
    const Fx overscan = static_cast<Fx>(
        std::min<std::int64_t>(std::numeric_limits<Fx>::max(),
                               static_cast<std::int64_t>(viewport_height) *
                                   overscan_viewports));
    const Fx begin_y = std::max<Fx>(0, top - overscan);
    const Fx end_y = std::min<Fx>(heights_.total(), top + viewport_height + overscan);
    std::size_t index = unit_at(begin_y);
    Fx y = heights_.prefix_sum(index);
    while (index < units_.size() && y < end_y) {
        const BlockLayout* layout = layout_unit(index);
        result.push_back({index, units_[index].node, y, layout});
        y += heights_.value(index);
        ++index;
    }
    return result;
}

bool VirtualDocumentLayout::preload_next(Fx from_y, Fx distance) {
    if (units_.empty() || distance <= 0) return false;
    from_y = std::max<Fx>(0, from_y);
    const Fx requested_end = static_cast<Fx>(std::min<std::int64_t>(
        std::numeric_limits<Fx>::max(),
        static_cast<std::int64_t>(from_y) + distance));
    if (preload_from_y_ != from_y || preload_end_y_ != requested_end) {
        preload_from_y_ = from_y;
        preload_end_y_ = requested_end;
        preload_next_unit_ = unit_at(from_y);
        preload_next_line_ = 0;
    }

    if (preload_next_unit_ >= units_.size() ||
        unit_top(preload_next_unit_) >= preload_end_y_) {
        return false;
    }

    // One layout unit is the work quantum. A unit can contain an arbitrarily
    // long paragraph, so doing more than one here would make an otherwise idle
    // prefetch burst visible as touch/input latency on calculator hardware.
    layout_unit(preload_next_unit_);
    ++preload_next_unit_;
    return true;
}

const LayoutLine* VirtualDocumentLayout::preload_next_line(Fx from_y,
                                                            Fx distance) {
    if (units_.empty() || distance <= 0) return nullptr;
    from_y = std::max<Fx>(0, from_y);
    const Fx requested_end = static_cast<Fx>(std::min<std::int64_t>(
        std::numeric_limits<Fx>::max(),
        static_cast<std::int64_t>(from_y) + distance));
    if (preload_from_y_ != from_y || preload_end_y_ != requested_end) {
        preload_from_y_ = from_y;
        preload_end_y_ = requested_end;
        preload_next_unit_ = unit_at(from_y);
        preload_next_line_ = 0;
    }

    while (preload_next_unit_ < units_.size()) {
        if (unit_top(preload_next_unit_) >= preload_end_y_) return nullptr;
        const BlockLayout* block = layout_unit(preload_next_unit_);
        if (block == nullptr) {
            ++preload_next_unit_;
            preload_next_line_ = 0;
            continue;
        }
        const Fx block_top = unit_top(preload_next_unit_);
        while (preload_next_line_ < block->lines.size()) {
            const LayoutLine& line = block->lines[preload_next_line_++];
            const Fx descent = line.descent < 0 ? -line.descent : line.descent;
            const Fx line_top = block_top + line.baseline_y - line.ascent;
            const Fx line_bottom = block_top + line.baseline_y + descent;
            if (line_bottom <= from_y) continue;
            if (line_top >= preload_end_y_) return nullptr;
            return &line;
        }
        ++preload_next_unit_;
        preload_next_line_ = 0;
    }
    return nullptr;
}

}  // namespace nmarkdown
