#include "podman_manager/systemd.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace podman_manager {
namespace {
bool safe_systemd_value(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

bool safe_systemd_unit(std::string_view unit) {
    if (unit.empty() || unit.size() > 255 || unit.starts_with('-')) {
        return false;
    }
    for (const unsigned char c : unit) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@' || c == '\\')) {
            return false;
        }
    }
    return unit.ends_with(".service") || unit.ends_with(".socket") || unit.ends_with(".timer") || unit.ends_with(".target");
}

Result<int> wait_for_child(pid_t pid) {
    int status{};
    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) {
            return status;
        }
        if (errno == EINTR) {
            continue;
        }
        return std::unexpected(
            make_error(ErrorKind::systemd, "waitpid failed for systemctl: " + std::string{std::strerror(errno)}, 0, errno));
    }
}

Result<void> run_process(const std::vector<std::string> &args) {
    if (args.empty()) {
        return std::unexpected(make_error(ErrorKind::systemd, "no process arguments supplied"));
    }

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t     pid{};
    const int spawn_rc = posix_spawnp(&pid, args.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn_rc != 0) {
        return std::unexpected(
            make_error(ErrorKind::systemd, "posix_spawnp failed for '" + args.front() + "': " + std::strerror(spawn_rc), 0, spawn_rc));
    }

    auto status = wait_for_child(pid);
    if (!status) {
        return std::unexpected(status.error());
    }

    if (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0) {
        return std::unexpected(make_error(ErrorKind::systemd, "systemctl exited with status " + std::to_string(*status)));
    }

    return {};
}

Result<std::string> run_process_capture(const std::vector<std::string> &args) {
    if (args.empty()) {
        return std::unexpected(make_error(ErrorKind::systemd, "no process arguments supplied"));
    }

    int pipe_fds[2]{};
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        return std::unexpected(make_error(ErrorKind::systemd, "pipe2 failed: " + std::string{std::strerror(errno)}, 0, errno));
    }

    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t     pid{};
    const int spawn_rc = posix_spawnp(&pid, args.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fds[1]);

    if (spawn_rc != 0) {
        close(pipe_fds[0]);
        return std::unexpected(
            make_error(ErrorKind::systemd, "posix_spawnp failed for '" + args.front() + "': " + std::strerror(spawn_rc), 0, spawn_rc));
    }

    std::string output;
    char        buffer[4096];
    for (;;) {
        const auto n = read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        close(pipe_fds[0]);
        return std::unexpected(
            make_error(ErrorKind::systemd, "read failed for systemctl output: " + std::string{std::strerror(errno)}, 0, errno));
    }
    close(pipe_fds[0]);

    auto status = wait_for_child(pid);
    if (!status) {
        return std::unexpected(status.error());
    }
    if (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0) {
        return std::unexpected(
            make_error(ErrorKind::systemd, "systemctl exited with status " + std::to_string(*status) + " output: " + output));
    }
    return output;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
UnitStatus parse_show_status(std::string_view unit, std::string_view output) {
    UnitStatus status;
    status.unit = std::string{unit};

    std::istringstream in{std::string{output}};
    std::string        line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const auto key   = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "LoadState") {
            status.load_state = value;
        } else if (key == "ActiveState") {
            status.active_state = value;
        } else if (key == "SubState") {
            status.sub_state = value;
        }
    }
    return status;
}
} // namespace

Result<void> validate_user_slice_policy(const UserSlicePolicy &policy) {
    const bool any = policy.cpu_quota || policy.cpu_weight || policy.memory_max || policy.tasks_max || policy.allowed_cpus;
    if (!any) {
        return std::unexpected(make_error(ErrorKind::policy, "user slice policy contains no properties"));
    }

    if (policy.cpu_quota && !safe_systemd_value(*policy.cpu_quota)) {
        return std::unexpected(make_error(ErrorKind::policy, "CPUQuota value is empty or contains control chars"));
    }
    if (policy.cpu_weight && (*policy.cpu_weight < 1 || *policy.cpu_weight > 10000)) {
        return std::unexpected(make_error(ErrorKind::policy, "CPUWeight must be between 1 and 10000"));
    }
    if (policy.memory_max && !safe_systemd_value(*policy.memory_max)) {
        return std::unexpected(make_error(ErrorKind::policy, "MemoryMax value is empty or contains control chars"));
    }
    if (policy.tasks_max && *policy.tasks_max == 0) {
        return std::unexpected(make_error(ErrorKind::policy, "TasksMax must be positive"));
    }
    if (policy.allowed_cpus && !safe_systemd_value(*policy.allowed_cpus)) {
        return std::unexpected(make_error(ErrorKind::policy, "AllowedCPUs value is empty or contains control chars"));
    }

    return {};
}

