#include "pod_installer/socket_validation.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

namespace pod_installer {
namespace {
std::filesystem::path normalized(const std::filesystem::path &path) { return path.lexically_normal(); }
} // namespace

Result<void> validate_podman_socket(const PodmanTarget &target, const SocketValidationOptions &options) {
    if (target.socket_path.empty()) {
        return std::unexpected(make_error(ErrorKind::socket_validation, "target socket path is empty"));
    }

    if (options.require_default_path) {
        const auto expected = normalized(options.layout.podman_socket_for(target.uid));
        const auto actual   = normalized(target.socket_path);
        if (actual != expected) {
            return std::unexpected(make_error(ErrorKind::socket_validation, "target socket path '" + actual.string() +
                                                                                "' does not match expected rootless Podman path '" +
                                                                                expected.string() + "'"));
        }
    }

    struct stat st{};
    if (lstat(target.socket_path.c_str(), &st) != 0) {
        return std::unexpected(make_error(ErrorKind::socket_validation,
                                          "lstat failed for '" + target.socket_path.string() + "': " + std::strerror(errno), 0, errno));
    }

    if (options.reject_symlink && S_ISLNK(st.st_mode)) {
        return std::unexpected(make_error(ErrorKind::socket_validation, "target socket path is a symlink: " + target.socket_path.string()));
    }

    if (options.require_socket && !S_ISSOCK(st.st_mode)) {
        return std::unexpected(
            make_error(ErrorKind::socket_validation, "target path is not a Unix socket: " + target.socket_path.string()));
    }

    if (options.require_owner && st.st_uid != target.uid) {
        return std::unexpected(make_error(ErrorKind::socket_validation, "target socket owner uid " + std::to_string(st.st_uid) +
                                                                            " does not match target uid " + std::to_string(target.uid)));
    }

    return {};
}
} // namespace pod_installer
