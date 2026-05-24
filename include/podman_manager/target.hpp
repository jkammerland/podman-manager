#pragma once

#include "podman_manager/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace podman_manager {
struct RuntimeDirectoryLayout {
    std::filesystem::path root{"/run/user"};

    [[nodiscard]] std::filesystem::path runtime_dir_for(uid_t uid) const;
    [[nodiscard]] std::filesystem::path podman_socket_for(uid_t uid) const;
};

struct PodmanTarget {
    uid_t                 uid{};
    std::string           user_name;
    std::filesystem::path runtime_dir;
    std::filesystem::path socket_path;
    std::string           api_version{"5.0.0"};
};

PodmanTarget make_target(uid_t uid, std::string user_name, const RuntimeDirectoryLayout &layout = {}, std::string api_version = "5.0.0");

Result<PodmanTarget> resolve_user(std::string_view user_name, const RuntimeDirectoryLayout &layout = {}, std::string api_version = "5.0.0");

Result<PodmanTarget> resolve_uid(uid_t uid, const RuntimeDirectoryLayout &layout = {}, std::string api_version = "5.0.0");
} // namespace podman_manager
