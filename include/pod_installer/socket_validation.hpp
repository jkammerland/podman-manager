#pragma once

#include "pod_installer/error.hpp"
#include "pod_installer/target.hpp"

namespace pod_installer {
struct SocketValidationOptions {
    bool                   require_socket{true};
    bool                   require_owner{true};
    bool                   reject_symlink{true};
    bool                   require_default_path{true};
    RuntimeDirectoryLayout layout{};
};

Result<void> validate_podman_socket(const PodmanTarget &target, const SocketValidationOptions &options = {});
} // namespace pod_installer
