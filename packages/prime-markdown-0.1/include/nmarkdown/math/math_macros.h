#ifndef NMARKDOWN_MATH_MATH_MACROS_H
#define NMARKDOWN_MATH_MATH_MACROS_H

#include <cstddef>
#include <string>
#include <string_view>

namespace nmarkdown {

struct MathMacroExpansion {
    std::string source;
    std::string error;
    std::size_t definition_count = 0;

    bool ok() const { return error.empty(); }
};

// Expands formula-local, zero-argument aliases declared at the beginning with
// \newcommand{\name}{replacement}, \renewcommand, or \def\name{replacement}.
// Definitions, names, replacements, recursion, and total output are bounded.
bool expand_safe_math_macros(std::string_view input,
                             MathMacroExpansion& result);

}  // namespace nmarkdown

#endif
