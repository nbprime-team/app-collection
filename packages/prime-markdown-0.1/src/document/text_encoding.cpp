#include "nmarkdown/document/text_encoding.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "nmarkdown/document/utf8.h"

namespace nmarkdown {
namespace {

#include "legacy_gbk_table.inc"
#include "legacy_shift_jis_table.inc"
#include "legacy_jis0212_table.inc"

constexpr std::uint32_t kReplacement = kReplacementCodepoint;

struct Candidate {
    TextEncoding encoding = TextEncoding::Utf8;
    std::string output;
    std::size_t replacements = 0;
    std::size_t non_ascii = 0;
    std::size_t mapped_pairs = 0;
    std::size_t han = 0;
    std::size_t kana = 0;
    std::size_t halfwidth_kana = 0;
    std::size_t japanese_punctuation = 0;
    std::size_t recognized_escapes = 0;
    std::size_t syntax_errors = 0;
    bool overflow = false;
};

bool has_utf8_bom(const std::uint8_t* bytes, std::size_t size) {
    return size >= 3 && bytes[0] == 0xEFU && bytes[1] == 0xBBU &&
           bytes[2] == 0xBFU;
}

bool known_binary_signature(const std::uint8_t* bytes, std::size_t size) {
    if (size >= 4) {
        if ((bytes[0] == 'O' && bytes[1] == 'T' && bytes[2] == 'T' &&
             bytes[3] == 'O') ||
            (bytes[0] == 0x00U && bytes[1] == 0x01U && bytes[2] == 0x00U &&
             bytes[3] == 0x00U) ||
            (bytes[0] == '%' && bytes[1] == 'P' && bytes[2] == 'D' &&
             bytes[3] == 'F') ||
            (bytes[0] == 0x89U && bytes[1] == 'P' && bytes[2] == 'N' &&
             bytes[3] == 'G') ||
            (bytes[0] == 0x7FU && bytes[1] == 'E' && bytes[2] == 'L' &&
             bytes[3] == 'F') ||
            (bytes[0] == 'P' && bytes[1] == 'K' &&
             (bytes[2] == 3U || bytes[2] == 5U || bytes[2] == 7U) &&
             (bytes[3] == 4U || bytes[3] == 6U || bytes[3] == 8U))) {
            return true;
        }
    }
    if (size >= 3 && bytes[0] == 0xFFU && bytes[1] == 0xD8U &&
        bytes[2] == 0xFFU) {
        return true;
    }
    if (size >= 6 &&
        ((bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
          bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9') &&
          bytes[5] == 'a') ||
         (bytes[0] == 0xFFU && bytes[1] == 0xFEU && bytes[2] == 0x00U &&
          bytes[3] == 0x00U) ||
         (bytes[0] == 0x00U && bytes[1] == 0x00U && bytes[2] == 0xFEU &&
          bytes[3] == 0xFFU))) {
        return true;
    }
    // UTF-16 is deliberately not guessed as a legacy single/double-byte TXT.
    return size >= 2 &&
           ((bytes[0] == 0xFFU && bytes[1] == 0xFEU) ||
            (bytes[0] == 0xFEU && bytes[1] == 0xFFU));
}

std::size_t effective_size(const std::uint8_t* bytes, std::size_t size) {
    // DOS/Windows text files sometimes retain a final Ctrl-Z EOF marker.
    return size != 0 && bytes[size - 1] == 0x1AU ? size - 1 : size;
}

bool forbidden_raw_control(const std::uint8_t* bytes,
                           std::size_t size,
                           bool allow_escape) {
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t value = bytes[index];
        if (value == 0 || value == 0x7FU) return true;
        if (value < 0x20U && value != '\t' && value != '\n' && value != '\r' &&
            !(allow_escape && value == 0x1BU)) {
            return true;
        }
    }
    return false;
}

std::size_t utf8_length(std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) return 1;
    if (codepoint <= 0x7FFU) return 2;
    if (codepoint <= 0xFFFFU) return 3;
    return 4;
}

