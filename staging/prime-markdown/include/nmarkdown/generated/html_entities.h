#ifndef NMARKDOWN_GENERATED_HTML_ENTITIES_H
#define NMARKDOWN_GENERATED_HTML_ENTITIES_H

#include <string_view>

namespace nmarkdown {

bool lookup_html_entity(std::string_view name, std::string_view& utf8_value);

}  // namespace nmarkdown

#endif

