#pragma once

#include "podman_manager/error.hpp"
#include "podman_manager/target.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace podman_manager {
struct UserSlicePolicy {
    uid_t                      uid{};
    std::optional<std::string> cpu_quota;
    std::optional<uint64_t>    cpu_weight;
    std::optional<std::string> memory_max;
    std::optional<uint64_t>    tasks_max;
    std::optional<std::string> allowed_cpus;
};

Result<void>                     validate_user_slice_policy(const UserSlicePolicy &policy);
Result<std::vector<std::string>> build_systemctl_set_property_args(const UserSlicePolicy &policy);

struct UnitStatus {
    std::string unit;
    std::string load_state;
    std::string active_state;
    std::string sub_state;
};

struct UnitOperationResult {
    std::optional<std::string> job_path;
    UnitStatus                 final_status;
};

class UserSystemdController {
  public:
    virtual ~UserSystemdController() = default;

    virtual Result<void>                daemon_reload(const PodmanTarget &target) const                       = 0;
    virtual Result<UnitOperationResult> start_unit(const PodmanTarget &target, std::string_view unit) const   = 0;
    virtual Result<UnitOperationResult> restart_unit(const PodmanTarget &target, std::string_view unit) const = 0;
    virtual Result<UnitOperationResult> stop_unit(const PodmanTarget &target, std::string_view unit) const    = 0;
    virtual Result<UnitStatus>          status(const PodmanTarget &target, std::string_view unit) const       = 0;
};

Result<void>                     validate_systemd_unit_name(std::string_view unit);
Result<std::vector<std::string>> build_systemctl_user_args(const PodmanTarget &target, std::string_view command,
                                                           std::optional<std::string_view> unit = std::nullopt);

class SystemctlSliceController {
  public:
    explicit SystemctlSliceController(bool dry_run = false);

    [[nodiscard]] bool dry_run() const noexcept;
    Result<void>       apply(const UserSlicePolicy &policy) const;

  private:
    bool dry_run_{};
};

class SystemctlUserSystemdController final : public UserSystemdController {
  public:
    explicit SystemctlUserSystemdController(bool dry_run = false);

    [[nodiscard]] bool dry_run() const noexcept;

    Result<void>                daemon_reload(const PodmanTarget &target) const override;
    Result<UnitOperationResult> start_unit(const PodmanTarget &target, std::string_view unit) const override;
    Result<UnitOperationResult> restart_unit(const PodmanTarget &target, std::string_view unit) const override;
    Result<UnitOperationResult> stop_unit(const PodmanTarget &target, std::string_view unit) const override;
    Result<UnitStatus>          status(const PodmanTarget &target, std::string_view unit) const override;

  private:
    bool dry_run_{};
};

#if PODMAN_MANAGER_HAS_SDBUS
class SdbusUserSystemdController final : public UserSystemdController {
  public:
    explicit SdbusUserSystemdController(std::chrono::milliseconds job_timeout = std::chrono::seconds{30});

    Result<void>                daemon_reload(const PodmanTarget &target) const override;
    Result<UnitOperationResult> start_unit(const PodmanTarget &target, std::string_view unit) const override;
    Result<UnitOperationResult> restart_unit(const PodmanTarget &target, std::string_view unit) const override;
    Result<UnitOperationResult> stop_unit(const PodmanTarget &target, std::string_view unit) const override;
    Result<UnitStatus>          status(const PodmanTarget &target, std::string_view unit) const override;

  private:
    std::chrono::milliseconds job_timeout_;
};
#endif
} // namespace podman_manager