void note_codepoint(Candidate& candidate, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU || codepoint == kReplacement) return;
    ++candidate.non_ascii;
    if ((codepoint >= 0x3400U && codepoint <= 0x4DBFU) ||
        (codepoint >= 0x4E00U && codepoint <= 0x9FFFU) ||
        (codepoint >= 0xF900U && codepoint <= 0xFAFFU)) {
        ++candidate.han;
    }
    if ((codepoint >= 0x3040U && codepoint <= 0x30FFU) ||
        (codepoint >= 0x31F0U && codepoint <= 0x31FFU)) {
        ++candidate.kana;
    }
    if (codepoint >= 0xFF61U && codepoint <= 0xFF9FU) {
        ++candidate.halfwidth_kana;
    }
    if ((codepoint >= 0x3000U && codepoint <= 0x303FU) ||
        codepoint == 0x30FCU) {
        ++candidate.japanese_punctuation;
    }
}

bool append_codepoint(Candidate& candidate,
                      std::uint32_t codepoint,
                      std::size_t maximum_output_bytes) {
    const std::size_t required = utf8_length(codepoint);
    if (candidate.output.size() > maximum_output_bytes ||
        required > maximum_output_bytes - candidate.output.size()) {
        candidate.overflow = true;
        // Detection must still score the entire stream. Otherwise a correct
        // candidate that exceeds the admission budget could be discarded in
        // favor of a wrong encoding whose partial output happened to fit.
        note_codepoint(candidate, codepoint);
        return false;
    }
    utf8_append(codepoint, candidate.output);
    note_codepoint(candidate, codepoint);
    return true;
}

bool append_replacement(Candidate& candidate,
                        std::size_t maximum_output_bytes) {
    ++candidate.replacements;
    ++candidate.syntax_errors;
    return append_codepoint(candidate, kReplacement, maximum_output_bytes);
}

std::uint32_t gbk_mapping(std::uint8_t lead, std::uint8_t trail) {
    if (lead < 0x81U || lead > 0xFEU || trail < 0x40U || trail == 0x7FU ||
        trail > 0xFEU) {
        return 0;
    }
    const std::size_t trail_index =
        trail < 0x7FU ? trail - 0x40U : trail - 0x41U;
    const std::size_t pointer = (lead - 0x81U) * 190U + trail_index;
    return kGbkTable[pointer];
}

std::size_t shift_jis_pointer(std::uint8_t lead, std::uint8_t trail) {
    const std::size_t lead_offset = lead < 0xA0U ? 0x81U : 0xC1U;
    const std::size_t trail_offset = trail < 0x7FU ? 0x40U : 0x41U;
    return (lead - lead_offset) * 188U + (trail - trail_offset);
}

std::uint32_t shift_jis_mapping(std::uint8_t lead, std::uint8_t trail) {
    const bool lead_valid = (lead >= 0x81U && lead <= 0x9FU) ||
                            (lead >= 0xE0U && lead <= 0xFCU);
    if (!lead_valid || trail < 0x40U || trail == 0x7FU || trail > 0xFCU) {
        return 0;
    }
    const std::size_t pointer = shift_jis_pointer(lead, trail);
    return pointer < 11280U ? kShiftJisTable[pointer] : 0;
}

std::uint32_t jis0208_mapping(std::uint8_t row, std::uint8_t cell) {
    if (row < 0x21U || row > 0x7EU || cell < 0x21U || cell > 0x7EU) {
        return 0;
    }
    const std::size_t pointer = (row - 0x21U) * 94U + (cell - 0x21U);
    return kShiftJisTable[pointer];
}

std::uint32_t jis0212_mapping(std::uint8_t row, std::uint8_t cell) {
    if (row < 0x21U || row > 0x7EU || cell < 0x21U || cell > 0x7EU) {
        return 0;
    }
    const std::size_t pointer = (row - 0x21U) * 94U + (cell - 0x21U);
    return kJis0212Table[pointer];
}

Candidate decode_gbk(const std::uint8_t* bytes,
                     std::size_t size,
                     std::size_t maximum_output_bytes) {
    Candidate result;
    result.encoding = TextEncoding::Gbk;
    result.output.reserve(std::min(size, maximum_output_bytes));
    for (std::size_t index = 0; index < size;) {
        const std::uint8_t first = bytes[index];
        if (first <= 0x7FU) {
            append_codepoint(result, first, maximum_output_bytes);
            ++index;
        } else if (first == 0x80U) {
            append_codepoint(result, 0x20ACU, maximum_output_bytes);
            ++index;
        } else if (index + 1 < size) {
            const std::uint32_t mapped = gbk_mapping(first, bytes[index + 1]);
            if (mapped != 0) {
                append_codepoint(result, mapped, maximum_output_bytes);
                ++result.mapped_pairs;
                index += 2;
            } else {
                append_replacement(result, maximum_output_bytes);
                ++index;
            }
        } else {
            append_replacement(result, maximum_output_bytes);
            ++index;
        }
    }
    return result;
}

