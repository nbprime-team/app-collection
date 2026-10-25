#ifndef NMARKDOWN_DOCUMENT_TEXT_ENCODING_H
#define NMARKDOWN_DOCUMENT_TEXT_ENCODING_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace nmarkdown {

// Encodings accepted for plain-text documents. Markdown remains UTF-8 and is
// intentionally not routed through legacy auto-detection.
enum class TextEncoding : std::uint8_t {
    Utf8,
    Gbk,
    ShiftJis,
    EucJp,
    Iso2022Jp,
};

struct TextDecodeInfo {
    TextEncoding encoding = TextEncoding::Utf8;
    std::size_t replacement_count = 0;
    bool had_bom = false;
};

const char* text_encoding_name(TextEncoding encoding);

// Decode a TXT byte stream using BOM/signature checks plus deterministic
// scoring between GBK, Shift-JIS, and EUC-JP. ISO-2022-JP escape sequences are
// detected before the multibyte candidates. The output is always valid UTF-8
// and never grows beyond maximum_output_bytes.
bool decode_text_auto(const std::uint8_t* bytes,
                      std::size_t size,
                      std::size_t maximum_output_bytes,
                      std::string& output,
                      TextDecodeInfo& info,
                      std::string& error);

// Move-aware variant for file readers that already own the complete byte
// stream. Valid UTF-8 is transferred into output without allocating a second
// maximum-sized buffer; legacy encodings retain the normal detector path.
bool decode_text_auto(std::string&& bytes,
                      std::size_t maximum_output_bytes,
                      std::string& output,
                      TextDecodeInfo& info,
                      std::string& error);

// Deterministic entry point used by tests and callers that already know the
// source encoding. Invalid legacy byte sequences become U+FFFD; invalid UTF-8
// is rejected so a UTF-8 BOM can never silently fall through to another codec.
bool decode_text_as(const std::uint8_t* bytes,
                    std::size_t size,
                    TextEncoding encoding,
                    std::size_t maximum_output_bytes,
                    std::string& output,
                    TextDecodeInfo& info,
                    std::string& error);

}  // namespace nmarkdown

#endif
