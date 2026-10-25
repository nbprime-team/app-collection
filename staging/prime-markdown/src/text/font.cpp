#include "nmarkdown/text/font.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "nmarkdown/document/utf8.h"
#include "nmarkdown/io/block_cached_random_access.h"

extern "C" {
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
}

namespace nmarkdown {

int external_font_role_index(FontRole role) {
    switch (role) {
    case FontRole::BodySans: return 0;
    case FontRole::BodySansItalic: return 1;
    case FontRole::Monospace: return 2;
    case FontRole::Cjk: return 3;
    case FontRole::BodySansBold: return 4;
    case FontRole::BodySansBoldItalic: return 5;
    case FontRole::MonospaceItalic: return 6;
    default: return -1;
    }
}

FontRole external_font_role(std::size_t index) {
    static constexpr FontRole kRoles[kExternalFontRoleCount] = {
        FontRole::BodySans,
        FontRole::BodySansItalic,
        FontRole::Monospace,
        FontRole::Cjk,
        FontRole::BodySansBold,
        FontRole::BodySansBoldItalic,
        FontRole::MonospaceItalic,
    };
    return index < kExternalFontRoleCount ? kRoles[index]
                                           : FontRole::Replacement;
}

namespace {

bool set_face_size(FT_Face face, Fx pixel_size) {
    if (face == nullptr || pixel_size <= 0) return false;
    FT_Set_Transform(face, nullptr, nullptr);
    return FT_Set_Char_Size(face, 0, pixel_size, 72, 72) == 0;
}

std::uint16_t bounded_dimension(unsigned long value) {
    return static_cast<std::uint16_t>(std::min<unsigned long>(
        value, std::numeric_limits<std::uint16_t>::max()));
}

constexpr FT_Int32 kLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP |
                                  FT_LOAD_TARGET_NORMAL;
constexpr FT_Int32 kMetricLoadFlags = FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;

}  // namespace

class FreeTypeFontFace::StreamState {
public:
    explicit StreamState(std::shared_ptr<RandomAccessData> input)
        : source(make_block_cached_random_access(std::move(input))) {
        pin_metrics_tables();
        record.size = static_cast<unsigned long>(source->size());
        record.pos = 0;
        record.descriptor.pointer = this;
        record.read = &StreamState::read;
        record.close = &StreamState::close;
    }

    static unsigned long read(FT_Stream stream,
                              unsigned long offset,
                              unsigned char* buffer,
                              unsigned long count) {
        if (stream == nullptr || stream->descriptor.pointer == nullptr) {
            return 0;
        }
        StreamState* state = static_cast<StreamState*>(
            stream->descriptor.pointer);
        if (count == 0) {
            return offset <= state->source->size() ? 0 : 1;
        }
        try {
            for (const PinnedTable& table : state->pinned_tables) {
                if (offset >= table.offset &&
                    count <= table.data.size() &&
                    offset - table.offset <=
                        table.data.size() - count) {
                    std::memcpy(
                        buffer,
                        table.data.data() +
                            static_cast<std::size_t>(
                                offset - table.offset),
                        count);
                    return count;
                }
            }
            return state->source->read(offset, buffer, count) ? count : 0;
        } catch (...) {
            // FreeType invokes this through a C callback. Never allow a C++
            // allocation or adapter exception to cross that ABI boundary.
            return 0;
        }
    }

    static void close(FT_Stream) {}

private:
    struct PinnedTable {
        std::uint64_t offset = 0;
        std::vector<std::uint8_t> data;
    };

    static std::uint16_t read_be16(const std::uint8_t* data) {
        return static_cast<std::uint16_t>(data[0]) << 8U |
               static_cast<std::uint16_t>(data[1]);
    }

    static std::uint32_t read_be32(const std::uint8_t* data) {
        return static_cast<std::uint32_t>(data[0]) << 24U |
               static_cast<std::uint32_t>(data[1]) << 16U |
               static_cast<std::uint32_t>(data[2]) << 8U |
               static_cast<std::uint32_t>(data[3]);
    }