Candidate decode_shift_jis(const std::uint8_t* bytes,
                           std::size_t size,
                           std::size_t maximum_output_bytes) {
    Candidate result;
    result.encoding = TextEncoding::ShiftJis;
    result.output.reserve(std::min(size, maximum_output_bytes));
    for (std::size_t index = 0; index < size;) {
        const std::uint8_t first = bytes[index];
        if (first <= 0x7FU) {
            append_codepoint(result, first, maximum_output_bytes);
            ++index;
        } else if (first >= 0xA1U && first <= 0xDFU) {
            append_codepoint(result, 0xFF61U + first - 0xA1U,
                             maximum_output_bytes);
            ++index;
        } else if (index + 1 < size) {
            const std::uint32_t mapped =
                shift_jis_mapping(first, bytes[index + 1]);
            if (mapped != 0) {
                append_codepoint(result, mapped, maximum_output_bytes);
                ++result.mapped_pairs;
                index += 2;
            } else {
                append_replacement(result, maximum_output_bytes);
                ++index;
            }
        } else {
            append_replacement(result, maximum_output_bytes);
            ++index;
        }
    }
    return result;
}

Candidate decode_euc_jp(const std::uint8_t* bytes,
                        std::size_t size,
                        std::size_t maximum_output_bytes) {
    Candidate result;
    result.encoding = TextEncoding::EucJp;
    result.output.reserve(std::min(size, maximum_output_bytes));
    for (std::size_t index = 0; index < size;) {
        const std::uint8_t first = bytes[index];
        if (first <= 0x7FU) {
            append_codepoint(result, first, maximum_output_bytes);
            ++index;
            continue;
        }
        if (first == 0x8EU && index + 1 < size &&
            bytes[index + 1] >= 0xA1U && bytes[index + 1] <= 0xDFU) {
            append_codepoint(result, 0xFF61U + bytes[index + 1] - 0xA1U,
                             maximum_output_bytes);
            ++result.mapped_pairs;
            index += 2;
            continue;
        }
        if (first == 0x8FU && index + 2 < size &&
            bytes[index + 1] >= 0xA1U && bytes[index + 1] <= 0xFEU &&
            bytes[index + 2] >= 0xA1U && bytes[index + 2] <= 0xFEU) {
            const std::uint32_t mapped = jis0212_mapping(
                bytes[index + 1] - 0x80U, bytes[index + 2] - 0x80U);
            if (mapped != 0) {
                append_codepoint(result, mapped, maximum_output_bytes);
                ++result.mapped_pairs;
            } else {
                append_replacement(result, maximum_output_bytes);
            }
            index += 3;
            continue;
        }
        if (first >= 0xA1U && first <= 0xFEU && index + 1 < size &&
            bytes[index + 1] >= 0xA1U && bytes[index + 1] <= 0xFEU) {
            const std::uint32_t mapped = jis0208_mapping(
                first - 0x80U, bytes[index + 1] - 0x80U);
            if (mapped != 0) {
                append_codepoint(result, mapped, maximum_output_bytes);
                ++result.mapped_pairs;
            } else {
                append_replacement(result, maximum_output_bytes);
            }
            index += 2;
            continue;
        }
        append_replacement(result, maximum_output_bytes);
        ++index;
    }
    return result;
}

enum class IsoState : std::uint8_t {
    Ascii,
    Roman,
    Katakana,
    Jis0208,
    Jis0212,
};

