#include "nmarkdown/app/application.h"

#include <array>
#include <memory>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "nmarkdown/app/viewer.h"
#include "nmarkdown/document/markdown.h"
#include "nmarkdown/document/state.h"
#include "nmarkdown/document/text_encoding.h"
#include "nmarkdown/document/unicode.h"
#include "nmarkdown/document/utf8.h"
#include "nmarkdown/io/memory_random_access.h"
#include "nmarkdown/platform/allocation_stats.h"

#if defined(NMARKDOWN_PRIME)
#include "prime_log.h" /* prime_log_write */

/* ⚠️ 实测（HP Prime 原生 + 引导器直接跳 app_entry 的环境）：
 *   prime_log_write("<字面量>")  可以；
 *   prime_log_write(<运行时指针>) 会崩（v7 通过 / v9 崩，只差这一点）。
 * 因此日志统一在编译期拼成单个字面量，绝不把运行时指针传进 prime_log_write。 */
#define integration_log(msg) prime_log_write("[IT] " msg "\n")
/* 错误日志同样只输出字面量（stage 在所有调用点都是字面量）；error 是运行时
 * std::string，不能传给 prime_log_write，故只记录阶段名。 */
#define integration_error(stage, error)           \
    do {                                          \
        prime_log_write("[IT!] " stage "\n");    \
        (void)(error);                            \
    } while (0)
#define allocation_profile_log(stage) prime_log_write("[MEM] " stage "\n")
#endif


namespace nmarkdown {
namespace {

class DisplayShutdownGuard {
public:
    explicit DisplayShutdownGuard(Display& display) : display_(display) {}
    ~DisplayShutdownGuard() { display_.shutdown(); }

    DisplayShutdownGuard(const DisplayShutdownGuard&) = delete;
    DisplayShutdownGuard& operator=(const DisplayShutdownGuard&) = delete;

private:
    Display& display_;
};

constexpr std::uint64_t kLoadingFeedbackDelayMs = 120;
constexpr std::uint64_t kLargeDocumentFeedbackBytes = 128U * 1024U;
constexpr std::uint64_t kLargeFontFeedbackBytes = 256U * 1024U;

// After promoting a font payload into RAM, the reader must still be able to
// allocate its ordinary working set: the 1 MiB plain-text sequential cache
// plus its scratch block, a shaped-screen window, and room for the glyph
// atlas to keep growing in 64 KiB pages. Probe that mix — including the
// contiguity of the largest block — before keeping a resident font, so a
// tight TI-OS heap degrades back to streaming instead of failing later
// document opens. Markdown documents can need more than this reserve; their
// open path already reports allocation failures with a clear message.
bool heap_reserve_available() {
    struct Block {
        std::size_t size;
        std::size_t count;
    };
    constexpr Block kReserveBlocks[] = {
        {1U * 1024U * 1024U, 1},  // plain-text sequential cache (contiguous)
        {512U * 1024U, 1},        // screen window, scratch, decode overhead
        {64U * 1024U, 8},         // glyph atlas growth headroom
    };
    std::vector<void*> held;
    held.reserve(16);
    bool available = true;
    for (const Block& block : kReserveBlocks) {
        for (std::size_t index = 0; index < block.count; ++index) {
            void* memory = ::operator new(block.size, std::nothrow);
            if (memory == nullptr) {
                available = false;
                break;
            }
            held.push_back(memory);
        }
        if (!available) break;
    }
    for (void* memory : held) {
        ::operator delete(memory);
    }
    return available;
}
constexpr std::size_t kLoadingAnimationProgressInterval = 8;
constexpr std::size_t kPlainTextEncodingProbeBytes = 64U * 1024U;

// Synchronous filesystem adapters report bounded checkpoints. This guard keeps
// fast operations transactional (no intermediate frame), but presents a
// compact status card once the operation has lasted long enough or a probed
// file is already known to be expensive on the calculator. The card remains
// visible during the following blocking stage and is removed on every return
// path by the destructor.
class DelayedLoadingFeedback {
public:
    DelayedLoadingFeedback(Viewer& viewer,
                           Display& display,
                           FileSystem& files,
                           Clock& clock,
                           std::string title,
                           std::string detail)
        : viewer_(viewer),
          display_(display),
          files_(files),
          clock_(clock),
          title_(std::move(title)),
          detail_(std::move(detail)),
          started_ms_(clock.milliseconds()) {
        files_.set_operation_progress_callback(&progress_callback, this);
    }

    ~DelayedLoadingFeedback() {
        files_.set_operation_progress_callback(nullptr, nullptr);
        if (visible_) viewer_.clear_loading_feedback();
    }

    DelayedLoadingFeedback(const DelayedLoadingFeedback&) = delete;
    DelayedLoadingFeedback& operator=(const DelayedLoadingFeedback&) = delete;

    void stage(std::string detail, bool expected_slow = false) {
        detail_ = std::move(detail);
        progress_percent_ = -1;
        checkpoint(expected_slow);
    }

    // A nested DelayedLoadingFeedback (for example a font promotion inside a
    // document open) unregisters the shared filesystem progress callback when
    // it is destroyed. The outer scope calls this to take the callback back
    // so later long reads keep animating its card.
    void reattach() {
        files_.set_operation_progress_callback(&progress_callback, this);
    }

    void checkpoint(bool expected_slow = false) {
        const std::uint64_t now = clock_.milliseconds();
        if (!visible_ && !expected_slow &&
            now - started_ms_ < kLoadingFeedbackDelayMs) {
            return;
        }
        // ClockNdless deliberately uses logical time rather than taking over
        // an OS timer channel. Filesystem checkpoints therefore still give
        // long synchronous scans a bounded way to animate without repainting
        // on every directory entry.
        if (visible_ && painted_detail_ == detail_) {
            if (++progress_checkpoints_since_paint_ <
                kLoadingAnimationProgressInterval) {
                return;
            }
        }
        progress_checkpoints_since_paint_ = 0;
        viewer_.show_loading_feedback(title_, detail_, progress_percent_);
        viewer_.render(display_.surface());
        display_.present();
        viewer_.clear_dirty();
        visible_ = true;
        painted_detail_ = detail_;
        integration_log("LOADING_FEEDBACK_PRESENTED");
    }

private:
    void progress(std::uint64_t completed, std::uint64_t total) {
        if (total != 0) {
            progress_percent_ = static_cast<int>(
                std::min<std::uint64_t>(100, completed * 100U / total));
        }
        checkpoint();
    }

    static void progress_callback(void* context,
                                  std::uint64_t completed,
                                  std::uint64_t total) {
        static_cast<DelayedLoadingFeedback*>(context)->progress(
            completed, total);
    }

