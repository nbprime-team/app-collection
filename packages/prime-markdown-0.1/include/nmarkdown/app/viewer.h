#ifndef NMARKDOWN_APP_VIEWER_H
#define NMARKDOWN_APP_VIEWER_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nmarkdown/document/markdown.h"
#include "nmarkdown/document/search.h"
#include "nmarkdown/document/state.h"
#include "nmarkdown/layout/block_layout.h"
#include "nmarkdown/layout/plain_text_layout.h"
#include "nmarkdown/math/math_system.h"
#include "nmarkdown/platform/platform.h"
#include "nmarkdown/render/surface565.h"
#include "nmarkdown/text/font_catalog.h"
#include "nmarkdown/text/text_shaper.h"
#include "nmarkdown/text/text_system.h"

namespace nmarkdown {

struct ReaderPerformanceMetrics {
    std::uint64_t document_load_parse_ms = 0;
    std::uint64_t first_visible_render_ms = 0;
    std::uint64_t last_visible_render_ms = 0;
    std::uint64_t peak_visible_render_ms = 0;
    std::uint64_t last_present_ms = 0;
    std::uint64_t peak_present_ms = 0;
};

class Viewer {
public:
    Viewer();

    void set_document(const DocumentProbe& document);
    bool set_markdown_document(std::unique_ptr<MarkdownDocument> document,
                               const DocumentProbe& probe,
                               std::string& error);
    bool set_plain_text_document(
        std::shared_ptr<RandomAccessData> source,
        std::uint32_t source_offset,
        std::uint32_t source_size,
        const Utf8ValidationResult& sampled_validation,
        const DocumentProbe& probe,
        std::string& error);
    void set_document_title(std::string title);
    void set_document_error(std::string message = {});
    void set_reading_mode(ReadingMode mode);
    bool apply_reader_state(const ReaderState& state,
                            std::uint64_t document_identity);
    ReaderState reader_state(std::uint64_t document_identity) const;
    bool handle_event(const InputEvent& event);
    bool perform_incremental_work(const Clock& clock,
                                  std::uint64_t deadline_ms);
    void render(const Surface565& surface);
    bool take_document_link_request(std::string& target);
    bool take_document_browser_request();
    bool take_state_save_request();
    bool take_document_open_request(std::string& path);
    void show_document_browser(const std::vector<std::string>& paths,
                               bool truncated = false);
    bool take_font_menu_request();
    bool take_font_assignments(
        std::array<std::string, kExternalFontRoleCount>& paths);
    void show_font_manager(
        const std::vector<FontFaceCatalogEntry>& fonts,
        const std::array<std::string, kExternalFontRoleCount>& active_paths,
        bool truncated = false);
    bool set_font_registry(
        FontRegistryState registry,
        const std::array<std::string, kExternalFontRoleCount>& labels,
        std::string& error);
    FontRegistryState font_registry_state() const {
        return text_.font_registry();
    }
    std::size_t external_font_count() const {
        return text_.external_font_count();
    }
    FontFaceId external_font_id(FontRole role) const {
        return text_.external_font_id(role);
    }
    bool navigate_to_anchor(std::string_view fragment);
    void show_message(std::string title, std::string message);
    // Message variant asking the user to assign a CJK font: confirming with
    // Enter opens the font manager, Esc continues without one.
    void show_cjk_font_prompt();
    void reshape_message_dialog_runs();
    void show_loading_feedback(std::string title,
                               std::string detail,
                               int progress_percent = -1);
    void clear_loading_feedback();
    bool set_font(FontRole role,
                  std::vector<std::uint8_t> font,
                  std::string label,
                  std::string& error);
    bool set_fonts(std::vector<ExternalFontUpdate> fonts,
                   const std::vector<std::string>& labels,
                   std::string& error);

