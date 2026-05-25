#include "pod_installer/target.hpp"

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pod_installer {
std::filesystem::path RuntimeDirectoryLayout::runtime_dir_for(uid_t uid) const { return root / std::to_string(uid); }

std::filesystem::path RuntimeDirectoryLayout::podman_socket_for(uid_t uid) const { return runtime_dir_for(uid) / "podman" / "podman.sock"; }

PodmanTarget make_target(uid_t uid, std::string user_name, const RuntimeDirectoryLayout &layout, std::string api_version) {
    return PodmanTarget{.uid         = uid,
                        .user_name   = std::move(user_name),
                        .runtime_dir = layout.runtime_dir_for(uid),
                        .socket_path = layout.podman_socket_for(uid),
                        .api_version = std::move(api_version)};
}

namespace {
size_t passwd_buffer_size() {
    const long configured = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (configured > 0) {
        return static_cast<size_t>(configured);
    }
    return static_cast<size_t>(16) * 1024;
}
} // namespace

Result<PodmanTarget> resolve_user(std::string_view user_name, const RuntimeDirectoryLayout &layout, std::string api_version) {
    if (user_name.empty()) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "user name is empty"));
    }

    std::string       user_name_owned{user_name};
    passwd            pw{};
    passwd           *result{};
    std::vector<char> buffer(passwd_buffer_size());

    const int rc = getpwnam_r(user_name_owned.c_str(), &pw, buffer.data(), buffer.size(), &result);
    if (rc != 0) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "getpwnam_r failed for user '" + user_name_owned + "': " + std::strerror(rc), 0, rc));
    }
    if (result == nullptr) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "user '" + user_name_owned + "' does not exist"));
    }

    return make_target(pw.pw_uid, pw.pw_name, layout, std::move(api_version));
}

Result<PodmanTarget> resolve_uid(uid_t uid, const RuntimeDirectoryLayout &layout, std::string api_version) {
    passwd            pw{};
    passwd           *result{};
    std::vector<char> buffer(passwd_buffer_size());

    const int rc = getpwuid_r(uid, &pw, buffer.data(), buffer.size(), &result);
    if (rc != 0) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "getpwuid_r failed for uid " + std::to_string(uid) + ": " + std::strerror(rc), 0, rc));
    }
    if (result == nullptr) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "uid " + std::to_string(uid) + " does not exist"));
    }

    return make_target(pw.pw_uid, pw.pw_name, layout, std::move(api_version));
}
} // namespace pod_installer