    Viewer& viewer_;
    Display& display_;
    FileSystem& files_;
    Clock& clock_;
    std::string title_;
    std::string detail_;
    std::string painted_detail_;
    std::uint64_t started_ms_ = 0;
    std::size_t progress_checkpoints_since_paint_ = 0;
    int progress_percent_ = -1;
    bool visible_ = false;
};

std::string local_path_from_link(std::string_view current,
                                 std::string_view target) {
    if (target.empty()) return std::string(current);
    if (target.front() == '/') return std::string(target);
    const std::size_t slash = current.find_last_of("/\\");
    std::string combined = slash == std::string_view::npos
                               ? std::string(target)
                               : std::string(current.substr(0, slash + 1)) +
                                     std::string(target);
    const bool absolute = !combined.empty() && combined.front() == '/';
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin <= combined.size()) {
        const std::size_t end = combined.find('/', begin);
        const std::size_t limit = end == std::string::npos ? combined.size() : end;
        const std::string component = combined.substr(begin, limit - begin);
        if (component.empty() || component == ".") {
            // Ignore repeated separators and current-directory components.
        } else if (component == ".." && !components.empty() &&
                   components.back() != "..") {
            components.pop_back();
        } else if (component != ".." || !absolute) {
            components.push_back(component);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    std::string result = absolute ? "/" : "";
    for (const std::string& component : components) {
        if (!result.empty() && result.back() != '/') result.push_back('/');
        result += component;
    }
    return result;
}

bool is_reader_document_path(std::string_view path) {
    if (path.size() > 4) {
        std::string suffix(path.substr(path.size() - 4));
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (suffix == ".tns") path.remove_suffix(4);
    }
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash)) {
        return true;
    }
    std::string extension(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension == ".md" || extension == ".markdown" ||
           extension == ".txt";
}

bool is_plain_text_path(std::string_view path) {
    if (path.size() > 4) {
        std::string suffix(path.substr(path.size() - 4));
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (suffix == ".tns") path.remove_suffix(4);
    }
    if (path.size() < 4) return false;
    std::string extension(path.substr(path.size() - 4));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return extension == ".txt";
}

bool error_is_missing(std::string_view error) {
    std::string lower(error);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return lower.find("no such file") != std::string::npos ||
           lower.find("not found") != std::string::npos ||
           lower.find("does not exist") != std::string::npos;
}

bool looks_like_binary_document(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() >= 4) {
        const bool known_binary =
            (bytes[0] == 'O' && bytes[1] == 'T' && bytes[2] == 'T' && bytes[3] == 'O') ||
            (bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x00 && bytes[3] == 0x00) ||
            (bytes[0] == '%' && bytes[1] == 'P' && bytes[2] == 'D' && bytes[3] == 'F') ||
            (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G');
        if (known_binary) return true;
    }
    std::size_t controls = 0;
    const std::size_t sampled = std::min<std::size_t>(bytes.size(), 4096);
    for (std::size_t index = 0; index < sampled; ++index) {
        const std::uint8_t value = bytes[index];
        if (value == 0) return true;
        if (value < 0x20U && value != '\n' && value != '\r' && value != '\t') {
            ++controls;
        }
    }
    return sampled != 0 && controls > std::max<std::size_t>(2, sampled / 100);
}

std::size_t complete_utf8_probe_prefix(
    const std::vector<std::uint8_t>& bytes,
    std::size_t requested) {
    requested = std::min(requested, bytes.size());
    if (requested == bytes.size()) return requested;
    while (requested != 0 &&
           (bytes[requested] & 0xC0U) == 0x80U) {
        --requested;
    }
    return requested;
}

// The CJK-role payload is only worth residency when the document actually
// contains CJK text. A bounded prefix decides that: a codepoint match can
// stop early, and a truncated trailing sequence decodes as invalid and is
// skipped, so no boundary trimming is needed.
constexpr std::size_t kCjkSampleBytes = 64U * 1024U;

bool sample_contains_cjk(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || size == 0) return false;
    return contains_cjk_text(std::string_view(
        reinterpret_cast<const char*>(bytes),
        std::min(size, kCjkSampleBytes)));
}

// Startup variant for a document that has not been read yet: one bounded
// sequential read, then the same encoding probe the loader uses. A legacy
// CJK encoding (GBK, Shift-JIS, ...) is CJK by definition; UTF-8 samples
// are scanned for CJK codepoints. Unknown or unreadable content reports
// false, which keeps the CJK payload streaming — the safe direction.
bool sampled_document_contains_cjk(FileSystem& files,
                                   const std::string& path) {
    std::shared_ptr<RandomAccessData> source;
    std::string error;
    if (!files.open_random_access(path.c_str(), source, error) ||
        source == nullptr || source->size() == 0) {
        return false;
    }
    std::vector<std::uint8_t> sample(static_cast<std::size_t>(
        std::min<std::uint64_t>(source->size(), kCjkSampleBytes)));
    if (!source->read(0, sample.data(), sample.size())) return false;
    std::string decoded;
    TextDecodeInfo info;
    std::string decode_error;
    if (!decode_text_auto(sample.data(),
                          complete_utf8_probe_prefix(sample, sample.size()),
                          kCjkSampleBytes * 4U, decoded, info,
                          decode_error)) {
        return false;
    }
    if (info.encoding != TextEncoding::Utf8) return true;
    return contains_cjk_text(decoded);
}

std::uint64_t sampled_document_identity(
    const std::vector<std::uint8_t>& prefix,
    RandomAccessData& source,
    std::uint64_t source_size) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    const auto add = [&](const std::uint8_t* data, std::size_t size,
                         std::uint64_t& value) {
        for (std::size_t index = 0; index < size; ++index) {
            value ^= data[index];
            value *= kPrime;
        }
    };
    add(prefix.data(), prefix.size(), hash);

    constexpr std::size_t kTailBytes = 4096;
    if (source_size > prefix.size()) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(kTailBytes, source_size));
        std::array<std::uint8_t, kTailBytes> tail{};
        if (source.read(source_size - count, tail.data(), count)) {
            add(tail.data(), count, hash);
        }
    }
    hash ^= source_size;
    hash *= kPrime;
    return hash;
}

std::string font_label_from_path(std::string path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".tns") == 0) {
        path.resize(path.size() - 4);
        lower.resize(lower.size() - 4);
    }
    if (lower.size() > 4 &&
        (lower.compare(lower.size() - 4, 4, ".ttf") == 0 ||
         lower.compare(lower.size() - 4, 4, ".otf") == 0)) {
        path.resize(path.size() - 4);
    }
    return path;
}

const char* default_role_label(FontRole role) {
    switch (role) {
    case FontRole::BodySans: return "ASCII UI";
    case FontRole::BodySansItalic: return "Outline slant";
    case FontRole::Monospace: return "Built-in DejaVu Mono";
    case FontRole::Cjk: return "None";
    case FontRole::BodySansBold: return "Synthetic bold";
    case FontRole::BodySansBoldItalic: return "Synthetic bold italic";
    case FontRole::MonospaceItalic: return "Outline slant";
    default: return "None";
    }
}

std::string catalog_font_label(const FontFaceCatalogEntry& face,
                               const std::string& path) {
    if (face.family.empty()) return font_label_from_path(path);
    std::string label = face.family;
    if (!face.subfamily.empty() && face.subfamily != "Regular") {
        label += " ";
        label += face.subfamily;
    }
    return label;
}

std::string font_specific_error(std::string error) {
    if (error.empty()) return "font could not be loaded";
    constexpr std::string_view document = "document";
    std::size_t offset = 0;
    while ((offset = error.find(document, offset)) != std::string::npos) {
        error.replace(offset, document.size(), "font");
        offset += 4;
    }
    return error;
}

std::string state_specific_error(std::string error) {
    if (error.empty()) return "saved state could not be restored";
    constexpr std::string_view document = "document";
    std::size_t offset = 0;
    while ((offset = error.find(document, offset)) != std::string::npos) {
        error.replace(offset, document.size(), "state file");
        offset += 10;
    }
    return error;
}

constexpr std::size_t kMaximumRememberedFontPath = 4096;
constexpr std::size_t kRememberedFontRoleCount = kExternalFontRoleCount;
constexpr std::size_t kV2RememberedFontRoleCount = 6;
constexpr std::size_t kLegacyRememberedFontRoleCount = 4;
using RememberedFontPaths =
    std::array<std::string, kRememberedFontRoleCount>;
constexpr std::uint8_t kFontPreferenceMagic[] = {'N', 'M', 'F', '3'};
constexpr std::uint8_t kV2FontPreferenceMagic[] = {'N', 'M', 'F', '2'};
constexpr std::uint8_t kLegacyFontPreferenceMagic[] = {'N', 'M', 'F', '1'};
constexpr std::uint8_t kCjkFontPreferenceMagic[] = {'N', 'M', 'C', '1'};

std::uint32_t preference_checksum(const std::uint8_t* bytes,
                                  std::size_t size) {
    std::uint32_t hash = 2166136261U;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

std::string cjk_font_preference_path(std::string root) {
    if (root.empty()) root = ".";
    if (root.back() != '/' && root.back() != '\\') root.push_back('/');
    // Hidden files are deliberately skipped by the document/font scanners.
    return root + ".nmarkdown-cjk-font";
}

std::string font_preference_path(std::string root) {
    if (root.empty()) root = ".";
    if (root.back() != '/' && root.back() != '\\') root.push_back('/');
    return root + ".nmarkdown-fonts";
}

std::string font_preload_preference_path(std::string root) {
    if (root.empty()) root = ".";
    if (root.back() != '/' && root.back() != '\\') root.push_back('/');
    return root + ".nmarkdown-font-preload";
}

constexpr std::uint8_t kFontPreloadPreferenceMagic[6] = {
    'N', 'M', 'F', 'P', 'L', '1'};

void encode_font_preload_preference(bool enabled,
                                    std::vector<std::uint8_t>& bytes) {
    bytes.assign(std::begin(kFontPreloadPreferenceMagic),
                 std::end(kFontPreloadPreferenceMagic));
    bytes.push_back(enabled ? 1U : 0U);
    const std::uint32_t checksum =
        preference_checksum(bytes.data(), bytes.size());
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(checksum >> shift));
    }
}

bool decode_font_preload_preference(const std::uint8_t* bytes,
                                    std::size_t size,
                                    bool& enabled) {
    constexpr std::size_t kExpected =
        sizeof(kFontPreloadPreferenceMagic) + 1U + 4U;
    if (bytes == nullptr || size != kExpected ||
        !std::equal(std::begin(kFontPreloadPreferenceMagic),
                    std::end(kFontPreloadPreferenceMagic), bytes)) {
        return false;
    }
    const std::size_t payload = kExpected - 4U;
    std::uint32_t checksum = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        checksum |= static_cast<std::uint32_t>(bytes[payload + shift / 8])
                    << shift;
    }
    if (checksum != preference_checksum(bytes, payload)) return false;
    enabled = bytes[sizeof(kFontPreloadPreferenceMagic)] != 0;
    return true;
}

