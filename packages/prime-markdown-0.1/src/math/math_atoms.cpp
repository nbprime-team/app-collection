#include "nmarkdown/math/math_atoms.h"

namespace nmarkdown {

void MathTree::clear() {
    nodes.clear();
    children.clear();
    strings.clear();
    root = kInvalidMathNode;
    recovered_error = false;
    diagnostic.clear();
    diagnostic_offset = 0;
}

std::string_view MathTree::text(const MathNode& node) const {
    if (node.text_offset > strings.size() ||
        node.text_length > strings.size() - node.text_offset) {
        return {};
    }
    return std::string_view(strings.data() + node.text_offset, node.text_length);
}

}  // namespace nmarkdown