Candidate decode_iso_2022_jp(const std::uint8_t* bytes,
                             std::size_t size,
                             std::size_t maximum_output_bytes) {
    Candidate result;
    result.encoding = TextEncoding::Iso2022Jp;
    result.output.reserve(std::min(size, maximum_output_bytes));
    IsoState state = IsoState::Ascii;
    for (std::size_t index = 0; index < size;) {
        const std::uint8_t first = bytes[index];
        if (first == 0x1BU) {
            if (index + 2 < size && bytes[index + 1] == '(' &&
                (bytes[index + 2] == 'B' || bytes[index + 2] == 'J' ||
                 bytes[index + 2] == 'I')) {
                state = bytes[index + 2] == 'B'
                            ? IsoState::Ascii
                            : (bytes[index + 2] == 'J' ? IsoState::Roman
                                                       : IsoState::Katakana);
                ++result.recognized_escapes;
                index += 3;
                continue;
            }
            if (index + 2 < size && bytes[index + 1] == '$' &&
                (bytes[index + 2] == '@' || bytes[index + 2] == 'B')) {
                state = IsoState::Jis0208;
                ++result.recognized_escapes;
                index += 3;
                continue;
            }
            if (index + 3 < size && bytes[index + 1] == '$' &&
                bytes[index + 2] == '(' && bytes[index + 3] == 'D') {
                state = IsoState::Jis0212;
                ++result.recognized_escapes;
                index += 4;
                continue;
            }
            // ISO-2022-JP-1 permits an ESC & @ announcement before a later
            // designation. It has no visible character of its own.
            if (index + 2 < size && bytes[index + 1] == '&' &&
                bytes[index + 2] == '@') {
                ++result.recognized_escapes;
                index += 3;
                continue;
            }
            append_replacement(result, maximum_output_bytes);
            ++index;
            continue;
        }

        // Keep ordinary TXT line controls usable even if a producer failed to
        // return to ASCII before the line boundary.
        if (first == '\t' || first == '\n' || first == '\r') {
            append_codepoint(result, first, maximum_output_bytes);
            ++index;
            continue;
        }

        if (state == IsoState::Jis0208 || state == IsoState::Jis0212) {
            if (index + 1 >= size) {
                append_replacement(result, maximum_output_bytes);
                ++index;
                continue;
            }
            const std::uint32_t mapped =
                state == IsoState::Jis0208
                    ? jis0208_mapping(first, bytes[index + 1])
                    : jis0212_mapping(first, bytes[index + 1]);
            if (mapped != 0) {
                append_codepoint(result, mapped, maximum_output_bytes);
                ++result.mapped_pairs;
                index += 2;
            } else {
                append_replacement(result, maximum_output_bytes);
                ++index;
            }
            continue;
        }

        if (first > 0x7FU) {
            append_replacement(result, maximum_output_bytes);
        } else if (state == IsoState::Katakana) {
            if (first >= 0x21U && first <= 0x5FU) {
                append_codepoint(result, 0xFF61U + first - 0x21U,
                                 maximum_output_bytes);
            } else if (first == 0x20U) {
                append_codepoint(result, first, maximum_output_bytes);
            } else {
                append_replacement(result, maximum_output_bytes);
            }
        } else if (state == IsoState::Roman && first == 0x5CU) {
            append_codepoint(result, 0x00A5U, maximum_output_bytes);
        } else if (state == IsoState::Roman && first == 0x7EU) {
            append_codepoint(result, 0x203EU, maximum_output_bytes);
        } else {
            append_codepoint(result, first, maximum_output_bytes);
        }
        ++index;
    }
    return result;
}

std::int64_t candidate_score(const Candidate& candidate) {
    std::int64_t score =
        static_cast<std::int64_t>(candidate.mapped_pairs) * 8 +
        static_cast<std::int64_t>(candidate.non_ascii) * 2 +
        static_cast<std::int64_t>(candidate.han) * 5 -
        static_cast<std::int64_t>(candidate.replacements) * 120;
    if (candidate.encoding == TextEncoding::ShiftJis ||
        candidate.encoding == TextEncoding::EucJp) {
        score += static_cast<std::int64_t>(candidate.kana) * 14 +
                 static_cast<std::int64_t>(candidate.japanese_punctuation) * 6 +
                 static_cast<std::int64_t>(candidate.halfwidth_kana) * 2;
        // A Chinese GBK stream often consists entirely of bytes in A1..DF;
        // treating each as standalone halfwidth kana is syntactically valid
        // Shift-JIS but linguistically implausible.
        if (candidate.halfwidth_kana > 2 &&
            candidate.halfwidth_kana * 2 > candidate.non_ascii) {
            score -= static_cast<std::int64_t>(candidate.halfwidth_kana) * 12;
        }
    } else if (candidate.encoding == TextEncoding::Gbk) {
        // GBK is the deterministic fallback for all-Han streams, where no
        // detector can distinguish languages from byte legality alone.
        score += static_cast<std::int64_t>(candidate.han);
    }
    return score;
}

bool candidate_acceptable(const Candidate& candidate, std::size_t size) {
    if (candidate.non_ascii == 0) return false;
    const std::size_t tolerance = std::max<std::size_t>(2, size / 20U);
    return candidate.replacements <= tolerance;
}