bool encode_font_preferences(const RememberedFontPaths& paths,
                             std::vector<std::uint8_t>& bytes,
                             std::string& error) {
    bytes.clear();
    error.clear();
    bytes.insert(bytes.end(), std::begin(kFontPreferenceMagic),
                 std::end(kFontPreferenceMagic));
    for (const std::string& path : paths) {
        if (path.size() > kMaximumRememberedFontPath ||
            path.find('\0') != std::string::npos) {
            error = "font path is too long or invalid";
            bytes.clear();
            return false;
        }
        const std::uint16_t length = static_cast<std::uint16_t>(path.size());
        bytes.push_back(static_cast<std::uint8_t>(length));
        bytes.push_back(static_cast<std::uint8_t>(length >> 8U));
        bytes.insert(bytes.end(), path.begin(), path.end());
    }
    const std::uint32_t checksum = preference_checksum(bytes.data(), bytes.size());
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(checksum >> shift));
    }
    return true;
}

bool decode_font_preferences(const std::uint8_t* bytes,
                             std::size_t size,
                             RememberedFontPaths& paths) {
    paths = {};
    if (bytes == nullptr || size < sizeof(kFontPreferenceMagic) + 4U) {
        return false;
    }
    std::size_t path_count = 0;
    if (std::equal(std::begin(kFontPreferenceMagic),
                   std::end(kFontPreferenceMagic), bytes)) {
        path_count = kRememberedFontRoleCount;
    } else if (std::equal(std::begin(kV2FontPreferenceMagic),
                          std::end(kV2FontPreferenceMagic), bytes)) {
        path_count = kV2RememberedFontRoleCount;
    } else if (std::equal(std::begin(kLegacyFontPreferenceMagic),
                          std::end(kLegacyFontPreferenceMagic), bytes)) {
        path_count = kLegacyRememberedFontRoleCount;
    } else {
        return false;
    }
    if (size < sizeof(kFontPreferenceMagic) + path_count * 2U + 4U) {
        return false;
    }
    std::size_t offset = sizeof(kFontPreferenceMagic);
    for (std::size_t index = 0; index < path_count; ++index) {
        if (offset + 2U > size - 4U) return false;
        const std::size_t length = static_cast<std::size_t>(bytes[offset]) |
                                   static_cast<std::size_t>(bytes[offset + 1])
                                       << 8U;
        offset += 2U;
        if (length > kMaximumRememberedFontPath ||
            offset + length > size - 4U) {
            paths = {};
            return false;
        }
        paths[index].assign(reinterpret_cast<const char*>(bytes + offset),
                            length);
        if (paths[index].find('\0') != std::string::npos) {
            paths = {};
            return false;
        }
        offset += length;
    }
    if (offset + 4U != size) {
        paths = {};
        return false;
    }
    std::uint32_t stored = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        stored |= static_cast<std::uint32_t>(bytes[offset + shift / 8U])
                  << shift;
    }
    if (stored != preference_checksum(bytes, offset)) {
        paths = {};
        return false;
    }
    return true;
}

bool decode_cjk_font_preference(const std::uint8_t* bytes,
                                std::size_t size,
                                std::string& path) {
    path.clear();
    constexpr std::size_t kFixedBytes =
        sizeof(kCjkFontPreferenceMagic) + 2U + 4U;
    if (bytes == nullptr || size < kFixedBytes ||
        !std::equal(std::begin(kCjkFontPreferenceMagic),
                    std::end(kCjkFontPreferenceMagic), bytes)) {
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(bytes[4]) |
                               static_cast<std::size_t>(bytes[5]) << 8U;
    if (length > kMaximumRememberedFontPath || size != kFixedBytes + length) {
        return false;
    }
    std::uint32_t stored = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        stored |= static_cast<std::uint32_t>(bytes[6 + length + shift / 8])
                  << shift;
    }
    if (stored != preference_checksum(bytes, 6 + length)) return false;
    path.assign(reinterpret_cast<const char*>(bytes + 6), length);
    return path.find('\0') == std::string::npos;
}

std::string document_label_from_path(std::string path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".tns") == 0) {
        path.resize(path.size() - 4);
    }
    return path.empty() ? "nMarkdown" : path;
}

}  // namespace