    void pin_metrics_tables() {
        // TrueType's loader streams horizontal and vertical metrics for glyph
        // loads, and the auto-hinter can inspect many glyphs while preparing a
        // script. Retain only hmtx/vmtx; the multi-megabyte glyf table remains
        // block-cached and storage-backed.
        constexpr std::size_t kMaximumTables = 256;
        constexpr std::size_t kMaximumPinnedMetricsBytes =
            256U * 1024U;
        std::array<std::uint8_t, 12> header{};
        if (source == nullptr || source->contiguous_data() != nullptr ||
            source->size() < header.size() ||
            !source->read(0, header.data(), header.size())) {
            return;
        }
        const std::uint32_t signature = read_be32(header.data());
        if (signature != 0x00010000U &&
            signature != 0x4F54544FU &&
            signature != 0x74727565U &&
            signature != 0x74797031U) {
            return;
        }
        const std::size_t table_count = read_be16(header.data() + 4);
        if (table_count == 0 || table_count > kMaximumTables) return;
        std::vector<std::uint8_t> directory;
        try {
            directory.resize(table_count * 16U);
        } catch (const std::bad_alloc&) {
            return;
        }
        if (!source->read(12, directory.data(), directory.size())) {
            return;
        }

        struct TableRange {
            std::uint64_t offset = 0;
            std::size_t size = 0;
        };
        std::array<TableRange, 2> metrics{};
        for (std::size_t index = 0; index < table_count; ++index) {
            const std::uint8_t* entry =
                directory.data() + index * 16U;
            const std::uint32_t tag = read_be32(entry);
            std::size_t slot = metrics.size();
            if (tag == 0x686D7478U) slot = 0;       // hmtx
            else if (tag == 0x766D7478U) slot = 1;  // vmtx
            if (slot == metrics.size()) continue;
            const std::uint64_t offset = read_be32(entry + 8);
            const std::uint64_t size = read_be32(entry + 12);
            if (offset > source->size() ||
                size > source->size() - offset ||
                size > kMaximumPinnedMetricsBytes) {
                continue;
            }
            metrics[slot] = {
                offset, static_cast<std::size_t>(size)};
        }

        std::size_t pinned_bytes = 0;
        try {
            pinned_tables.reserve(metrics.size());
        } catch (const std::bad_alloc&) {
            // Metrics pinning is an optional acceleration. Keep the streamed
            // face usable through the block cache when the heap is tight.
            return;
        }
        for (const TableRange& table : metrics) {
            if (table.size == 0 ||
                table.size >
                    kMaximumPinnedMetricsBytes - pinned_bytes) {
                continue;
            }
            PinnedTable pinned;
            pinned.offset = table.offset;
            try {
                pinned.data.resize(table.size);
            } catch (const std::bad_alloc&) {
                continue;
            }
            if (!source->read(
                    pinned.offset, pinned.data.data(),
                    pinned.data.size())) {
                continue;
            }
            pinned_bytes += pinned.data.size();
            pinned_tables.push_back(std::move(pinned));
        }
    }

    std::shared_ptr<RandomAccessData> source;
    std::vector<PinnedTable> pinned_tables;

public:
    FT_StreamRec record{};
};

FreeTypeFontFace::~FreeTypeFontFace() {
    if (font_ != nullptr) {
        FT_Done_Face(static_cast<FT_Face>(font_));
    }
}

bool FreeTypeFontFace::initialize(void* library,
                                  const FontPackFace& packed_face,
                                  std::string& error) {
    return initialize_memory(library, packed_face.id, packed_face.role,
                             packed_face.font_data, packed_face.font_size,
                             packed_face.ranges, error);
}

bool FreeTypeFontFace::initialize_memory(
    void* library,
    FontFaceId id,
    FontRole role,
    const std::uint8_t* data,
    std::size_t size,
    const std::vector<CodepointRange>& ranges,
    std::string& error) {
    error.clear();
    ranges_.clear();
    font_data_ = nullptr;
    font_size_ = 0;
    italic_design_ = false;
    bold_design_ = false;
    if (font_ != nullptr) {
        FT_Done_Face(static_cast<FT_Face>(font_));
        font_ = nullptr;
    }
    stream_.reset();
    if (data == nullptr || size == 0) {
        error = "font face has no OpenType payload";
        return false;
    }
    if (library == nullptr ||
        size > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max())) {
        error = "font payload is too large";
        return false;
    }

    FT_Face face = nullptr;
    if (FT_New_Memory_Face(static_cast<FT_Library>(library),
                           data,
                           static_cast<FT_Long>(size),
                           0,
                           &face) != 0 ||
        face == nullptr) {
        error = "FreeType rejected the font payload";
        return false;
    }
    font_ = face;
    id_ = id;
    role_ = role;
    font_data_ = data;
    font_size_ = size;
    italic_design_ = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    bold_design_ = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    ranges_ = ranges;
    return true;
}

