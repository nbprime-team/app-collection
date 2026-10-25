#ifndef NMARKDOWN_DOCUMENT_ENTITY_H
#define NMARKDOWN_DOCUMENT_ENTITY_H

#include <string>
#include <string_view>

namespace nmarkdown {

bool decode_html_entity(std::string_view source, std::string& decoded);

}  // namespace nmarkdown

#endif