int run_reader(Display& display,
               Input& input,
               FileSystem& files,
               Clock& clock,
               const char* document_path,
               ReaderOptions options) {
    integration_log("READER_START");
    if (!display.initialize()) {
        integration_log("DISPLAY_FAIL");
        return 1;
    }
    integration_log("DISPLAY_READY");
    DisplayShutdownGuard shutdown_guard(display);

    // Viewer owns the font cache, layout state, and many long-lived vectors.
    // Keep it off the calculator's small process stack; Markdown parsing uses
    // enough temporary stack that a stack-resident Viewer can be overwritten.
    std::unique_ptr<Viewer> viewer_storage(new Viewer());
    Viewer& viewer = *viewer_storage;
    viewer.set_reading_mode(options.initial_reading_mode);
    integration_log("VIEWER_READY");
    if (viewer.text_ready()) {
        integration_log("HARFBUZZ_READY");
    } else {
        integration_log("HARFBUZZ_FAIL");
    }
    allocation_profile_log("viewer_ready");
    std::uint64_t identity = 0;
    std::string state_path;
    std::string current_path;
    RememberedFontPaths current_font_paths;
    struct RuntimeFontResource {
        FontFaceId id = 0;
        std::string path;
    };
    std::vector<RuntimeFontResource> runtime_font_resources;
    FontFaceId next_runtime_font_id = 10001;
    std::vector<FontFaceCatalogEntry> font_file_catalog;
    bool font_file_catalog_ready = false;
    bool font_file_catalog_truncated = false;
    std::vector<std::string> document_file_catalog;
    bool document_file_catalog_ready = false;
    bool document_file_catalog_truncated = false;
    const std::string remembered_font_path =
        font_preference_path(options.document_root);
    const std::string remembered_cjk_font_path =
        cjk_font_preference_path(options.document_root);
    const std::string remembered_font_preload_path =
        font_preload_preference_path(options.document_root);
    std::string pending_state_warning;
    bool state_save_failure_known = false;
    const auto save_current_state = [&]() {
        if (!options.persist_state ||
            (!viewer.has_markdown_document() &&
             !viewer.has_plain_text_document()) ||
            state_path.empty()) {
            return;
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!encode_reader_state(viewer.reader_state(identity), bytes, error)) {
            pending_state_warning = error.empty()
                                        ? "could not encode reader state"
                                        : error;
            state_save_failure_known = true;
            integration_log("STATE_SAVE_FAIL");
        } else if (!files.write_atomic(state_path.c_str(), bytes.data(),
                                       bytes.size(), error)) {
            pending_state_warning = error.empty()
                                        ? "could not write reader state"
                                        : error;
            state_save_failure_known = true;
            integration_log("STATE_SAVE_FAIL");
        } else {
            // write_atomic returns success only after the final destination is
            // committed, or byte-verified when Ndless rename is unavailable.
            state_save_failure_known = false;
            integration_log("STATE_SAVE_OK");
        }
    };
    // Size of the document that is open or about to open; the resident-font
    // policy uses it to decide whether one upfront sequential font read is
    // cheaper than the per-glyph streaming reads the session would otherwise
    // pay. Zero means unknown, which never justifies the upfront read.
    std::uint64_t current_document_bytes = 0;
    // Whether that document contains CJK text; the CJK-role payload never
    // becomes resident for documents that cannot exercise it.
    bool current_document_has_cjk = false;
    // One failed promotion (heap probe or allocation) is remembered so a
    // large-document open does not re-read a multi-megabyte payload just to
    // fail the same way; an explicit font apply from the menu retries.
    bool resident_font_promotion_failed = false;
    // Assigned after load_document below; declared first so opening a large
    // document can upgrade still-streamed fonts through the same code path.
    std::function<bool(const RememberedFontPaths&, bool, std::string&)>
        apply_font_assignments_safely;
    const auto load_document = [&](const std::string& path, std::string& error) -> bool {
        const std::string label = document_label_from_path(path);
        DelayedLoadingFeedback loading(viewer, display, files, clock,
                                       "Opening document", label);
        integration_log("DOCUMENT_LOAD_START");
        const std::uint64_t load_started = clock.milliseconds();
        std::string actual_path = path;
        DocumentProbe document;
        if (!files.probe(actual_path.c_str(), document, error)) {
            if (!error_is_missing(error)) return false;
            std::string lower = actual_path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".tns") == 0) {
                return false;
            }
            actual_path += ".tns";
            if (!files.probe(actual_path.c_str(), document, error)) return false;
        }
        integration_log("DOCUMENT_PROBED");
        loading.stage("Reading file",
                      document.size >= kLargeDocumentFeedbackBytes &&
                          document.size <= options.maximum_source_bytes);
        std::unique_ptr<MarkdownDocument> markdown;
        std::uint64_t new_identity = 0;
        std::vector<std::uint8_t> bytes;
        std::string plain_text_bytes;
        std::shared_ptr<RandomAccessData> plain_text_source;
        std::uint32_t plain_text_content_offset = 0;
        std::uint32_t plain_text_content_size = 0;
        Utf8ValidationResult segmented_validation;
        bool streamed_utf8_text = false;
        bool document_has_cjk = false;
        const bool plain_text = is_plain_text_path(actual_path);
        if (plain_text) {
            if (document.size > options.maximum_source_bytes) {
                error = "file exceeds the configured size limit";
                return false;
            }
            // The Ndless heap cannot reliably satisfy a single multi-megabyte
            // allocation. Admit valid UTF-8 TXT through persistent random
            // access after a bounded encoding probe; PlainTextLayout then
            // shows the first screen from 64 KiB and grows its 1 MiB
            // sequential RAM window while idle. Legacy encodings retain the
            // complete decoder path because their byte offsets change.
            std::string source_error;
            if (files.open_random_access(
                    actual_path.c_str(), plain_text_source, source_error)) {
                if (plain_text_source == nullptr ||
                    plain_text_source->size() != document.size) {
                    error = "plain-text source changed while it was opened";
                    return false;
                }
                const std::size_t requested = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        document.size,
                        kPlainTextEncodingProbeBytes + 4U));
                std::vector<std::uint8_t> probe(requested);
                if (requested != 0 &&
                    !plain_text_source->read(
                        0, probe.data(), probe.size())) {
                    error = "could not read the plain-text encoding probe";
                    return false;
                }
                const std::size_t safe_size =
                    complete_utf8_probe_prefix(
                        probe,
                        std::min<std::size_t>(
                            kPlainTextEncodingProbeBytes, probe.size()));
                std::string decoded_probe;
                TextDecodeInfo probe_info;
                std::string probe_error;
                if (decode_text_auto(
                        probe.data(), safe_size,
                        options.maximum_source_bytes,
                        decoded_probe, probe_info, probe_error) &&
                    probe_info.encoding == TextEncoding::Utf8) {
                    plain_text_content_offset =
                        probe_info.had_bom ? 3U : 0U;
                    if (document.size < plain_text_content_offset ||
                        document.size - plain_text_content_offset >
                            std::numeric_limits<std::uint32_t>::max()) {
                        error = "decoded plain text is too large";
                        return false;
                    }
                    plain_text_content_size = static_cast<std::uint32_t>(
                        document.size - plain_text_content_offset);
                    const std::uint8_t* const validation_bytes =
                        safe_size > plain_text_content_offset
                            ? probe.data() + plain_text_content_offset
                            : nullptr;
                    segmented_validation = utf8_validate(
                        validation_bytes,
                        safe_size - plain_text_content_offset, false);
                    if (!segmented_validation.valid()) {
                        error = "plain text is not valid UTF-8";
                        return false;
                    }
                    new_identity = sampled_document_identity(
                        probe, *plain_text_source, document.size);
                    streamed_utf8_text = true;
                    document_has_cjk = sample_contains_cjk(
                        validation_bytes,
                        safe_size - plain_text_content_offset);
                } else {
                    plain_text_source.reset();
                }
            } else if (!source_error.empty()) {
                error = std::move(source_error);
                return false;
            }
            if (!streamed_utf8_text &&
                !files.read_all_text(actual_path.c_str(),
                                     options.maximum_source_bytes,
                                     plain_text_bytes, error)) {
                return false;
            }
        } else if (!files.read_all(actual_path.c_str(),
                                   options.maximum_source_bytes, bytes,
                                   error)) {
            return false;
        }
        integration_log("DOCUMENT_READ");
        allocation_profile_log("document_read");
        if (plain_text) {
            loading.stage("Checking text encoding");
            if (!streamed_utf8_text) {
                new_identity = document_identity(
                    reinterpret_cast<const std::uint8_t*>(
                        plain_text_bytes.data()),
                    plain_text_bytes.size());
                std::string decoded;
                TextDecodeInfo decode_info;
                if (!decode_text_auto(std::move(plain_text_bytes),
                                      options.maximum_source_bytes, decoded,
                                      decode_info, error)) {
                    return false;
                }
                segmented_validation = utf8_validate(
                    reinterpret_cast<const std::uint8_t*>(decoded.data()),
                    decoded.size(), false);
                if (!segmented_validation.valid() ||
                    decoded.size() >
                        static_cast<std::size_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
                    error = "decoded plain text is too large";
                    return false;
                }
                plain_text_content_offset = 0;
                plain_text_content_size =
                    static_cast<std::uint32_t>(decoded.size());
                // A document that needed a legacy CJK decoder is CJK by
                // definition; a UTF-8 one is sampled.
                document_has_cjk =
                    decode_info.encoding != TextEncoding::Utf8 ||
                    sample_contains_cjk(
                        reinterpret_cast<const std::uint8_t*>(
                            decoded.data()),
                        decoded.size());
                plain_text_source =
                    std::make_shared<MemoryRandomAccessData>(
                        std::move(decoded));
            }
        } else {
            markdown.reset(new MarkdownDocument());
            new_identity = document_identity(bytes.data(), bytes.size());
            loading.stage("Parsing Markdown");
            if (looks_like_binary_document(bytes)) {
                error = "file contains binary data, not Markdown text";
                return false;
            }
            const Utf8ValidationResult source_utf8 =
                utf8_validate(bytes.data(), bytes.size(), true);
            const std::size_t bom_bytes = source_utf8.had_bom ? 3U : 0U;
            const std::size_t retained_bytes = bytes.size() - bom_bytes;
            // Each malformed sequence can expand to a three-byte U+FFFD. Keep
            // the retained, sanitized source within the same admission budget
            // as the raw file so malformed UTF-8 cannot triple RAM use.
            if (retained_bytes > options.maximum_source_bytes ||
                source_utf8.invalid_sequence_count >
                    (options.maximum_source_bytes - retained_bytes) / 2U) {
                error =
                    "sanitized Markdown text exceeds the configured size limit";
                return false;
            }
            document_has_cjk =
                sample_contains_cjk(bytes.data() + bom_bytes, retained_bytes);
            if (!parse_markdown(bytes.data(), bytes.size(), *markdown, error)) {
                return false;
            }
        }
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        constexpr std::uint64_t kExpectedNovelBytes = 2622979ULL;
        constexpr std::uint64_t kExpectedNovelSampledIdentity =
            0x814f6e58b9b74020ULL;
        if (!plain_text || document.size != kExpectedNovelBytes ||
            new_identity != kExpectedNovelSampledIdentity) {
            error = "红楼梦 test asset does not match the required file";
            std::printf(
                "NMARKDOWN_IT/1 REAL_NOVEL_MISMATCH bytes=%llu "
                "identity=%016llx streamed=%d\n",
                static_cast<unsigned long long>(document.size),
                static_cast<unsigned long long>(new_identity),
                streamed_utf8_text ? 1 : 0);
            std::fflush(stdout);
            return false;
        }
        std::printf(
            "NMARKDOWN_IT/1 REAL_NOVEL_EXACT bytes=%llu "
            "identity=%016llx streamed=%d\n",
            static_cast<unsigned long long>(document.size),
            static_cast<unsigned long long>(new_identity),
            streamed_utf8_text ? 1 : 0);
        std::fflush(stdout);
#endif
        integration_log("DOCUMENT_PARSED");
        allocation_profile_log("document_parsed");
#if defined(NMARKDOWN_ALLOCATION_PROBE)
        if (plain_text) {
            std::printf(
                "NMARKDOWN_PERF/1 stage=document_parsed elapsed_ms=%llu "
                "source_bytes=%lu txt_ir=0 global_line_index=0\n",
                static_cast<unsigned long long>(
                    clock.milliseconds() - load_started),
                static_cast<unsigned long>(plain_text_content_size));
        } else {
            std::printf(
                "NMARKDOWN_PERF/1 stage=document_parsed elapsed_ms=%llu "
                "source_bytes=%lu blocks=%lu tokens=%lu md4c_ir=1\n",
                static_cast<unsigned long long>(
                    clock.milliseconds() - load_started),
                static_cast<unsigned long>(markdown->source_size()),
                static_cast<unsigned long>(markdown->ir.blocks.size()),
                static_cast<unsigned long>(markdown->ir.tokens.size()));
        }
        std::fflush(stdout);