bool FreeTypeFontFace::initialize(void* library,
                                  const MemoryFontFace& memory_face,
                                  std::string& error) {
    if (memory_face.source == nullptr) {
        return initialize_memory(library, memory_face.id, memory_face.role,
                                 memory_face.data, memory_face.size, {}, error);
    }

    error.clear();
    ranges_.clear();
    font_data_ = nullptr;
    font_size_ = 0;
    italic_design_ = false;
    bold_design_ = false;
    if (font_ != nullptr) {
        FT_Done_Face(static_cast<FT_Face>(font_));
        font_ = nullptr;
    }
    stream_.reset();
    if (library == nullptr || memory_face.id == 0 ||
        memory_face.source->size() == 0 ||
        memory_face.source->size() >
            static_cast<std::uint64_t>(
                std::numeric_limits<unsigned long>::max())) {
        error = "font stream is empty or too large";
        return false;
    }

    std::unique_ptr<StreamState> stream(
        new StreamState(memory_face.source));
    FT_Open_Args arguments{};
    arguments.flags = FT_OPEN_STREAM;
    arguments.stream = &stream->record;
    FT_Face face = nullptr;
    if (FT_Open_Face(static_cast<FT_Library>(library), &arguments, 0, &face) !=
            0 ||
        face == nullptr) {
        error = "FreeType rejected the streamed font payload";
        return false;
    }
    font_ = face;
    id_ = memory_face.id;
    role_ = memory_face.role;
    font_size_ = static_cast<std::size_t>(memory_face.source->size());
    italic_design_ = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    bold_design_ = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    stream_ = std::move(stream);
    return true;
}

bool FreeTypeFontFace::load_sfnt_table(
    std::uint32_t tag,
    std::vector<std::uint8_t>& data) const {
    data.clear();
    FT_Face face = static_cast<FT_Face>(font_);
    if (face == nullptr) return false;
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, static_cast<FT_ULong>(tag), 0, nullptr,
                           &length) != 0 ||
        length == 0 || length > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    data.resize(static_cast<std::size_t>(length));
    FT_ULong read = length;
    if (FT_Load_Sfnt_Table(face, static_cast<FT_ULong>(tag), 0,
                           data.data(), &read) != 0 ||
        read != length) {
        data.clear();
        return false;
    }
    return true;
}

bool FreeTypeFontFace::glyph_for_codepoint(std::uint32_t codepoint,
                                           GlyphId& glyph) const {
    if (font_ == nullptr) {
        return false;
    }
    if (!ranges_.empty()) {
        const auto range = std::lower_bound(
            ranges_.begin(), ranges_.end(), codepoint,
            [](const CodepointRange& item, std::uint32_t value) {
                return item.last < value;
            });
        if (range == ranges_.end() || range->first > codepoint) {
            return false;
        }
    }
    const FT_UInt result = FT_Get_Char_Index(static_cast<FT_Face>(font_), codepoint);
    if (result == 0) return false;
    glyph = result;
    return true;
}

bool FreeTypeFontFace::line_metrics(Fx pixel_size,
                                    FontLineMetrics& metrics) const {
    FT_Face face = static_cast<FT_Face>(font_);
    if (!set_face_size(face, pixel_size)) return false;
    metrics.ascent = static_cast<Fx>(face->size->metrics.ascender);
    metrics.descent = static_cast<Fx>(face->size->metrics.descender);
    metrics.line_gap = std::max<Fx>(
        0, static_cast<Fx>(face->size->metrics.height) -
               (metrics.ascent - metrics.descent));
    return true;
}

bool FreeTypeFontFace::glyph_metrics(GlyphId glyph,
                                     Fx pixel_size,
                                     GlyphMetrics& metrics) const {
    FT_Face face = static_cast<FT_Face>(font_);
    if (!set_face_size(face, pixel_size) ||
        FT_Load_Glyph(face, static_cast<FT_UInt>(glyph), kMetricLoadFlags) != 0) {
        return false;
    }
    const FT_Glyph_Metrics& source = face->glyph->metrics;
    metrics.advance = static_cast<Fx>(face->glyph->advance.x);
    metrics.bearing_x = static_cast<int>(source.horiBearingX >> 6);
    metrics.y_offset = -static_cast<int>((source.horiBearingY + 63) >> 6);
    metrics.width = bounded_dimension(static_cast<unsigned long>(
        std::max<FT_Pos>(0, (source.width + 63) >> 6)));
    metrics.height = bounded_dimension(static_cast<unsigned long>(
        std::max<FT_Pos>(0, (source.height + 63) >> 6)));
    return true;
}

