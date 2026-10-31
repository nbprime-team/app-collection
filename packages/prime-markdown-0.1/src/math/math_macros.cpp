#include "nmarkdown/math/math_macros.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "nmarkdown/math/math_lexer.h"

namespace nmarkdown {
namespace {

constexpr std::size_t kMaximumMacros = 16;
constexpr std::size_t kMaximumMacroName = 32;
constexpr std::size_t kMaximumReplacementBytes = 256;
constexpr unsigned kMaximumExpansionDepth = 8;

struct Macro {
    std::string name;
    std::string replacement;
};

bool letter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

void skip_spaces(std::string_view source, std::size_t& position) {
    while (position < source.size() &&
           (source[position] == ' ' || source[position] == '\t' ||
            source[position] == '\r' || source[position] == '\n')) {
        ++position;
    }
}

bool command_at(std::string_view source,
                std::size_t position,
                std::string_view command) {
    if (position >= source.size() || source[position] != '\\' ||
        command.size() > source.size() - position - 1 ||
        source.substr(position + 1, command.size()) != command) {
        return false;
    }
    const std::size_t end = position + 1 + command.size();
    return end == source.size() || !letter(source[end]);
}

bool braced_content(std::string_view source,
                    std::size_t& position,
                    std::string& content) {
    if (position >= source.size() || source[position] != '{') return false;
    const std::size_t begin = ++position;
    unsigned depth = 1;
    while (position < source.size()) {
        if (source[position] == '\\' && position + 1 < source.size()) {
            position += 2;
            continue;
        }
        if (source[position] == '{') {
            ++depth;
        } else if (source[position] == '}' && --depth == 0) {
            content.assign(source.substr(begin, position - begin));
            ++position;
            return true;
        }
        ++position;
    }
    return false;
}

bool macro_name(std::string_view value, std::string& name) {
    if (value.size() < 2 || value.front() != '\\' ||
        value.size() - 1 > kMaximumMacroName) return false;
    for (std::size_t index = 1; index < value.size(); ++index) {
        if (!letter(value[index])) return false;
    }
    name.assign(value.substr(1));
    return true;
}

bool reserved_name(std::string_view name) {
    constexpr const char* reserved[] = {
        "begin", "end", "left", "right", "frac", "sqrt", "text",
        "operatorname", "newcommand", "renewcommand", "def",
    };
    return std::any_of(std::begin(reserved), std::end(reserved),
                       [name](const char* value) { return name == value; });
}

const Macro* find_macro(const std::vector<Macro>& macros, std::string_view name) {
    for (const Macro& macro : macros) {
        if (macro.name == name) return &macro;
    }
    return nullptr;
}

bool expand(std::string_view source,
            const std::vector<Macro>& macros,
            std::vector<std::string>& stack,
            unsigned depth,
            std::string& output,
            std::string& error) {
    if (depth > kMaximumExpansionDepth) {
        error = "math macro expansion exceeds the depth limit";
        return false;
    }
    std::size_t position = 0;
    while (position < source.size()) {
        if (source[position] != '\\' || position + 1 >= source.size() ||
            !letter(source[position + 1])) {
            output.push_back(source[position++]);
        } else {
            const std::size_t name_begin = position + 1;
            std::size_t name_end = name_begin;
            while (name_end < source.size() && letter(source[name_end])) ++name_end;
            const std::string_view name = source.substr(name_begin, name_end - name_begin);
            const Macro* macro = find_macro(macros, name);
            if (macro == nullptr) {
                output.append(source.substr(position, name_end - position));
            } else {
                if (std::find(stack.begin(), stack.end(), name) != stack.end()) {
                    error = "recursive math macro aliases are not allowed";
                    return false;
                }
                stack.emplace_back(name);
                if (!expand(macro->replacement, macros, stack, depth + 1,
                            output, error)) return false;
                stack.pop_back();
            }
            position = name_end;
        }
        if (output.size() > kMaximumFormulaBytes) {
            error = "expanded formula exceeds the 16 KiB source limit";
            return false;
        }
    }
    return true;
}

}  // namespace

bool expand_safe_math_macros(std::string_view input,
                             MathMacroExpansion& result) {
    result = {};
    std::vector<Macro> macros;
    std::size_t position = 0;
    while (true) {
        skip_spaces(input, position);
        enum class Definition { None, New, Renew, Def } definition = Definition::None;
        std::size_t command_length = 0;
        if (command_at(input, position, "newcommand")) {
            definition = Definition::New;
            command_length = 10;
        } else if (command_at(input, position, "renewcommand")) {
            definition = Definition::Renew;
            command_length = 12;
        } else if (command_at(input, position, "def")) {
            definition = Definition::Def;
            command_length = 3;
        } else {
            break;
        }
        position += command_length + 1;
        skip_spaces(input, position);
        std::string raw_name;
        if (definition == Definition::Def) {
            if (position >= input.size() || input[position] != '\\') {
                result.error = "expected a control-sequence name after def";
                return false;
            }
            const std::size_t begin = position++;
            while (position < input.size() && letter(input[position])) ++position;
            raw_name.assign(input.substr(begin, position - begin));
        } else if (!braced_content(input, position, raw_name)) {
            result.error = "expected a braced math macro name";
            return false;
        }
        std::string name;
        if (!macro_name(raw_name, name) || reserved_name(name)) {
            result.error = "math macro name is invalid or reserved";
            return false;
        }
        skip_spaces(input, position);
        std::string replacement;
        if (!braced_content(input, position, replacement)) {
            result.error = "expected a braced math macro replacement";
            return false;
        }
        if (replacement.size() > kMaximumReplacementBytes ||
            replacement.find('#') != std::string::npos) {
            result.error = "math macro replacement is too large or has parameters";
            return false;
        }
        auto found = std::find_if(macros.begin(), macros.end(),
                                  [&name](const Macro& macro) {
                                      return macro.name == name;
                                  });
        if ((definition == Definition::New && found != macros.end()) ||
            (definition == Definition::Renew && found == macros.end())) {
            result.error = definition == Definition::New
                               ? "math macro alias is already defined"
                               : "renewcommand refers to an undefined alias";
            return false;
        }
        if (found != macros.end()) found->replacement = std::move(replacement);
        else {
            if (macros.size() >= kMaximumMacros) {
                result.error = "formula defines more than 16 math macro aliases";
                return false;
            }
            macros.push_back({std::move(name), std::move(replacement)});
        }
    }
    std::vector<std::string> stack;
    if (!expand(input.substr(position), macros, stack, 0, result.source,
                result.error)) return false;
    result.definition_count = macros.size();
    return true;
}

}  // namespace nmarkdown
