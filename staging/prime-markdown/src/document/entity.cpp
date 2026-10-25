#include "nmarkdown/document/entity.h"

#include <cstdint>

#include "nmarkdown/document/utf8.h"
#include "nmarkdown/generated/html_entities.h"

namespace nmarkdown {
namespace {

std::uint32_t html_control_replacement(std::uint32_t value) {
    switch (value) {
    case 0x80: return 0x20AC;
    case 0x82: return 0x201A;
    case 0x83: return 0x0192;
    case 0x84: return 0x201E;
    case 0x85: return 0x2026;
    case 0x86: return 0x2020;
    case 0x87: return 0x2021;
    case 0x88: return 0x02C6;
    case 0x89: return 0x2030;
    case 0x8A: return 0x0160;
    case 0x8B: return 0x2039;
    case 0x8C: return 0x0152;
    case 0x8E: return 0x017D;
    case 0x91: return 0x2018;
    case 0x92: return 0x2019;
    case 0x93: return 0x201C;
    case 0x94: return 0x201D;
    case 0x95: return 0x2022;
    case 0x96: return 0x2013;
    case 0x97: return 0x2014;
    case 0x98: return 0x02DC;
    case 0x99: return 0x2122;
    case 0x9A: return 0x0161;
    case 0x9B: return 0x203A;
    case 0x9C: return 0x0153;
    case 0x9E: return 0x017E;
    case 0x9F: return 0x0178;
    default: return value;
    }
}

bool decode_numeric(std::string_view body, std::string& decoded) {
    int base = 10;
    std::size_t offset = 1;
    if (offset < body.size() && (body[offset] == 'x' || body[offset] == 'X')) {
        base = 16;
        ++offset;
    }
    if (offset >= body.size()) {
        return false;
    }

    std::uint32_t value = 0;
    for (; offset < body.size(); ++offset) {
        const char character = body[offset];
        int digit = -1;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (base == 16 && character >= 'a' && character <= 'f') {
            digit = 10 + character - 'a';
        } else if (base == 16 && character >= 'A' && character <= 'F') {
            digit = 10 + character - 'A';
        }
        if (digit < 0 || digit >= base) {
            return false;
        }
        if (value > 0x110000U / static_cast<unsigned>(base)) {
            value = 0x110000U;
        } else {
            value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
        }
    }

    value = html_control_replacement(value);
    if (value == 0 || !unicode_scalar_valid(value)) {
        value = kReplacementCodepoint;
    }
    utf8_append(value, decoded);
    return true;
}

}  // namespace

bool decode_html_entity(std::string_view source, std::string& decoded) {
    decoded.clear();
    if (source.size() < 3 || source.front() != '&' || source.back() != ';') {
        return false;
    }
    const std::string_view body = source.substr(1, source.size() - 2);
    if (!body.empty() && body.front() == '#') {
        return decode_numeric(body, decoded);
    }

    std::string_view value;
    if (!lookup_html_entity(body, value)) {
        return false;
    }
    decoded.assign(value.data(), value.size());
    return true;
}

}  // namespace nmarkdown