    bool quit_requested() const { return quit_requested_; }
    void cancel_quit_request() { quit_requested_ = false; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    int scroll_y() const { return scroll_y_; }
    int pan_x() const { return pan_x_; }
    int max_scroll_y() const;
    int current_page() const;
    int total_pages() const;
    bool document_loaded() const { return document_loaded_; }
    bool has_markdown_document() const { return markdown_document_ != nullptr; }
    bool has_plain_text_document() const {
        return plain_text_layout_.loaded();
    }
    bool jump_to_percentage(unsigned percentage);
    bool text_ready() const { return text_ready_; }
    bool dark_theme() const { return dark_theme_; }
    bool high_contrast() const { return high_contrast_; }
    RenderSharpness render_sharpness() const { return render_sharpness_; }
    bool loading_feedback_visible() const { return loading_feedback_visible_; }
    std::uint64_t retained_frame_fast_path_count() const {
        return retained_frame_fast_path_count_;
    }
    bool document_browser_truncated() const {
        return document_browser_truncated_;
    }
    bool font_browser_truncated() const { return font_browser_truncated_; }
    int body_pixel_size() const { return body_pixel_size_; }
    int line_gap_px() const { return line_gap_px_; }
    int side_margin_px() const { return side_margin_px_; }
    ReadingMode reading_mode() const { return reading_mode_; }
    bool natural_scrolling() const { return natural_scrolling_; }
    bool natural_swiping() const { return natural_swiping_; }
    // User switch for promoting external font payloads into RAM. Off keeps
    // every external font streaming regardless of document size or content.
    bool resident_font_preload() const { return resident_font_preload_; }
    void set_resident_font_preload(bool enabled);
    bool take_font_preload_save_request();
    std::string_view search_query() const { return search_query_; }
    std::size_t search_result_count() const { return search_results_.size(); }
    bool has_active_search_match() const { return has_active_search_match_; }
    GlyphCacheStats glyph_cache_stats() const { return text_.cache_stats(); }
    FormulaCacheStats formula_cache_stats() const { return math_.cache_stats(); }
    std::size_t external_font_bytes() const {
        return text_.external_font_bytes();
    }
    std::size_t external_font_bytes(FontRole role) const {
        return text_.external_font_bytes(role);
    }
    ReaderPerformanceMetrics performance_metrics() const { return performance_metrics_; }
    void set_performance_metrics(const ReaderPerformanceMetrics& metrics) {
        performance_metrics_ = metrics;
    }

private:
    void clamp_view();
    void rebuild_text_runs();
    bool rebuild_chrome_title(int maximum_width);
    void rebuild_toc_runs();
    void update_search();
    void rebuild_search_runs();
    void rebuild_bookmark_runs();
    void rebuild_settings_runs();
    void rebuild_jump_runs();
    void rebuild_diagnostics_runs();
    void rebuild_document_browser_runs();
    void rebuild_font_browser_runs();
    void set_dark_theme(bool dark);
    void show_content_jump();
    void begin_settings_session();
    void commit_settings_session();
    void activate_search_result(std::size_t index);
    bool activate_link(std::uint32_t link_id);
    bool activate_current_link();
    bool current_block_is_wide(NodeId& node, int& maximum_pan);
    void enter_wide_focus(NodeId node, int maximum_pan);
    void exit_wide_focus();
    bool consume_wide_pan(int pan_delta);
    int block_stride() const;
    int content_width() const;
    int reading_progress_width() const;
    bool current_markdown_view_is_text_only();
    int previous_line_scroll_y();
    int next_line_scroll_y();
    void move_markdown_line(int direction);
    int aligned_scroll_y_near(int nominal, bool forward);
    int next_page_scroll_y(int direction);
    void move_page(int direction);
    void render_document(const Surface565& surface, Rect viewport);
    void render_markdown_document(const Surface565& surface, Rect viewport);
    void render_plain_text_document(const Surface565& surface, Rect viewport);
    void render_overlay(const Surface565& surface, bool apply_scrim = true);
    void render_loading_feedback(const Surface565& surface,
                                 bool apply_scrim = true);
    void invalidate_retained_base_frame();
    bool capture_retained_base_frame(const Surface565& surface);
    bool restore_retained_base_frame(const Surface565& surface);
    void prepare_plain_text_cache(PlainTextLayout& layout);
    LayoutSignature layout_signature() const;

    DocumentProbe document_{};
    int document_height_ = 1800;
    int scroll_y_ = 0;
    int pan_x_ = 0;
    bool document_loaded_ = false;
    bool document_error_ = false;
    std::string document_error_message_;
    bool overlay_open_ = false;
    bool toc_overlay_ = false;
    bool search_overlay_ = false;
    bool jump_overlay_ = false;
    bool settings_overlay_ = false;
    struct SettingsSnapshot {
        bool dark_theme = false;
        bool high_contrast = false;
        int body_pixel_size = 15;
        int line_gap_px = -1;
        int side_margin_px = 5;
        bool code_wrap = true;
        std::uint8_t table_mode = 0;
        ReadingMode reading_mode = ReadingMode::VerticalScroll;
        bool natural_scrolling = true;
        bool natural_swiping = true;
        bool resident_font_preload = true;
        RenderSharpness render_sharpness = kDefaultRenderSharpness;
        int scroll_y = 0;
        int max_scroll_y = 0;
        std::uint32_t plain_text_offset = 0;
        ViewAnchor anchor;
    };
    SettingsSnapshot settings_snapshot_;
    bool settings_session_active_ = false;
    bool settings_overlay_repaint_only_ = false;
    std::uint16_t* last_render_surface_ = nullptr;
    bool full_frame_available_ = false;
    // One clean 320x240 RGB565 frame (153,600 bytes). Modal surfaces are
    // destructive, so retaining their unobscured base lets navigation and
    // dismissal avoid another document layout/compositing pass.
    std::vector<std::uint16_t> retained_base_frame_;
    bool retained_base_frame_valid_ = false;
    bool display_surface_is_base_frame_ = false;
    std::uint64_t retained_frame_fast_path_count_ = 0;
    bool diagnostics_overlay_ = false;
    bool document_browser_overlay_ = false;
    bool font_browser_overlay_ = false;
    bool link_overlay_ = false;
    bool bookmark_tab_ = false;
    bool dark_theme_ = false;
    bool high_contrast_ = false;
    bool quit_requested_ = false;
    bool dirty_ = true;
    int body_pixel_size_ = 15;
    // Negative selects automatic content-aware leading; 0-10 is a manual
    // gap in pixels, including a true zero.
    int line_gap_px_ = -1;
    int side_margin_px_ = 5;
    bool code_wrap_ = true;
    std::uint8_t table_mode_ = 0;
    ReadingMode reading_mode_ = ReadingMode::VerticalScroll;
    bool natural_scrolling_ = true;
    bool natural_swiping_ = true;
    bool resident_font_preload_ = true;
    bool pending_font_preload_save_request_ = false;
    // The filename bar overlays the top of the full-height document view:
    // upward navigation reveals it, reading forward hides it.
    bool chrome_visible_ = true;
    RenderSharpness render_sharpness_ = kDefaultRenderSharpness;
    struct ScreenStepPosition {
        int from = 0;
        int to = 0;
    };
    std::vector<ScreenStepPosition> screen_step_history_;
    bool screen_step_event_ = false;
    std::vector<ScreenStepPosition> line_step_history_;
    bool line_step_event_ = false;
    TextSystem text_;
    MathSystem math_;
    bool text_ready_ = false;
    std::unique_ptr<MarkdownDocument> markdown_document_;
    VirtualDocumentLayout markdown_layout_;
    PlainTextLayout plain_text_layout_;
    GlyphRun chrome_title_;
    int chrome_title_max_width_ = -1;
    bool pending_final_page_restore_ = false;
    const std::string default_document_title_ = "nMarkdown";
    std::string document_title_;
    GlyphRun toc_title_;
    GlyphRun search_title_;
    GlyphRun search_query_run_;
    GlyphRun search_status_run_;
    GlyphRun jump_title_;
    GlyphRun jump_query_run_;
    GlyphRun jump_hint_run_;
    GlyphRun bookmark_title_;
    GlyphRun bookmark_hint_run_;
    GlyphRun settings_title_;
    GlyphRun diagnostics_title_;
    GlyphRun document_browser_title_;
    GlyphRun font_browser_title_;
    GlyphRun link_title_;
    std::vector<GlyphRun> link_target_runs_;
    GlyphRun link_hint_run_;
    GlyphRun loading_title_run_;
    GlyphRun loading_detail_run_;
    GlyphRun document_error_title_run_;
    std::vector<GlyphRun> document_error_message_runs_;
    GlyphRun document_error_hint_run_;
    GlyphRun empty_document_run_;
    GlyphRun empty_document_hint_run_;
    GlyphRun bookmark_empty_run_;
    GlyphRun toc_empty_run_;
    std::vector<GlyphRun> sample_runs_;
    std::vector<GlyphRun> overlay_runs_;
    std::vector<GlyphRun> toc_runs_;
    std::vector<GlyphRun> search_result_runs_;
    std::vector<GlyphRun> bookmark_runs_;
    std::vector<GlyphRun> settings_runs_;
    std::vector<GlyphRun> diagnostics_runs_;
    std::vector<GlyphRun> document_browser_runs_;
    std::vector<std::string> document_browser_paths_;
    bool document_browser_truncated_ = false;
    // File names are shaped only for the six visible rows. Keeping every
    // discovered filename as a GlyphRun made opening the picker costly on the
    // calculator, especially while a multi-megabyte CJK face was resident.
    std::vector<std::string> font_browser_labels_;
    std::vector<FontFaceCatalogEntry> font_file_catalog_;
    bool font_browser_truncated_ = false;
    std::vector<std::uint32_t> bookmarks_;
    std::size_t toc_selected_ = 0;
    std::size_t bookmark_selected_ = 0;
    std::size_t settings_selected_ = 0;
    std::size_t document_browser_selected_ = 0;
    std::size_t font_browser_selected_ = 0;
    std::string search_query_;
    std::string jump_query_;
    std::vector<SearchMatch> search_results_;
    std::size_t search_selected_ = 0;
    SearchMode search_mode_ = SearchMode::AsciiCaseInsensitive;
    bool has_active_search_match_ = false;
    SearchMatch active_search_match_;
    std::string link_dialog_title_;
    std::string link_dialog_target_;
    bool loading_feedback_visible_ = false;
    bool loading_feedback_painted_ = false;
    std::uint8_t loading_feedback_phase_ = 0;
    int loading_feedback_progress_ = -1;
    bool link_choice_mode_ = false;
    std::vector<std::uint32_t> link_choice_ids_;
    std::vector<GlyphRun> link_choice_runs_;
    std::size_t link_choice_selected_ = 0;
    std::string pending_document_link_;
    bool pending_document_browser_request_ = false;
    bool pending_state_save_request_ = false;
    std::string pending_document_open_;
    bool font_detail_open_ = false;
    std::size_t font_detail_index_ = 0;
    std::array<std::string, kExternalFontRoleCount> active_font_paths_{};
    std::array<std::string, kExternalFontRoleCount> pending_font_paths_{};
    std::array<std::string, kExternalFontRoleCount> active_font_labels_{{
        "ASCII UI", "Outline slant", "Built-in DejaVu Mono", "None",
        "Synthetic bold", "Synthetic bold italic", "Outline slant"}};
    bool pending_font_menu_request_ = false;
    bool message_confirm_opens_font_menu_ = false;
    bool pending_font_assignments_available_ = false;
    bool cjk_font_hint_shown_ = false;
    std::uint32_t font_pack_signature_ = 1;
    bool wide_focus_ = false;
    NodeId focused_node_ = kInvalidNode;
    int focused_maximum_pan_ = 0;
    // Wrapped code remains part of the normal document flow. While focused,
    // this uncached alternate layout exposes its original lines for pan
    // without reflowing the surrounding document.
    BlockLayout focused_code_layout_;
    bool focused_code_layout_valid_ = false;
    int focused_code_top_y_ = 0;
    int focused_return_scroll_y_ = 0;
    bool focused_scroll_restore_valid_ = false;
    ReaderPerformanceMetrics performance_metrics_{};
};

}  // namespace nmarkdown

#endif