#endif
        // Parsing retains sanitized source in MarkdownDocument. Release the
        // raw input before reflow, font shaping, and state restoration begin.
        std::vector<std::uint8_t>().swap(bytes);
        current_document_bytes = document.size;
        current_document_has_cjk = document_has_cjk;
        // A document this large amortizes promoting a still-streamed font
        // into RAM. Upgrading before this document is laid out makes the
        // registry swap's glyph-cache clear and reflow free, and running
        // after the raw input was released leaves the most heap headroom.
        // A payload serving only the CJK role additionally requires actual
        // CJK content.
        if (viewer.resident_font_preload() &&
            !resident_font_promotion_failed &&
            options.maximum_resident_font_bytes != 0 &&
            document.size >= options.minimum_resident_font_document_bytes) {
            const FontRegistryState registry = viewer.font_registry_state();
            const std::size_t cjk_role = static_cast<std::size_t>(
                external_font_role_index(FontRole::Cjk));
            bool promotable_stream = false;
            for (const LoadedExternalFont& font : registry.fonts) {
                if (font.source == nullptr || font.byte_size() == 0 ||
                    font.byte_size() >
                        options.maximum_resident_font_bytes) {
                    continue;
                }
                bool cjk_only = registry.roles[cjk_role] == font.id;
                for (std::size_t role = 0;
                     cjk_only && role < registry.roles.size(); ++role) {
                    if (role != cjk_role &&
                        registry.roles[role] == font.id) {
                        cjk_only = false;
                    }
                }
                if (!cjk_only || current_document_has_cjk) {
                    promotable_stream = true;
                    break;
                }
            }
            if (promotable_stream) {
                std::string upgrade_error;
                if (!apply_font_assignments_safely(current_font_paths, false,
                                                   upgrade_error)) {
                    integration_log("FONT_RESIDENT_UPGRADE_FAILED");
                }
                loading.reattach();
            }
        }
        save_current_state();
        loading.stage("Preparing first page");
        if (plain_text) {
            if (!viewer.set_plain_text_document(
                    plain_text_source, plain_text_content_offset,
                    plain_text_content_size, segmented_validation,
                    document, error)) {
                return false;
            }
        } else if (!viewer.set_markdown_document(
                       std::move(markdown), document, error)) {
            return false;
        }
        viewer.set_document_title(document_label_from_path(actual_path));
        integration_log("DOCUMENT_READY");
        allocation_profile_log("document_ready");
#if defined(NMARKDOWN_ALLOCATION_PROBE)
        std::printf(
            "NMARKDOWN_PERF/1 stage=document_ready elapsed_ms=%llu\n",
            static_cast<unsigned long long>(clock.milliseconds() - load_started));
        std::fflush(stdout);
#endif
        ReaderPerformanceMetrics metrics;
        metrics.document_load_parse_ms = clock.milliseconds() - load_started;
        viewer.set_performance_metrics(metrics);
        identity = new_identity;
        current_path = actual_path;
        current_document_bytes = document.size;
        state_path = actual_path + ".nmdstate";
        std::string state_warning = std::move(pending_state_warning);
        pending_state_warning.clear();
        if (options.persist_state) {
            loading.stage("Restoring reading position");
            std::vector<std::uint8_t> state_bytes;
            std::string state_error;
            ReaderState state;
            DocumentProbe state_probe;
            if (files.probe(state_path.c_str(), state_probe, state_error)) {
                if (!files.read_all(state_path.c_str(), 64U * 1024U,
                                    state_bytes, state_error) ||
                    !decode_reader_state(state_bytes.data(), state_bytes.size(),
                                         state, state_error)) {
                    state_warning = state_error.empty()
                                        ? "saved state could not be restored"
                                        : state_specific_error(state_error);
                } else if (!viewer.apply_reader_state(state, identity)) {
                    state_warning = "saved state belongs to an older version of this document";
                }
            } else if (!error_is_missing(state_error)) {
                state_warning = state_specific_error(state_error);
            }
        }
        if (!state_warning.empty()) {
            viewer.show_message("Saved state warning", state_warning);
        }
        return true;
    };
    const auto load_document_safely = [&](const std::string& path,
                                          std::string& error) -> bool {
    };
    const auto show_document_browser = [&](bool interactive) {
        if (document_file_catalog_ready) {
            integration_log("DOCUMENT_CATALOG_CACHE_HIT");
            viewer.show_document_browser(document_file_catalog,
                                         document_file_catalog_truncated);
            return;
        }
        DelayedLoadingFeedback loading(viewer, display, files, clock,
                                       "Finding documents",
                                       "Scanning My Documents");
        // Scratchpad is an explicit user action. Paint its response before
        // entering the synchronous recursive scan; startup remains
        // transactional and does not gain an intermediate frame.
        if (interactive) loading.checkpoint(true);
        std::vector<std::string> paths;
        std::string error;
        bool truncated = false;
        if (!files.list_reader_documents(options.document_root.c_str(), 256,
                                         paths, error, &truncated)) {
            viewer.show_message("Could not list documents", error);
        } else {
            document_file_catalog = std::move(paths);
            document_file_catalog_truncated = truncated;
            document_file_catalog_ready = true;
            integration_log("DOCUMENT_CATALOG_SCANNED");
            viewer.show_document_browser(document_file_catalog,
                                         document_file_catalog_truncated);
        }
    };
    const auto apply_font_assignments = [&](const RememberedFontPaths& requested,
                                            bool interactive,
                                            std::string& error) {
        // The allocation-profile build treats each role application as a
        // measurement interval. This includes the old/new registry overlap,
        // FreeType/HarfBuzz setup, and the first layout invalidation.
        allocation_stats_begin_checkpoint();
        // Every apply is a fresh promotion opportunity; a failure recorded
        // below only suppresses automatic large-document retries.
        resident_font_promotion_failed = false;
        DelayedLoadingFeedback loading(viewer, display, files, clock,
                                       "Loading fonts",
                                       "Preparing assigned font files");
        if (interactive) loading.checkpoint(true);
        struct PreparedResource {
            FontFaceId id = 0;
            std::string path;
            std::shared_ptr<const std::vector<std::uint8_t>> data;
            std::shared_ptr<RandomAccessData> source;
            std::uint32_t signature = 0;
            std::string label;

            std::size_t byte_size() const {
                if (data != nullptr) return data->size();
                return source == nullptr
                           ? 0
                           : static_cast<std::size_t>(source->size());
            }
        };
        const FontRegistryState current_registry = viewer.font_registry_state();
        std::vector<PreparedResource> prepared;
        RememberedFontPaths actual_paths;
        FontRegistryState next_registry;
        std::array<std::string, kExternalFontRoleCount> labels;
        for (std::size_t index = 0; index < kExternalFontRoleCount; ++index) {
            labels[index] = default_role_label(external_font_role(index));
        }
        std::size_t unique_bytes = 0;
        std::size_t resident_bytes = 0;
        // A payload requested only for the CJK role cannot be exercised by a
        // document without CJK text, so it stays streamed for such
        // documents. Variant roles are applied after the CJK role; path
        // equality (tolerating the .tns suffix) decides sharing up front.
        const std::size_t cjk_role_index = static_cast<std::size_t>(
            external_font_role_index(FontRole::Cjk));
        const auto equivalent_paths = [](const std::string& left,
                                         const std::string& right) {
            return left == right || left + ".tns" == right ||
                   left == right + ".tns";
        };
        bool cjk_path_serves_other_role = false;
        if (!requested[cjk_role_index].empty()) {
            for (std::size_t index = 0; index < kExternalFontRoleCount;
                 ++index) {
                if (index != cjk_role_index &&
                    !requested[index].empty() &&
                    equivalent_paths(requested[index],
                                     requested[cjk_role_index])) {
                    cjk_path_serves_other_role = true;
                }
            }
        }

        for (std::size_t role_index = 0;
             role_index < kExternalFontRoleCount; ++role_index) {
            if (requested[role_index].empty()) continue;
            std::string actual_path = requested[role_index];
            DocumentProbe probe;
            if (!files.probe(actual_path.c_str(), probe, error)) {
                if (!error_is_missing(error)) {
                    error = font_specific_error(std::move(error));
                    return false;
                }
                std::string lower = actual_path;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char value) {
                                   return static_cast<char>(std::tolower(value));
                               });
                if (lower.size() >= 4 &&
                    lower.compare(lower.size() - 4, 4, ".tns") == 0) {
                    error = font_specific_error(std::move(error));
                    return false;
                }
                actual_path += ".tns";
                if (!files.probe(actual_path.c_str(), probe, error)) {
                    error = font_specific_error(std::move(error));
                    return false;
                }
            }

            auto resource = std::find_if(
                prepared.begin(), prepared.end(),
                [&actual_path](const PreparedResource& item) {
                    return item.path == actual_path;
                });
            if (resource == prepared.end()) {
                if (probe.size > options.maximum_font_bytes ||
                    probe.size > options.maximum_external_font_bytes ||
                    unique_bytes > options.maximum_external_font_bytes -
                                       static_cast<std::size_t>(probe.size)) {
                    error = "external fonts exceed the configured total size limit";
                    return false;
                }

                PreparedResource loaded;
                loaded.path = actual_path;
                const auto old_resource = std::find_if(
                    runtime_font_resources.begin(),
                    runtime_font_resources.end(),
                    [&actual_path](const RuntimeFontResource& item) {
                        return item.path == actual_path;
                    });
                if (old_resource != runtime_font_resources.end()) {
                    const auto old_font = std::find_if(
                        current_registry.fonts.begin(),
                        current_registry.fonts.end(),
                        [&old_resource](const LoadedExternalFont& item) {
                            return item.id == old_resource->id;
                        });
                    if (old_font != current_registry.fonts.end()) {
                        loaded.id = old_resource->id;
                        loaded.data = old_font->data;
                        loaded.source = old_font->source;
                        loaded.signature = old_font->signature;
                    }
                }
                if (loaded.data == nullptr && loaded.source == nullptr) {
                    loading.stage("Reading " + font_label_from_path(actual_path),
                                  probe.size >= kLargeFontFeedbackBytes);
                    std::shared_ptr<RandomAccessData> source;
                    std::string stream_error;
                    if (files.open_random_access(actual_path.c_str(), source,
                                                 stream_error)) {
                        if (source == nullptr || source->size() != probe.size) {
                            error = "font changed while it was being opened";
                            return false;
                        }
                        loaded.source = std::move(source);
                    } else {
                        if (!stream_error.empty()) {
                            error = font_specific_error(
                                std::move(stream_error));
                            return false;
                        }
                        // Compatibility path for deterministic/mock adapters
                        // that cannot retain a random-access handle.
                        std::vector<std::uint8_t> bytes;
                        if (!files.read_all(actual_path.c_str(),
                                            options.maximum_font_bytes, bytes,
                                            error)) {
                            error = font_specific_error(std::move(error));
                            return false;
                        }
                        loaded.data =
                            std::make_shared<const std::vector<std::uint8_t>>(
                                std::move(bytes));
                    }
                    loaded.id = next_runtime_font_id++;
                    loaded.signature = probe.sample_hash ^
                        static_cast<std::uint32_t>(probe.size) ^
                        static_cast<std::uint32_t>(probe.size >> 32U);
                    if (loaded.signature == 0) loaded.signature = 1;
                }
                // Best-effort promotion of any still-streamed payload —
                // freshly opened or reused from the previous registry — into
                // a RAM-resident buffer. One sequential read now removes
                // every per-glyph storage seek later, so it is attempted
                // only when the target document is large enough to amortize
                // it. Any failure — over budget, allocation, short read, or
                // a heap too tight to keep the reader's working-set reserve
                // — silently keeps the already-open stream.
                if (loaded.source != nullptr && loaded.data == nullptr) {
                    const std::size_t payload_bytes =
                        static_cast<std::size_t>(probe.size);
                    const bool fits_budget =
                        payload_bytes != 0 &&
                        payload_bytes <=
                            options.maximum_resident_font_bytes &&
                        resident_bytes <=
                            options.maximum_resident_font_bytes -
                                payload_bytes;
                    const bool document_amortizes_read =
                        current_document_bytes >=
                        options.minimum_resident_font_document_bytes;
                    const bool role_needed_by_document =
                        role_index != cjk_role_index ||
                        cjk_path_serves_other_role ||
                        current_document_has_cjk;
                    if (viewer.resident_font_preload() && fits_budget &&
                        document_amortizes_read &&
                        role_needed_by_document) {
                        loading.stage(
                            "Loading " +
                                font_label_from_path(actual_path) +
                                " into memory",
                            probe.size >= kLargeFontFeedbackBytes);
                        try {
                            std::vector<std::uint8_t> payload;
                            std::string resident_error;
                            if (files.read_all(
                                    actual_path.c_str(),
                                    options.maximum_font_bytes,
                                    payload, resident_error) &&
                                payload.size() == payload_bytes &&
                                heap_reserve_available()) {
                                loaded.data = std::make_shared<
                                    const std::vector<std::uint8_t>>(
                                    std::move(payload));
                                loaded.source.reset();
                            }
                        } catch (const std::bad_alloc&) {
                            // Promotion must never turn a working streamed
                            // font into an apply failure.
                        }
                        if (loaded.data == nullptr) {
                            resident_font_promotion_failed = true;
                        }
                    }
                    if (loaded.data != nullptr) {
                        integration_log("FONT_RESOURCE_RESIDENT");
                    } else {
                        integration_log("FONT_RESOURCE_STREAMED");
                    }
                }
                if (loaded.data != nullptr) {
                    resident_bytes += loaded.data->size();
                }
                unique_bytes += loaded.byte_size();
                if (unique_bytes > options.maximum_external_font_bytes) {
                    error = "external fonts exceed the configured total size limit";
                    return false;
                }
                FontFaceCatalogEntry metadata;
                std::string metadata_error;
                loaded.label = inspect_font_face(files, actual_path, metadata,
                                                 metadata_error)
                                   ? catalog_font_label(metadata, actual_path)
                                   : font_label_from_path(actual_path);
                prepared.push_back(std::move(loaded));
                resource = prepared.end() - 1;
            }
            actual_paths[role_index] = actual_path;
            next_registry.roles[role_index] = resource->id;
            labels[role_index] = resource->label;
        }

        next_registry.fonts.reserve(prepared.size());
        std::vector<RuntimeFontResource> next_runtime_resources;
        next_runtime_resources.reserve(prepared.size());
        for (const PreparedResource& resource : prepared) {
            next_registry.fonts.push_back(
                {resource.id, resource.data, resource.source,
                 resource.signature});
            next_runtime_resources.push_back({resource.id, resource.path});
        }
        loading.stage("Applying roles and layout", interactive);
        if (!viewer.set_font_registry(std::move(next_registry), labels, error)) {
            error = font_specific_error(std::move(error));
            return false;
        }
        current_font_paths = std::move(actual_paths);
        runtime_font_resources = std::move(next_runtime_resources);
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
        std::printf("NMARKDOWN_IT/1 FONT_REGISTRY_READY resources=%lu mono=%lu cjk=%lu\n",
                    static_cast<unsigned long>(viewer.external_font_count()),
                    static_cast<unsigned long>(
                        viewer.external_font_id(FontRole::Monospace)),
                    static_cast<unsigned long>(
                        viewer.external_font_id(FontRole::Cjk)));
        std::fflush(stdout);