Result<std::vector<std::string>> build_systemctl_set_property_args(const UserSlicePolicy &policy) {
    if (auto result = validate_user_slice_policy(policy); !result) {
        return std::unexpected(result.error());
    }

    std::vector<std::string> args{
        "systemctl",
        "set-property",
        "--runtime",
        "user-" + std::to_string(policy.uid) + ".slice",
    };
    if (policy.cpu_quota) {
        args.push_back("CPUQuota=" + *policy.cpu_quota);
    }
    if (policy.cpu_weight) {
        args.push_back("CPUWeight=" + std::to_string(*policy.cpu_weight));
    }
    if (policy.memory_max) {
        args.push_back("MemoryMax=" + *policy.memory_max);
    }
    if (policy.tasks_max) {
        args.push_back("TasksMax=" + std::to_string(*policy.tasks_max));
    }
    if (policy.allowed_cpus) {
        args.push_back("AllowedCPUs=" + *policy.allowed_cpus);
    }

    return args;
}

Result<void> validate_systemd_unit_name(std::string_view unit) {
    if (!safe_systemd_unit(unit)) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "unsupported systemd unit name: " + std::string{unit}));
    }
    return {};
}

Result<std::vector<std::string>> build_systemctl_user_args(const PodmanTarget &target, std::string_view command,
                                                           std::optional<std::string_view> unit) {
    if (target.user_name.empty()) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "systemctl user commands require target user_name"));
    }
    if (!safe_systemd_value(command)) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "invalid systemctl command"));
    }
    if (unit) {
        if (auto result = validate_systemd_unit_name(*unit); !result) {
            return std::unexpected(result.error());
        }
    }

    std::vector<std::string> args{
        "systemctl",
        "--user",
        "--machine=" + target.user_name + "@.host",
        std::string{command},
    };
    if (unit) {
        args.emplace_back(*unit);
    }
    return args;
}

SystemctlSliceController::SystemctlSliceController(bool dry_run) : dry_run_{dry_run} {}

bool SystemctlSliceController::dry_run() const noexcept { return dry_run_; }

Result<void> SystemctlSliceController::apply(const UserSlicePolicy &policy) const {
    auto args = build_systemctl_set_property_args(policy);
    if (!args) {
        return std::unexpected(args.error());
    }
    if (dry_run_) {
        return {};
    }
    return run_process(*args);
}

SystemctlUserSystemdController::SystemctlUserSystemdController(bool dry_run) : dry_run_{dry_run} {}

bool SystemctlUserSystemdController::dry_run() const noexcept { return dry_run_; }

Result<void> SystemctlUserSystemdController::daemon_reload(const PodmanTarget &target) const {
    auto args = build_systemctl_user_args(target, "daemon-reload");
    if (!args) {
        return std::unexpected(args.error());
    }
    if (dry_run_) {
        return {};
    }
    return run_process(*args);
}

namespace {
Result<UnitOperationResult> completed_operation(const SystemctlUserSystemdController &controller, const PodmanTarget &target,
                                                std::string_view unit) {
    auto status = controller.status(target, unit);
    if (!status) {
        return std::unexpected(status.error());
    }
    UnitOperationResult out;
    out.final_status = *status;
    return out;
}
} // namespace

Result<UnitOperationResult> SystemctlUserSystemdController::start_unit(const PodmanTarget &target, std::string_view unit) const {
    auto args = build_systemctl_user_args(target, "start", unit);
    if (!args) {
        return std::unexpected(args.error());
    }
    if (dry_run_) {
        return completed_operation(*this, target, unit);
    }
    if (auto result = run_process(*args); !result) {
        return std::unexpected(result.error());
    }
    return completed_operation(*this, target, unit);
}

Result<UnitOperationResult> SystemctlUserSystemdController::restart_unit(const PodmanTarget &target, std::string_view unit) const {
    auto args = build_systemctl_user_args(target, "restart", unit);
    if (!args) {
        return std::unexpected(args.error());
    }
    if (dry_run_) {
        return completed_operation(*this, target, unit);
    }
    if (auto result = run_process(*args); !result) {
        return std::unexpected(result.error());
    }
    return completed_operation(*this, target, unit);
}

Result<UnitOperationResult> SystemctlUserSystemdController::stop_unit(const PodmanTarget &target, std::string_view unit) const {
    auto args = build_systemctl_user_args(target, "stop", unit);
    if (!args) {
        return std::unexpected(args.error());
    }
    if (dry_run_) {
        return completed_operation(*this, target, unit);
    }
    if (auto result = run_process(*args); !result) {
        return std::unexpected(result.error());
    }
    return completed_operation(*this, target, unit);
}

Result<UnitStatus> SystemctlUserSystemdController::status(const PodmanTarget &target, std::string_view unit) const {
    if (auto result = validate_systemd_unit_name(unit); !result) {
        return std::unexpected(result.error());
    }
    if (dry_run_) {
        UnitStatus out;
        out.unit = std::string{unit};
        return out;
    }

    auto args = build_systemctl_user_args(target, "show", unit);
    if (!args) {
        return std::unexpected(args.error());
    }
    args->emplace_back("--property=LoadState");
    args->emplace_back("--property=ActiveState");
    args->emplace_back("--property=SubState");

    auto output = run_process_capture(*args);
    if (!output) {
        return std::unexpected(output.error());
    }
    return parse_show_status(unit, *output);
}
} // namespace podman_manager
