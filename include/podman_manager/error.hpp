#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace podman_manager {
enum class ErrorKind {
    invalid_argument,
    filesystem,
    socket_validation,
    transport,
    http,
    policy,
    systemd,
};

struct Error {
    ErrorKind   kind{ErrorKind::invalid_argument};
    std::string message;
    long        http_status{};
    int         os_errno{};
    int         transport_code{};
};

template <typename T> using Result = std::expected<T, Error>;

std::string_view to_string(ErrorKind kind) noexcept;

Error make_error(ErrorKind kind, std::string message, long http_status = 0, int os_errno = 0, int transport_code = 0);
} // namespace podman_manager