Fx FreeTypeFontFace::kerning(GlyphId left,
                             GlyphId right,
                             Fx pixel_size) const {
    FT_Face face = static_cast<FT_Face>(font_);
    if (!set_face_size(face, pixel_size) || !FT_HAS_KERNING(face)) return 0;
    FT_Vector value{};
    if (FT_Get_Kerning(face, static_cast<FT_UInt>(left),
                       static_cast<FT_UInt>(right), FT_KERNING_DEFAULT,
                       &value) != 0) {
        return 0;
    }
    return static_cast<Fx>(value.x);
}

bool FreeTypeFontFace::rasterize(GlyphId glyph,
                                 Fx pixel_size,
                                 std::uint8_t subpixel_phase,
                                 std::uint8_t render_flags,
                                 GlyphBitmap& bitmap) const {
    bitmap = {};
    FT_Face face = static_cast<FT_Face>(font_);
    if (!set_face_size(face, pixel_size)) return false;
    FT_Vector delta{static_cast<FT_Pos>((subpixel_phase & 3U) * 16U), 0};
    // FreeType's standard 12-degree synthetic oblique, applied to the outline
    // before grayscale rasterization. Unlike shifting completed bitmap rows,
    // this retains fractional edge coverage and lets the renderer report the
    // transformed bitmap's true bearings and bounds.
    FT_Matrix oblique{0x10000L, 0x0366AL, 0, 0x10000L};
    FT_Set_Transform(face,
                     (render_flags & GlyphRenderOblique) != 0 ? &oblique
                                                              : nullptr,
                     &delta);
    const FT_Int32 load_flags =
        kLoadFlags |
        (stream_ != nullptr ? FT_LOAD_NO_AUTOHINT : 0);
    if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyph), load_flags) != 0 ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return false;
    }
    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& source = slot->bitmap;
    bitmap.metrics.advance = static_cast<Fx>(slot->advance.x);
    bitmap.metrics.bearing_x = slot->bitmap_left;
    bitmap.metrics.y_offset = -slot->bitmap_top;
    bitmap.metrics.width = bounded_dimension(source.width);
    bitmap.metrics.height = bounded_dimension(source.rows);
    const std::size_t area = static_cast<std::size_t>(bitmap.metrics.width) *
                             bitmap.metrics.height;
    if (area == 0) {
        return true;
    }
    if (source.pixel_mode != FT_PIXEL_MODE_GRAY || source.buffer == nullptr ||
        source.width != bitmap.metrics.width || source.rows != bitmap.metrics.height) {
        bitmap = {};
        return false;
    }
    bitmap.coverage.assign(area, 0);
    const int pitch = source.pitch;
    const std::size_t stride = static_cast<std::size_t>(pitch < 0 ? -pitch : pitch);
    for (std::size_t row = 0; row < bitmap.metrics.height; ++row) {
        const std::size_t source_row = pitch < 0
                                           ? bitmap.metrics.height - row - 1
                                           : row;
        const std::uint8_t* pixels = source.buffer + source_row * stride;
        std::copy(pixels, pixels + bitmap.metrics.width,
                  bitmap.coverage.begin() + row * bitmap.metrics.width);
    }
    return true;
}

FontCollection::~FontCollection() {
    reset();
}

void FontCollection::reset() {
    faces_.clear();
    bindings_.fill(0);
    if (library_ != nullptr) {
        FT_Done_FreeType(static_cast<FT_Library>(library_));
        library_ = nullptr;
    }
}

bool FontCollection::initialize(const FontPack& pack, std::string& error) {
    return initialize(pack, {}, {}, error);
}

bool FontCollection::initialize(
    const FontPack& pack,
    const std::vector<MemoryFontFace>& memory_faces,
    std::string& error) {
    FontRoleBindings bindings{};
    for (const MemoryFontFace& face : memory_faces) {
        const int index = external_font_role_index(face.role);
        if (index >= 0) {
            bindings[static_cast<std::size_t>(index)] = face.id;
        }
    }
    return initialize(pack, memory_faces, bindings, error);
}