#endif
        return true;
    };
    apply_font_assignments_safely =
        [&](const RememberedFontPaths& requested,
            bool interactive,
            std::string& error) {
            bool applied = false;
            try {
                applied = apply_font_assignments(
                    requested, interactive, error);
            } catch (const std::bad_alloc&) {
                error = "not enough free memory to apply the selected fonts";
                applied = false;
            }
#if defined(NMARKDOWN_ALLOCATION_PROBE)
            const AllocationStats stats = allocation_stats();
            std::printf(
                "NMARKDOWN_MEMORY/1 font_apply=%s current=%llu "
                "lifetime_peak=%llu font_peak=%llu allocations=%llu "
                "failures=%llu tracking_overflows=%llu\n",
                applied ? "ok" : "failed",
                static_cast<unsigned long long>(stats.current_bytes),
                static_cast<unsigned long long>(stats.lifetime_peak_bytes),
                static_cast<unsigned long long>(stats.checkpoint_peak_bytes),
                static_cast<unsigned long long>(stats.allocation_count),
                static_cast<unsigned long long>(stats.allocation_failures),
                static_cast<unsigned long long>(stats.tracking_overflows));
            std::fflush(stdout);
#endif
            return applied;
        };
    const auto save_font_preferences = [&](std::string& error) {
        if (!options.persist_state) return true;
        std::vector<std::uint8_t> bytes;
        if (!encode_font_preferences(current_font_paths, bytes, error)) {
            return false;
        }
        if (!files.write_atomic(remembered_font_path.c_str(), bytes.data(),
                                bytes.size(), error)) {
            if (error.empty()) error = "could not write font preferences";
            return false;
        }
        return true;
    };
    const auto show_font_manager = [&]() {
        DelayedLoadingFeedback loading(viewer, display, files, clock,
                                       "Finding fonts",
                                       "Scanning My Documents");
        std::string error;
        if (!font_file_catalog_ready) {
            std::vector<std::string> paths;
            bool truncated = false;
            if (!files.list_font_files(options.document_root.c_str(), 128,
                                       paths, error, &truncated)) {
                viewer.show_message("Could not list fonts", error);
                return;
            }
            loading.stage("Reading font information");
            font_file_catalog.clear();
            font_file_catalog.reserve(paths.size());
            for (const std::string& path : paths) {
                FontFaceCatalogEntry face;
                std::string metadata_error;
                if (inspect_font_face(files, path, face, metadata_error)) {
                    font_file_catalog.push_back(std::move(face));
                }
                loading.checkpoint();
            }
            std::sort(font_file_catalog.begin(), font_file_catalog.end(),
                      [](const FontFaceCatalogEntry& left,
                         const FontFaceCatalogEntry& right) {
                          const std::string left_label =
                              catalog_font_label(left, left.path);
                          const std::string right_label =
                              catalog_font_label(right, right.path);
                          return left_label == right_label
                                     ? left.path < right.path
                                     : left_label < right_label;
                      });
            font_file_catalog_truncated = truncated;
            font_file_catalog_ready = true;
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
            integration_log("FONT_CATALOG_SCANNED");
#endif
        } else {
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
            integration_log("FONT_CATALOG_CACHE_HIT");
#endif
        }
        viewer.show_font_manager(font_file_catalog, current_font_paths,
                                 font_file_catalog_truncated);
#if defined(NMARKDOWN_FIREBIRD_FONT_MENU_FIXTURE)
            integration_log("FONT_MANAGER_READY");
#endif
    };
    bool startup_ready = true;
    RememberedFontPaths startup_font_paths;
    bool restored_font_preferences = false;
    if (options.persist_state) {
        std::vector<std::uint8_t> preference_bytes;
        std::string preference_error;
        RememberedFontPaths saved_paths;
        if (files.read_all(remembered_font_path.c_str(),
                           kRememberedFontRoleCount *
                                   (kMaximumRememberedFontPath + 2U) + 8U,
                           preference_bytes, preference_error) &&
            decode_font_preferences(preference_bytes.data(),
                                    preference_bytes.size(), saved_paths)) {
            startup_font_paths = std::move(saved_paths);
            restored_font_preferences = true;
        }
    }
    // Migrate the old standalone CJK preference only when a newer role map
    // does not already assign the CJK role.
    if (options.persist_state && startup_font_paths[3].empty()) {
        std::vector<std::uint8_t> preference_bytes;
        std::string preference_error;
        std::string saved_path;
        if (files.read_all(remembered_cjk_font_path.c_str(),
                           kMaximumRememberedFontPath + 16U,
                           preference_bytes, preference_error) &&
            decode_cjk_font_preference(preference_bytes.data(),
                                       preference_bytes.size(), saved_path) &&
            !saved_path.empty()) {
            startup_font_paths[3] = std::move(saved_path);
        }
    }
    // Validate remembered paths independently so one moved or corrupt font
    // does not discard unrelated working assignments. This reads metadata
    // ranges only; payload loading remains deduplicated below.
    if (options.persist_state) {
        bool dropped_stale_path = false;
        for (std::string& path : startup_font_paths) {
            if (path.empty()) continue;
            std::string actual_path = path;
            DocumentProbe probe;
            std::string probe_error;
            if (!files.probe(actual_path.c_str(), probe, probe_error) &&
                error_is_missing(probe_error)) {
                actual_path += ".tns";
            }
            FontFaceCatalogEntry metadata;
            std::string metadata_error;
            if (!files.probe(actual_path.c_str(), probe, probe_error) ||
                !inspect_font_face(files, actual_path, metadata,
                                   metadata_error)) {
                path.clear();
                dropped_stale_path = true;
            } else {
                path = std::move(actual_path);
            }
        }
        if (dropped_stale_path) integration_log("FONT_PREFERENCE_STALE");
    }
    // Explicit launch options override the stored role assignments.
    if (!options.initial_body_font_path.empty()) {
        startup_font_paths[0] = options.initial_body_font_path;
    }
    if (!options.initial_body_italic_font_path.empty()) {
        startup_font_paths[1] = options.initial_body_italic_font_path;
    }
    if (!options.initial_monospace_font_path.empty()) {
        startup_font_paths[2] = options.initial_monospace_font_path;
    }
    if (!options.initial_cjk_font_path.empty()) {
        startup_font_paths[3] = options.initial_cjk_font_path;
    }
    const bool has_startup_fonts = std::any_of(
        startup_font_paths.begin(), startup_font_paths.end(),
        [](const std::string& path) { return !path.empty(); });
    // Restore the global preload switch before any promotion decision runs.
    // A missing or invalid preference file keeps the default (enabled).
    if (options.persist_state) {
        std::vector<std::uint8_t> preload_bytes;
        std::string preload_error;
        bool preload_enabled = true;
        if (files.read_all(remembered_font_preload_path.c_str(), 64U,
                           preload_bytes, preload_error) &&
            decode_font_preload_preference(preload_bytes.data(),
                                           preload_bytes.size(),
                                           preload_enabled)) {
            viewer.set_resident_font_preload(preload_enabled);
        }
    }
    // The size of the startup document decides whether resident-font
    // promotion is worth its one-time sequential read during the font apply
    // below. A missing or unspecified document leaves the size at zero, so
    // fonts stream until a large document is actually opened.
    if (document_path != nullptr && document_path[0] != '\0') {
        DocumentProbe startup_probe;
        std::string startup_probe_error;
        std::string startup_document_path = document_path;
        if (!files.probe(startup_document_path.c_str(), startup_probe,
                         startup_probe_error)) {
            startup_document_path += ".tns";
            if (!files.probe(startup_document_path.c_str(), startup_probe,
                             startup_probe_error)) {
                startup_probe.size = 0;
            }
        }
        current_document_bytes = startup_probe.size;
        // One bounded content sample decides whether a remembered CJK-role
        // font is worth promoting during the apply below. Read it only when
        // that decision is actually on the table.
        if (viewer.resident_font_preload() &&
            current_document_bytes >=
                options.minimum_resident_font_document_bytes &&
            options.maximum_resident_font_bytes != 0 &&
            !startup_font_paths[static_cast<std::size_t>(
                 external_font_role_index(FontRole::Cjk))].empty()) {
            current_document_has_cjk = sampled_document_contains_cjk(
                files, startup_document_path);
        }
    }
    // Resolve remembered roles before opening the document. Large UTF-8 TXT
    // uses a bounded sequential cache rather than a giant source allocation;
    // applying CJK first also avoids showing a stale "CJK font needed" dialog
    // over the correctly rendered first frame.
    if (startup_ready && has_startup_fonts) {
        std::string error;
        if (apply_font_assignments_safely(
                startup_font_paths, false, error)) {
            if (restored_font_preferences) {
                integration_log("FONT_PREFERENCE_READY");
            } else {
                integration_log("FONT_READY");
            }
        } else if (!options.initial_body_font_path.empty() ||
                   !options.initial_body_italic_font_path.empty() ||
                   !options.initial_monospace_font_path.empty() ||
                   !options.initial_cjk_font_path.empty()) {
            startup_ready = false;
            integration_log("FONT_FAIL");
            viewer.set_document_error(error);
        } else {
            current_font_paths = {};
            runtime_font_resources.clear();
            integration_log("FONT_PREFERENCE_STALE");
        }
    }
    if (startup_ready && document_path != nullptr && document_path[0] != '\0') {
        std::string error;
        const std::string startup_path(document_path);
        if (!load_document_safely(startup_path, error)) {
            integration_error("DOCUMENT_FAIL", error);
            viewer.set_document_title(document_label_from_path(document_path));
            viewer.set_document_error(error);
        }
    } else if (startup_ready && options.open_browser_on_empty_path) {
        show_document_browser(false);
    }