bool finish_candidate(Candidate&& candidate,
                      std::string& output,
                      TextDecodeInfo& info,
                      std::string& error) {
    if (candidate.overflow) {
        error = "decoded TXT exceeds the configured size limit";
        return false;
    }
    output = std::move(candidate.output);
    info.encoding = candidate.encoding;
    info.replacement_count = candidate.replacements;
    info.had_bom = false;
    error.clear();
    return true;
}

bool decode_utf8(const std::uint8_t* bytes,
                 std::size_t size,
                 std::size_t maximum_output_bytes,
                 std::string& output,
                 TextDecodeInfo& info,
                 std::string& error) {
    const Utf8ValidationResult validated = utf8_validate(bytes, size, true);
    if (!validated.valid()) {
        error = "TXT has invalid UTF-8";
        return false;
    }
    const std::size_t bom_bytes = validated.had_bom ? 3U : 0U;
    if (size - bom_bytes > maximum_output_bytes) {
        error = "decoded TXT exceeds the configured size limit";
        return false;
    }
    const Utf8ValidationResult sanitized =
        utf8_sanitize(bytes, size, output, true);
    info.encoding = TextEncoding::Utf8;
    info.replacement_count = 0;
    info.had_bom = sanitized.had_bom;
    error.clear();
    return true;
}

}  // namespace

const char* text_encoding_name(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Gbk: return "GBK";
    case TextEncoding::ShiftJis: return "Shift-JIS";
    case TextEncoding::EucJp: return "EUC-JP";
    case TextEncoding::Iso2022Jp: return "ISO-2022-JP";
    }
    return "unknown";
}

bool decode_text_as(const std::uint8_t* bytes,
                    std::size_t size,
                    TextEncoding encoding,
                    std::size_t maximum_output_bytes,
                    std::string& output,
                    TextDecodeInfo& info,
                    std::string& error) {
    output.clear();
    info = {};
    error.clear();
    if (bytes == nullptr && size != 0) {
        error = "TXT source pointer is null";
        return false;
    }
    if (known_binary_signature(bytes, size)) {
        error = "file contains binary data, not plain text";
        return false;
    }
    const std::size_t bounded_size = effective_size(bytes, size);
    if (bounded_size == 0) {
        info.encoding = TextEncoding::Utf8;
        return true;
    }
    const bool allow_escape = encoding == TextEncoding::Iso2022Jp;
    if (forbidden_raw_control(bytes, bounded_size, allow_escape)) {
        error = "file contains binary control data, not plain text";
        return false;
    }
    switch (encoding) {
    case TextEncoding::Utf8:
        return decode_utf8(bytes, bounded_size, maximum_output_bytes, output,
                           info, error);
    case TextEncoding::Gbk:
        return finish_candidate(
            decode_gbk(bytes, bounded_size, maximum_output_bytes), output, info,
            error);
    case TextEncoding::ShiftJis:
        return finish_candidate(
            decode_shift_jis(bytes, bounded_size, maximum_output_bytes), output,
            info, error);
    case TextEncoding::EucJp:
        return finish_candidate(
            decode_euc_jp(bytes, bounded_size, maximum_output_bytes), output,
            info, error);
    case TextEncoding::Iso2022Jp:
        return finish_candidate(
            decode_iso_2022_jp(bytes, bounded_size, maximum_output_bytes),
            output, info, error);
    }
    error = "unsupported TXT encoding";
    return false;
}

