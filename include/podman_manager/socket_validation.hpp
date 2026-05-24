#pragma once

#include "podman_manager/error.hpp"
#include "podman_manager/target.hpp"

namespace podman_manager {
struct SocketValidationOptions {
    bool                   require_socket{true};
    bool                   require_owner{true};
    bool                   reject_symlink{true};
    bool                   require_default_path{true};
    RuntimeDirectoryLayout layout{};
};

Result<void> validate_podman_socket(const PodmanTarget &target, const SocketValidationOptions &options = {});
} // namespace podman_manager