bool FontCollection::initialize(
    const FontPack& pack,
    const std::vector<MemoryFontFace>& memory_faces,
    const FontRoleBindings& bindings,
    std::string& error) {
    reset();
    error.clear();
    if (!pack.valid()) {
        error = "cannot initialize fonts from an invalid pack";
        return false;
    }
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0 || library == nullptr) {
        error = "could not initialize FreeType";
        return false;
    }
    library_ = library;

    // User-selected faces are loaded first so a selected body or monospace
    // face replaces the built-in face for that role. Fallback remains
    // role-aware below, so a CJK face cannot steal ordinary Latin body text.
    for (const MemoryFontFace& memory : memory_faces) {
        const bool valid_memory =
            memory.data != nullptr && memory.size != 0;
        const bool valid_stream =
            memory.source != nullptr && memory.source->size() != 0;
        if (memory.id == 0 || (!valid_memory && !valid_stream) ||
            (valid_memory && valid_stream) || face(memory.id) != nullptr) {
            error = "invalid in-memory font face";
            reset();
            return false;
        }
        std::unique_ptr<FreeTypeFontFace> loaded(new FreeTypeFontFace());
        if (!loaded->initialize(library_, memory, error)) {
            reset();
            return false;
        }
        faces_.push_back(std::move(loaded));
    }

    for (std::size_t index = 0; index < pack.face_count(); ++index) {
        const FontPackFace* packed_face = pack.face(index);
        if (packed_face == nullptr || face(packed_face->id) != nullptr) {
            error = packed_face == nullptr
                        ? "font pack returned a missing face"
                        : "font pack contains duplicate face ids";
            reset();
            return false;
        }
        std::unique_ptr<FreeTypeFontFace> loaded(new FreeTypeFontFace());
        if (!loaded->initialize(library_, *packed_face, error)) {
            reset();
            return false;
        }
        faces_.push_back(std::move(loaded));
    }

    for (FontFaceId id : bindings) {
        if (id != 0 && face(id) == nullptr) {
            error = "font role references an unloaded resource";
            reset();
            return false;
        }
    }
    bindings_ = bindings;

    ResolvedGlyph replacement;
    if (!resolve(kReplacementCodepoint, replacement)) {
        error = "font pack does not contain the required replacement glyph";
        reset();
        return false;
    }
    return true;
}

const FontFace* FontCollection::face(FontFaceId id) const {
    for (const std::unique_ptr<FreeTypeFontFace>& item : faces_) {
        if (item->id() == id) {
            return item.get();
        }
    }
    return nullptr;
}

const FontFace* FontCollection::primary_face(FontRole preferred_role) const {
    if (preferred_role == FontRole::MathText) preferred_role = FontRole::Math;
    const FontFaceId assigned = assigned_face(preferred_role);
    if (assigned != 0) return face(assigned);
    if (preferred_role == FontRole::BodySansItalic ||
        preferred_role == FontRole::BodySansBold ||
        preferred_role == FontRole::BodySansBoldItalic) {
        return primary_face(FontRole::BodySans);
    }
    if (preferred_role == FontRole::MonospaceItalic) {
        return primary_face(FontRole::Monospace);
    }
    for (const std::unique_ptr<FreeTypeFontFace>& item : faces_) {
        if (item->role() == preferred_role) return item.get();
    }
    if (preferred_role == FontRole::Monospace ||
        preferred_role == FontRole::Cjk) {
        for (const std::unique_ptr<FreeTypeFontFace>& item : faces_) {
            if (item->role() == FontRole::BodySans) return item.get();
        }
    }
    return faces_.empty() ? nullptr : faces_.front().get();
}

FontFaceId FontCollection::assigned_face(FontRole role) const {
    const int index = external_font_role_index(role);
    return index < 0 ? 0 : bindings_[static_cast<std::size_t>(index)];
}

