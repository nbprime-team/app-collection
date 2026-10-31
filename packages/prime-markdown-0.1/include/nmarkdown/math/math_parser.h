#ifndef NMARKDOWN_MATH_MATH_PARSER_H
#define NMARKDOWN_MATH_MATH_PARSER_H

#include <string_view>

#include "nmarkdown/math/math_atoms.h"

namespace nmarkdown {

// Returns false only when a hard resource limit prevents parsing. Syntax
// errors are recovered locally and reported through MathTree::diagnostic.
bool parse_math(std::string_view source, MathTree& tree);

}  // namespace nmarkdown

#endif
