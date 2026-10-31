#ifndef NMARKDOWN_TEXT_FONT_CATALOG_H
#define NMARKDOWN_TEXT_FONT_CATALOG_H

#include <cstdint>
#include <string>
#include <vector>

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

struct FontFaceCatalogEntry {
    std::string path;
    std::string family;
    std::string subfamily;
    std::uint16_t weight = 400;
    std::uint16_t width = 5;
    bool italic = false;
    bool bold = false;
    bool fixed_pitch = false;
    bool has_latin = false;
    bool has_cjk = false;
    bool variable = false;
};

// Reads only SFNT directory/name/style/cmap metadata through read_range(). It
// does not read the glyph payload and therefore remains cheap for large CJK
// fonts. Collections are deliberately not exposed until a face index can be
// represented in the persisted selection format.
bool inspect_font_face(FileSystem& files,
                       const std::string& path,
                       FontFaceCatalogEntry& face,
                       std::string& error);

}  // namespace nmarkdown

#endif
