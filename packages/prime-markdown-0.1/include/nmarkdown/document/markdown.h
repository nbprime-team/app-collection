#ifndef NMARKDOWN_DOCUMENT_MARKDOWN_H
#define NMARKDOWN_DOCUMENT_MARKDOWN_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nmarkdown/document/document_ir.h"
#include "nmarkdown/document/utf8.h"
namespace nmarkdown {

struct MarkdownDocument {
    std::string source;
    // Segmented source remains available for parser/layout fixtures. Runtime
    // TXT uses PlainTextLayout and never populates this Markdown document.
    std::deque<std::string> source_segments;
    std::deque<std::uint32_t> source_segment_offsets;
    DocumentIR ir;
    Utf8ValidationResult utf8;

    void clear();
    std::string_view text(TextRef ref) const;
    std::size_t source_size() const;
    std::size_t source_chunk_count() const;
    std::string_view source_chunk(std::size_t index) const;
    std::uint32_t source_chunk_offset(std::size_t index) const;
};

unsigned markdown_parser_flags();

bool parse_markdown(const std::uint8_t* bytes,
                    std::size_t size,
                    MarkdownDocument& document,
                    std::string& error);

// Build the same layout-facing document model without interpreting Markdown
// punctuation. Physical TXT line endings become hard breaks, and very long
// files are split into bounded layout units for incremental rendering.
bool parse_plain_text(const std::uint8_t* utf8_bytes,
                      std::size_t size,
                      MarkdownDocument& document,
                      std::string& error);

// Move an already-decoded UTF-8 buffer into the document so legacy TXT does
// not require a second maximum-sized allocation during loading.
bool parse_plain_text(std::string&& utf8_source,
                      MarkdownDocument& document,
                      std::string& error);

// Retain an already validated UTF-8 TXT stream as bounded allocations. Each
// segment must end on a UTF-8 codepoint boundary; the parser may split text
// tokens at segment edges without inserting a visual or logical line break.
bool parse_plain_text_segments(std::deque<std::string>&& utf8_segments,
                               const Utf8ValidationResult& validation,
                               MarkdownDocument& document,
                               std::string& error);

}  // namespace nmarkdown

#endif
