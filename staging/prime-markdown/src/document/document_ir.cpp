#include "nmarkdown/document/document_ir.h"

namespace nmarkdown {

void DocumentIR::clear() {
    blocks.clear();
    tokens.clear();
    headings.clear();
    links.clear();
    string_arena.clear();
    first_block = kInvalidNode;
}

TextRef DocumentIR::own(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const std::size_t offset = string_arena.size();
    string_arena.append(value.data(), value.size());
    return {static_cast<std::uint32_t>(offset),
            static_cast<std::uint32_t>(value.size()),
            TextStorageKind::Owned};
}

std::string_view DocumentIR::text(TextRef ref, std::string_view source) const {
    if (ref.storage == TextStorageKind::Source && ref.offset <= source.size() &&
        ref.length <= source.size() - ref.offset) {
        return source.substr(ref.offset, ref.length);
    }
    if (ref.storage == TextStorageKind::Owned && ref.offset <= string_arena.size() &&
        ref.length <= string_arena.size() - ref.offset) {
        return std::string_view(string_arena.data() + ref.offset, ref.length);
    }
    return {};
}

}  // namespace nmarkdown