#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    unsigned real_novel_swipe_requests = 0;
    unsigned real_novel_page_commits = 0;
    unsigned real_novel_presented_commits = 0;
    std::uint32_t real_novel_last_offset = 0;
    int real_novel_last_page = 0;
    bool real_novel_progress_ready = false;
    bool real_novel_progress_failed = false;
    bool real_novel_pass_reported = false;
#endif

    const auto render_and_present = [&]() {
        integration_log("RENDER_START");
        const std::uint64_t render_started = clock.milliseconds();
        viewer.render(display.surface());
        integration_log("RENDER_DONE");
        const std::uint64_t render_finished = clock.milliseconds();
        display.present();
        integration_log("PRESENTED");
        allocation_profile_log("presented");
        const std::uint64_t present_finished = clock.milliseconds();
        ReaderPerformanceMetrics metrics = viewer.performance_metrics();
        metrics.last_visible_render_ms = render_finished - render_started;
        if (metrics.first_visible_render_ms == 0) {
            metrics.first_visible_render_ms = metrics.last_visible_render_ms;
        }
        metrics.peak_visible_render_ms = std::max(
            metrics.peak_visible_render_ms, metrics.last_visible_render_ms);
        metrics.last_present_ms = present_finished - render_finished;
        metrics.peak_present_ms = std::max(metrics.peak_present_ms,
                                           metrics.last_present_ms);
        viewer.set_performance_metrics(metrics);
        allocation_profile_log("post_present");
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        if (real_novel_progress_ready &&
            real_novel_presented_commits < real_novel_page_commits) {
            real_novel_presented_commits = real_novel_page_commits;
            std::printf(
                "NMARKDOWN_IT/1 REAL_NOVEL_VISIBLE count=%u offset=%lu "
                "page=%d\n",
                real_novel_presented_commits,
                static_cast<unsigned long>(real_novel_last_offset),
                real_novel_last_page);
            if (!real_novel_progress_failed &&
                !real_novel_pass_reported &&
                real_novel_swipe_requests >= 30U &&
                real_novel_page_commits >= 30U &&
                real_novel_presented_commits >= 30U) {
                std::printf(
                    "NMARKDOWN_IT/1 REAL_NOVEL_30_SWIPE_PASS "
                    "requests=%u commits=%u presented=%u offset=%lu "
                    "page=%d\n",
                    real_novel_swipe_requests,
                    real_novel_page_commits,
                    real_novel_presented_commits,
                    static_cast<unsigned long>(real_novel_last_offset),
                    real_novel_last_page);
                real_novel_pass_reported = true;
            }
            std::fflush(stdout);
        }
#endif
    };

    render_and_present();
    viewer.clear_dirty();
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    real_novel_last_offset =
        viewer.reader_state(identity).position.source_offset;
    real_novel_last_page = viewer.current_page();
    real_novel_progress_ready = true;
    std::printf(
        "NMARKDOWN_IT/1 REAL_NOVEL_PROGRESS_START offset=%lu page=%d\n",
        static_cast<unsigned long>(real_novel_last_offset),
        real_novel_last_page);
    std::fflush(stdout);
