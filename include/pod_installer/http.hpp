#pragma once

#include "pod_installer/error.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pod_installer {
struct HttpRequest {
    std::string                method{"GET"};
    std::string                path{"/"};
    std::vector<std::string>   headers;
    std::optional<std::string> body;
};

struct HttpResponse {
    long                                             status{};
    std::string                                      body;
    std::vector<std::pair<std::string, std::string>> headers;

    [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
};

std::string         url_encode_component(std::string_view input);
Result<std::string> url_decode_component(std::string_view input, bool plus_as_space = false);
} // namespace pod_installer
