#include "pod_installer/http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace pod_installer {
namespace {
std::string lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
} // namespace

std::optional<std::string> HttpResponse::header(std::string_view name) const {
    const auto wanted = lower_ascii(name);
    for (const auto &[key, value] : headers) {
        if (lower_ascii(key) == wanted) {
            return value;
        }
    }
    return std::nullopt;
}

std::string url_encode_component(std::string_view input) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char c : input) {
        const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

Result<std::string> url_decode_component(std::string_view input, bool plus_as_space) {
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '+' && plus_as_space) {
            out.push_back(' ');
            continue;
        }

        if (c != '%') {
            out.push_back(c);
            continue;
        }

        if (i + 2 >= input.size()) {
            return std::unexpected(make_error(ErrorKind::invalid_argument, "incomplete percent escape in URL component"));
        }

        const int hi = hex_value(input[i + 1]);
        const int lo = hex_value(input[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::unexpected(make_error(ErrorKind::invalid_argument, "invalid percent escape in URL component"));
        }

        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }

    return out;
}
} // namespace pod_installer