#endif

    bool state_save_warning_shown = false;
    bool exit_after_state_save_warning = false;
    bool exit_state_save_attempted = false;
    const auto save_state_with_feedback = [&](bool exit_requested) {
        if (exit_requested) exit_state_save_attempted = true;
        pending_state_warning.clear();
        save_current_state();
        if (pending_state_warning.empty()) return false;
        const std::string warning = std::move(pending_state_warning);
        pending_state_warning.clear();
        if (state_save_warning_shown) return false;
        state_save_warning_shown = true;
        exit_after_state_save_warning = exit_requested;
        viewer.show_message("State not saved", warning);
        return true;
    };

    while (!viewer.quit_requested()) {
        InputEvent event;
        bool changed = false;
        if (input.poll(event)) {
#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
            if (event.type == InputEventType::SwipeDown &&
                viewer.reading_mode() == ReadingMode::HorizontalScroll &&
                viewer.natural_swiping()) {
                ++real_novel_swipe_requests;
                std::printf(
                    "NMARKDOWN_IT/1 REAL_NOVEL_SWIPE_REQUEST count=%u\n",
                    real_novel_swipe_requests);
                std::fflush(stdout);
            }
#endif
            const bool complete_deferred_exit =
                exit_after_state_save_warning &&
                (event.type == InputEventType::Back ||
                 event.type == InputEventType::Activate);
            const bool was_dark = viewer.dark_theme();
            changed = viewer.handle_event(event);
            if (event.type == InputEventType::Quit) {
                // Ctrl+Esc always exits immediately, including through an
                // exit-time save warning.
                exit_after_state_save_warning = false;
            } else if (complete_deferred_exit) {
                // Back/Enter dismisses the warning and completes the original
                // exit request. Requiring a third key here looks like a hang on
                // the calculator because the document simply reappears.
                exit_after_state_save_warning = false;
                changed = viewer.handle_event({InputEventType::Quit, 0}) ||
                          changed;
            }
            if (viewer.dark_theme() != was_dark) {
                if (viewer.dark_theme()) {
                    integration_log("THEME_DARK");
                } else {
                    integration_log("THEME_LIGHT");
                }
            }
            if (viewer.take_state_save_request()) {
                changed = save_state_with_feedback(false) || changed;
            }
            if (viewer.take_font_preload_save_request() &&
                options.persist_state) {
                std::vector<std::uint8_t> preload_bytes;
                encode_font_preload_preference(
                    viewer.resident_font_preload(), preload_bytes);
                std::string preload_error;
                if (!files.write_atomic(
                        remembered_font_preload_path.c_str(),
                        preload_bytes.data(), preload_bytes.size(),
                        preload_error)) {
                    viewer.show_message(
                        "Setting not saved",
                        preload_error.empty()
                            ? "could not write the font preload preference"
                            : preload_error);
                    changed = true;
                }
            }
            if (viewer.take_document_browser_request()) {
                show_document_browser(true);
                changed = true;
            }
            std::string target;
            if (viewer.take_document_open_request(target)) {
                std::string error;
                if (!load_document_safely(target, error)) {
                    viewer.show_message("Could not open document", error);
                }
                changed = true;
            }
            if (viewer.take_font_menu_request()) {
                show_font_manager();
                changed = true;
            }
            RememberedFontPaths font_assignments;
            if (viewer.take_font_assignments(font_assignments)) {
                std::string error;
                if (!apply_font_assignments_safely(
                        font_assignments, true, error)) {
                    viewer.show_message("Could not apply fonts", error);
                } else {
                    if (!save_font_preferences(error)) {
                        viewer.show_message("Fonts not remembered",
                                            font_specific_error(error));
                    }
                }
                changed = true;
            }
            if (viewer.take_document_link_request(target)) {
                const std::size_t fragment_offset = target.find('#');
                const std::string fragment = fragment_offset == std::string::npos
                                                 ? std::string()
                                                 : target.substr(fragment_offset + 1);
                const std::string link_path = fragment_offset == std::string::npos
                                                  ? target
                                                  : target.substr(0, fragment_offset);
                const std::string resolved = local_path_from_link(current_path, link_path);
                if (!is_reader_document_path(resolved)) {
                    viewer.show_message("Linked asset", resolved);
                } else {
                    std::string error;
                    if (!load_document_safely(resolved, error)) {
                        viewer.show_message("Could not open link", error);
                    } else if (!fragment.empty() && !viewer.navigate_to_anchor(fragment)) {
                        viewer.show_message("Link target not found", "#" + fragment);
                    }
                }
                changed = true;
            }
            if (event.type == InputEventType::Back && viewer.quit_requested() &&
                !state_save_warning_shown && !state_save_failure_known &&
                save_state_with_feedback(true)) {
                viewer.cancel_quit_request();
                changed = true;
            }
        } else if (input.interaction_active()) {
            // A held key or an in-progress touch gesture can spend several
            // polls below its repeat/motion threshold. Keep those polls free
            // of NAND reads, shaping, and glyph rasterization.
            clock.sleep_ms(1);
        } else {
            constexpr std::uint64_t kWorkBudgetMs = 2;
            const std::uint64_t deadline = clock.milliseconds() + kWorkBudgetMs;
            const bool did_work =
                viewer.perform_incremental_work(clock, deadline);
            if (!did_work) {
                clock.sleep_ms(4);
            }
        }

#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
        if (real_novel_progress_ready) {
            const std::uint32_t current_offset =
                viewer.reader_state(identity).position.source_offset;
            const int current_page = viewer.current_page();
            if (current_offset != real_novel_last_offset ||
                current_page != real_novel_last_page) {
                const bool strictly_forward =
                    current_offset > real_novel_last_offset &&
                    current_page == real_novel_last_page + 1 &&
                    real_novel_page_commits < real_novel_swipe_requests;
                if (!strictly_forward) {
                    real_novel_progress_failed = true;
                    std::printf(
                        "NMARKDOWN_IT/1 REAL_NOVEL_PROGRESS_FAIL "
                        "requests=%u commits=%u before=%lu after=%lu "
                        "page_before=%d page_after=%d\n",
                        real_novel_swipe_requests,
                        real_novel_page_commits,
                        static_cast<unsigned long>(real_novel_last_offset),
                        static_cast<unsigned long>(current_offset),
                        real_novel_last_page,
                        current_page);
                } else {
                    ++real_novel_page_commits;
                    std::printf(
                        "NMARKDOWN_IT/1 REAL_NOVEL_PAGE_COMMIT count=%u "
                        "before=%lu after=%lu page=%d\n",
                        real_novel_page_commits,
                        static_cast<unsigned long>(real_novel_last_offset),
                        static_cast<unsigned long>(current_offset),
                        current_page);
                }
                std::fflush(stdout);
                real_novel_last_offset = current_offset;
                real_novel_last_page = current_page;
            }
        }
#endif

        if ((changed || viewer.dirty()) && !viewer.quit_requested()) {
            render_and_present();
            viewer.clear_dirty();
        }
    }

    // A normal Esc exit may already have attempted persistence, and a setting
    // change may have exposed the same storage failure earlier in the session.
    // Retrying either known failure while tearing down makes the dismissed
    // warning appear to freeze the application on Ndless.
    if (!exit_state_save_attempted && !state_save_failure_known) {
        save_current_state();
    }

#if defined(NMARKDOWN_FIREBIRD_REAL_NOVEL_FIXTURE)
    if (!real_novel_pass_reported) {
        std::printf(
            "NMARKDOWN_IT/1 REAL_NOVEL_30_SWIPE_INCOMPLETE "
            "requests=%u commits=%u presented=%u failed=%d\n",
            real_novel_swipe_requests,
            real_novel_page_commits,
            real_novel_presented_commits,
            real_novel_progress_failed ? 1 : 0);
        std::fflush(stdout);
    }
#endif

    return 0;
}

}  // namespace nmarkdown
