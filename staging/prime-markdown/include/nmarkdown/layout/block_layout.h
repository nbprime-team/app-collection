#ifndef NMARKDOWN_LAYOUT_BLOCK_LAYOUT_H
#define NMARKDOWN_LAYOUT_BLOCK_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nmarkdown/document/markdown.h"
#include "nmarkdown/layout/fenwick.h"
#include "nmarkdown/text/text_shaper.h"

namespace nmarkdown {

class TextSystem;
class MathSystem;
struct MathLayoutResult;

struct LayoutSignature {
    std::uint16_t content_width = 304;
    std::uint16_t body_px = 15;
    // Zero selects content-aware automatic leading. Non-zero values retain a
    // fixed minimum line height for the manual spacing override.
    std::uint16_t line_height_px = 0;
    std::uint32_t font_pack_version = 1;
    std::uint8_t table_mode = 0;
    std::uint8_t code_wrap = 1;

    bool operator==(const LayoutSignature& other) const;
    bool operator!=(const LayoutSignature& other) const { return !(*this == other); }
};

struct LayoutRun {
    GlyphRun glyphs;
    Fx x = 0;
    Fx pixel_size = 0;
    std::uint8_t style_flags = InlineStyleNone;
    InlineKind source_kind = InlineKind::Text;
    std::shared_ptr<const MathLayoutResult> math;
    bool display_math = false;
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
    std::uint32_t link_id = kInvalidToken;
    bool exact_source_mapping = false;
    bool collapsible_space = false;
    bool task_checkbox = false;
    bool task_checked = false;
};

struct LayoutLine {
    std::vector<LayoutRun> runs;
    Fx baseline_y = 0;
    Fx width = 0;
    Fx ascent = 0;
    Fx descent = 0;
    Fx advance = 0;
    // Exact UTF-8 source span covered by this shaped line. Markdown does not
    // require these aggregate fields, but the streaming TXT layout uses them
    // to move between wrapped lines without building a document-wide index.
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;
};

struct BlockLayout {
    NodeId node = kInvalidNode;
    BlockKind kind = BlockKind::Paragraph;
    std::vector<LayoutLine> lines;
    Fx height = 0;
    Fx maximum_line_width = 0;
    std::uint16_t indent_px = 0;
    // Decorated blocks keep their own symmetric inner padding inside the
    // document's configured outer side margins. Ordinary prose uses zero.
    std::uint8_t horizontal_inset_px = 0;
    std::uint8_t quote_depth = 0;
    std::uint8_t heading_level = 0;
    bool code_background = false;
    bool horizontal_rule = false;
};

struct ViewAnchor {
    NodeId block = kInvalidNode;
    Fx local_y_26_6 = 0;
    std::uint32_t source_offset = 0;
};

struct VisibleBlock {
    std::size_t unit_index = 0;
    NodeId node = kInvalidNode;
    Fx document_y = 0;
    const BlockLayout* layout = nullptr;
};

// Shape a bounded UTF-8 TXT region directly into wrapped lines. This bypasses
// DocumentIR entirely and stops after enough vertical content has been
// produced for a small screen cache. `consumed_bytes` is the amount admitted
// to the flow builder; callers should use the line source spans, rather than
// this coarse boundary, when choosing the next page start.
bool layout_plain_text_region(std::string_view utf8,
                              std::uint32_t source_offset,
                              TextSystem& text,
                              const LayoutSignature& signature,
                              Fx target_height,
                              BlockLayout& output,
                              std::size_t& consumed_bytes);

class VirtualDocumentLayout {
public:
    bool initialize(MarkdownDocument& document,
                    TextSystem& text,
                    const LayoutSignature& signature,
                    std::string& error,
                    MathSystem* math = nullptr);
    void clear();
    bool reconfigure(const LayoutSignature& signature, std::string& error);

    std::size_t unit_count() const { return units_.size(); }
    std::size_t measured_count() const { return measured_count_; }
    std::size_t cache_size() const { return cache_.size(); }
    Fx total_height() const { return heights_.total(); }
    Fx unit_top(std::size_t index) const { return heights_.prefix_sum(index); }
    Fx unit_height(std::size_t index) const { return heights_.value(index); }
    NodeId unit_node(std::size_t index) const;
    const LayoutSignature& signature() const { return signature_; }

    std::size_t unit_at(Fx document_y) const;
    ViewAnchor anchor_at(Fx document_y) const;
    Fx position_of(const ViewAnchor& anchor) const;
    Fx position_for_source(NodeId block,
                           std::uint32_t source_offset,
                           std::uint16_t relative_position);

    const BlockLayout* layout_unit(std::size_t index);
    // Build an uncached, unwrapped view of a code unit without changing the
    // document's wrapping mode or measured vertical geometry. The viewer uses
    // this transient canvas while a wrapped code block is focused for pan.
    bool layout_unwrapped_code_unit(std::size_t index, BlockLayout& output);
    std::vector<VisibleBlock> layout_window(Fx top,
                                            Fx viewport_height,
                                            unsigned overscan_viewports = 2);
    // Lay out at most one block after the visible viewport. Repeated idle-loop
    // calls warm a bounded forward window without making a page turn pay for
    // multiple off-screen viewports synchronously.
    bool preload_next(Fx from_y, Fx distance);
    // Return one future shaped line per call. The viewer uses it to warm glyph
    // rasters incrementally without retaining page-sized framebuffers.
    const LayoutLine* preload_next_line(Fx from_y, Fx distance);

private:
    struct UnitInfo {
        NodeId node = kInvalidNode;
        std::uint16_t indent_px = 0;
        std::uint8_t quote_depth = 0;
        std::string prefix;
        bool task_checkbox = false;
        bool task_checked = false;
    };

    struct CacheEntry {
        BlockLayout layout;
        std::uint64_t last_used = 0;
    };

    void collect_units();
    void collect_units_from(NodeId first_node);
    Fx estimate(const UnitInfo& unit) const;
    bool make_layout(std::size_t index,
                     BlockLayout& output,
                     bool force_unwrapped_code = false);
    void evict_if_needed(std::size_t protected_index);
    std::size_t find_unit(NodeId node, std::uint32_t source_offset) const;

    MarkdownDocument* document_ = nullptr;
    TextSystem* text_ = nullptr;
    MathSystem* math_ = nullptr;
    LayoutSignature signature_{};
    std::deque<UnitInfo> units_;
    std::vector<bool> measured_;
    FenwickHeights heights_;
    std::unordered_map<std::size_t, CacheEntry> cache_;
    std::unordered_map<NodeId, std::size_t> unit_by_node_;
    std::size_t measured_count_ = 0;
    std::uint64_t use_clock_ = 0;
    Fx preload_from_y_ = -1;
    Fx preload_end_y_ = -1;
    std::size_t preload_next_unit_ = 0;
    std::size_t preload_next_line_ = 0;
};

}  // namespace nmarkdown

#endif
