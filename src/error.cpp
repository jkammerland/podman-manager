#include "pod_installer/error.hpp"

#include <utility>

namespace pod_installer {
std::string_view to_string(ErrorKind kind) noexcept {
    switch (kind) {
    case ErrorKind::invalid_argument: return "invalid_argument";
    case ErrorKind::filesystem: return "filesystem";
    case ErrorKind::socket_validation: return "socket_validation";
    case ErrorKind::transport: return "transport";
    case ErrorKind::http: return "http";
    case ErrorKind::policy: return "policy";
    case ErrorKind::systemd: return "systemd";
    }
    return "unknown";
}

Error make_error(ErrorKind kind, std::string message, long http_status, int os_errno, int transport_code) {
    return Error{.kind           = kind,
                 .message        = std::move(message),
                 .http_status    = http_status,
                 .os_errno       = os_errno,
                 .transport_code = transport_code};
}
} // namespace pod_installer