bool decode_text_auto(const std::uint8_t* bytes,
                      std::size_t size,
                      std::size_t maximum_output_bytes,
                      std::string& output,
                      TextDecodeInfo& info,
                      std::string& error) {
    output.clear();
    info = {};
    error.clear();
    if (bytes == nullptr && size != 0) {
        error = "TXT source pointer is null";
        return false;
    }
    if (known_binary_signature(bytes, size)) {
        error = "file contains binary data, not plain text";
        return false;
    }
    const std::size_t bounded_size = effective_size(bytes, size);
    if (bounded_size == 0) {
        info.encoding = TextEncoding::Utf8;
        return true;
    }
    if (has_utf8_bom(bytes, bounded_size)) {
        if (forbidden_raw_control(bytes + 3, bounded_size - 3, false)) {
            error = "file contains binary control data, not plain text";
            return false;
        }
        return decode_utf8(bytes, bounded_size, maximum_output_bytes, output,
                           info, error);
    }

    const bool has_escape = std::find(bytes, bytes + bounded_size, 0x1BU) !=
                            bytes + bounded_size;
    if (has_escape) {
        if (forbidden_raw_control(bytes, bounded_size, true)) {
            error = "file contains binary control data, not plain text";
            return false;
        }
        Candidate iso =
            decode_iso_2022_jp(bytes, bounded_size, maximum_output_bytes);
        if (iso.recognized_escapes != 0 &&
            iso.syntax_errors <= std::max<std::size_t>(2, bounded_size / 20U)) {
            return finish_candidate(std::move(iso), output, info, error);
        }
        error = "TXT contains an invalid ISO-2022-JP escape sequence";
        return false;
    }
    if (forbidden_raw_control(bytes, bounded_size, false)) {
        error = "file contains binary control data, not plain text";
        return false;
    }

    const Utf8ValidationResult utf8 =
        utf8_validate(bytes, bounded_size, false);
    if (utf8.valid()) {
        return decode_utf8(bytes, bounded_size, maximum_output_bytes, output,
                           info, error);
    }

    // Probe all three ambiguous legacy families without retaining their text,
    // then decode only the winner. Holding three maximum-sized UTF-8 buffers
    // at once is too expensive on a 64 MiB calculator.
    Candidate gbk = decode_gbk(bytes, bounded_size, 0);
    Candidate shift_jis = decode_shift_jis(bytes, bounded_size, 0);
    Candidate euc_jp = decode_euc_jp(bytes, bounded_size, 0);

    Candidate* selected = nullptr;
    // Keep GBK first: exact-score ties are intrinsically ambiguous and the
    // calculator's primary CJK fallback is Simplified Chinese.
    Candidate* candidates[] = {&gbk, &shift_jis, &euc_jp};
    for (Candidate* candidate : candidates) {
        if (!candidate_acceptable(*candidate, bounded_size)) continue;
        if (selected == nullptr ||
            candidate_score(*candidate) > candidate_score(*selected)) {
            selected = candidate;
        }
    }
    if (selected == nullptr) {
        error = "TXT encoding could not be detected as UTF-8, GBK, Shift-JIS, or EUC-JP";
        return false;
    }
    if (selected == &gbk) {
        return finish_candidate(
            decode_gbk(bytes, bounded_size, maximum_output_bytes), output, info,
            error);
    }
    if (selected == &shift_jis) {
        return finish_candidate(
            decode_shift_jis(bytes, bounded_size, maximum_output_bytes), output,
            info, error);
    }
    return finish_candidate(
        decode_euc_jp(bytes, bounded_size, maximum_output_bytes), output, info,
        error);
}

bool decode_text_auto(std::string&& input,
                      std::size_t maximum_output_bytes,
                      std::string& output,
                      TextDecodeInfo& info,
                      std::string& error) {
    output.clear();
    info = {};
    error.clear();
    const auto* bytes =
        reinterpret_cast<const std::uint8_t*>(input.data());
    if (known_binary_signature(bytes, input.size())) {
        error = "file contains binary data, not plain text";
        return false;
    }
    const std::size_t bounded_size = effective_size(bytes, input.size());
    if (bounded_size != input.size()) input.resize(bounded_size);
    if (bounded_size == 0) {
        info.encoding = TextEncoding::Utf8;
        output = std::move(input);
        return true;
    }

    bytes = reinterpret_cast<const std::uint8_t*>(input.data());
    const bool had_bom = has_utf8_bom(bytes, bounded_size);
    const std::size_t content_offset = had_bom ? 3U : 0U;
    const bool has_escape =
        std::find(bytes + content_offset, bytes + bounded_size, 0x1BU) !=
        bytes + bounded_size;
    if (!has_escape &&
        !forbidden_raw_control(bytes + content_offset,
                               bounded_size - content_offset, false)) {
        const Utf8ValidationResult validated =
            utf8_validate(bytes, bounded_size, true);
        if (validated.valid()) {
            if (bounded_size - content_offset > maximum_output_bytes) {
                error = "decoded TXT exceeds the configured size limit";
                return false;
            }
            if (had_bom) input.erase(0, content_offset);
            output = std::move(input);
            info.encoding = TextEncoding::Utf8;
            info.replacement_count = 0;
            info.had_bom = had_bom;
            return true;
        }
    }

    return decode_text_auto(
        reinterpret_cast<const std::uint8_t*>(input.data()), input.size(),
        maximum_output_bytes, output, info, error);
}

}  // namespace nmarkdown