bool FontCollection::resolve(std::uint32_t codepoint,
                             ResolvedGlyph& result,
                             FontRole preferred_role) const {
    result = {};
    const auto find_assigned = [&](FontRole role,
                                   std::uint32_t requested,
                                   bool substituted) {
        const FontFaceId assigned = assigned_face(role);
        if (assigned == 0) return false;
        const FontFace* item = face(assigned);
        GlyphId glyph = 0;
        if (item != nullptr && item->glyph_for_codepoint(requested, glyph)) {
            result = {item, glyph, requested, role, substituted};
            return true;
        }
        return false;
    };
    const auto find_builtin = [&](FontRole role,
                                  std::uint32_t requested,
                                  bool substituted) {
        for (const std::unique_ptr<FreeTypeFontFace>& item : faces_) {
            if (item->role() != role) continue;
            GlyphId glyph = 0;
            if (item->glyph_for_codepoint(requested, glyph)) {
                result = {item.get(), glyph, requested, role, substituted};
                return true;
            }
        }
        return false;
    };
    const auto try_chain = [&](std::uint32_t requested, bool substituted) {
        if (preferred_role == FontRole::MathText) {
            // Formula prose keeps Latin glyphs in Latin Modern Math while
            // still allowing a separately selected CJK face for unsupported
            // scripts. This differs from the deliberately closed Math role.
            return find_builtin(FontRole::Math, requested, substituted) ||
                   find_assigned(FontRole::Cjk, requested, substituted) ||
                   find_assigned(FontRole::BodySans, requested, substituted) ||
                   find_builtin(FontRole::BodySans, requested, substituted);
        }
        if (preferred_role == FontRole::BodySans ||
            preferred_role == FontRole::BodySansItalic ||
            preferred_role == FontRole::BodySansBold ||
            preferred_role == FontRole::BodySansBoldItalic) {
            // Body uses its assigned face, then the built-in ASCII face, then
            // the CJK fallback. Keeping the generic CJK role last prevents a
            // multifunction CJK font from taking over ordinary Latin text.
            if (find_assigned(preferred_role, requested, substituted)) {
                return true;
            }
            if (preferred_role != FontRole::BodySans &&
                find_assigned(FontRole::BodySans, requested, substituted)) {
                return true;
            }
            return find_builtin(FontRole::BodySans, requested, substituted) ||
                   find_assigned(FontRole::Cjk, requested, substituted);
        }
        if (preferred_role == FontRole::Monospace ||
            preferred_role == FontRole::MonospaceItalic) {
            // Code uses its assigned face, then the embedded minimal DejaVu
            // Sans Mono face, CJK, and finally the UI replacement fallback.
            if (find_assigned(preferred_role, requested, substituted)) {
                return true;
            }
            if (preferred_role == FontRole::MonospaceItalic &&
                find_assigned(FontRole::Monospace, requested, substituted)) {
                return true;
            }
            return find_builtin(FontRole::Monospace, requested, substituted) ||
                   find_assigned(FontRole::Cjk, requested, substituted) ||
                   find_builtin(FontRole::BodySans, requested, substituted);
        }
        if (preferred_role == FontRole::Cjk) {
            return find_assigned(FontRole::Cjk, requested, substituted) ||
                   find_builtin(FontRole::BodySans, requested, substituted);
        }
        return find_assigned(preferred_role, requested, substituted) ||
               find_builtin(preferred_role, requested, substituted) ||
               find_builtin(FontRole::BodySans, requested, substituted);
    };

    // Formula layout is intentionally a closed Latin Modern Math domain. Falling
    // through to the selected body/CJK faces would reintroduce mixed metrics
    // and mismatched symbol designs inside one formula. Latin Modern Math does
    // not encode U+FFFD, so unsupported formula codepoints use that face's
    // .notdef glyph instead of borrowing a replacement from another font.
    if (preferred_role == FontRole::Math) {
        if (find_builtin(FontRole::Math, codepoint, false)) return true;
        const FontFace* math = primary_face(FontRole::Math);
        if (math == nullptr || math->role() != FontRole::Math) return false;
        result = {math, 0, codepoint, FontRole::Math, true};
        return true;
    }

    if (try_chain(codepoint, false)) return true;

    if (codepoint == kReplacementCodepoint) {
        const FontFace* math = primary_face(FontRole::Math);
        if (math == nullptr || math->role() != FontRole::Math) return false;
        result = {math, 0, codepoint, FontRole::Math, true};
        return true;
    }
    if (try_chain(kReplacementCodepoint, true)) return true;
    const FontFace* math = primary_face(FontRole::Math);
    if (math == nullptr || math->role() != FontRole::Math) return false;
    result = {math, 0, codepoint, FontRole::Math, true};
    return true;
}

}  // namespace nmarkdown
