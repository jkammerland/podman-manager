#pragma once

#include "pod_installer/error.hpp"
#include "pod_installer/podman_client.hpp"
#include "pod_installer/quadlet.hpp"
#include "pod_installer/socket_validation.hpp"
#include "pod_installer/systemd.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace pod_installer {
struct ImageArchive {
    std::filesystem::path path;
    std::string           expected_sha256;
};

struct DeploymentBundle {
    uid_t                       target_uid{};
    std::string                 service_name;
    std::optional<ImageArchive> image_archive;
    QuadletFile                 quadlet;
    std::string                 revision;
};

struct DeploymentOptions {
    RuntimeDirectoryLayout               runtime_layout{};
    std::optional<std::filesystem::path> image_archive_root;
    uintmax_t                            max_image_archive_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    std::string                          api_version{"5.0.0"};
    bool                                 validate_socket{true};
    bool                                 load_image_archive{true};
    bool                                 restart_unit{true};
    bool                                 dry_run{false};
};

struct DeploymentResult {
    std::filesystem::path      installed_quadlet_path;
    std::string                systemd_unit;
    std::optional<std::string> job_path;
    std::optional<UnitStatus>  status;
    std::optional<std::string> status_error;
    bool                       dry_run{};
    bool                       rolled_back{};
};

Result<void> validate_deployment_bundle(const DeploymentBundle &bundle);

class BundleVerifier {
  public:
    Result<void> verify(const DeploymentBundle &bundle) const;
};

class DeploymentOrchestrator {
  public:
    DeploymentOrchestrator(QuadletInstaller installer, std::shared_ptr<UserSystemdController> systemd, DeploymentOptions options = {});

    DeploymentOrchestrator(const DeploymentOrchestrator &)                = delete;
    DeploymentOrchestrator &operator=(const DeploymentOrchestrator &)     = delete;
    DeploymentOrchestrator(DeploymentOrchestrator &&) noexcept            = default;
    DeploymentOrchestrator &operator=(DeploymentOrchestrator &&) noexcept = default;

    Result<DeploymentResult> deploy(const DeploymentBundle &bundle) const;

  private:
    QuadletInstaller                       installer_;
    std::shared_ptr<UserSystemdController> systemd_;
    DeploymentOptions                      options_;
    BundleVerifier                         verifier_;
};
} // namespace pod_installer
