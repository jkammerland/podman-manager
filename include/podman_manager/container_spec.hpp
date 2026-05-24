#pragma once

#include "podman_manager/error.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace podman_manager {
struct CpuLimits {
    std::optional<int64_t>     period;
    std::optional<int64_t>     quota;
    std::optional<int64_t>     shares;
    std::optional<std::string> cpus;
};

struct MemoryLimits {
    std::optional<int64_t> limit;
    std::optional<int64_t> swap;
};

struct PidsLimits {
    std::optional<int64_t> limit;
};

struct ResourceLimits {
    std::optional<CpuLimits>    cpu;
    std::optional<MemoryLimits> memory;
    std::optional<PidsLimits>   pids;
};

struct ContainerSpec {
    std::string                        name;
    std::string                        image;
    std::vector<std::string>           command;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> labels;
    bool                               read_only_filesystem{true};
    std::vector<std::string>           cap_drop{"ALL"};
    std::vector<std::string>           security_opt{"no-new-privileges"};
    std::optional<ResourceLimits>      resource_limits;
};

Result<void>        validate_container_spec(const ContainerSpec &spec);
Result<std::string> to_podman_create_json(const ContainerSpec &spec);
} // namespace podman_manager
