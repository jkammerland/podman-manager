#include "pod_installer/container_spec.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace pod_installer {
namespace {
bool has_control(std::string_view value) {
    return std::ranges::any_of(value, [](unsigned char c) { return c < 0x20; });
}

bool valid_container_name(std::string_view value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    const auto valid_first = [](unsigned char c) { return std::isalnum(c); };
    const auto valid_rest  = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; };
    if (!valid_first(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    return std::ranges::all_of(value.substr(1), valid_rest);
}

bool valid_key(std::string_view value) { return !value.empty() && !has_control(value); }

bool valid_cpuset(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](unsigned char c) { return std::isdigit(c) || c == ',' || c == '-'; });
}

Result<void> require_positive(std::optional<int64_t> value, std::string_view field) {
    if (value && *value <= 0) {
        return std::unexpected(make_error(ErrorKind::policy, std::string(field) + " must be positive"));
    }
    return {};
}

void append_json_string(std::string &out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                out += escaped.str();
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void append_field(std::string &out, bool &first, std::string_view name, std::string_view raw_value) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    append_json_string(out, name);
    out.push_back(':');
    out += raw_value;
}

std::string json_string(std::string_view value) {
    std::string out;
    append_json_string(out, value);
    return out;
}

std::string json_string_array(const std::vector<std::string> &values) {
    std::string out{"["};
    bool        first = true;
    for (const auto &value : values) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        append_json_string(out, value);
    }
    out.push_back(']');
    return out;
}

std::string json_string_map(const std::map<std::string, std::string> &values) {
    std::string out{"{"};
    bool        first = true;
    for (const auto &[key, value] : values) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        append_json_string(out, key);
        out.push_back(':');
        append_json_string(out, value);
    }
    out.push_back('}');
    return out;
}

std::string resource_limits_json(const ResourceLimits &limits) {
    std::string out{"{"};
    bool        first = true;

    if (limits.cpu) {
        std::string cpu{"{"};
        bool        cpu_first = true;
        if (limits.cpu->period) {
            append_field(cpu, cpu_first, "period", std::to_string(*limits.cpu->period));
        }
        if (limits.cpu->quota) {
            append_field(cpu, cpu_first, "quota", std::to_string(*limits.cpu->quota));
        }
        if (limits.cpu->shares) {
            append_field(cpu, cpu_first, "shares", std::to_string(*limits.cpu->shares));
        }
        if (limits.cpu->cpus) {
            append_field(cpu, cpu_first, "cpus", json_string(*limits.cpu->cpus));
        }
        cpu.push_back('}');
        append_field(out, first, "cpu", cpu);
    }

    if (limits.memory) {
        std::string memory{"{"};
        bool        memory_first = true;
        if (limits.memory->limit) {
            append_field(memory, memory_first, "limit", std::to_string(*limits.memory->limit));
        }
        if (limits.memory->swap) {
            append_field(memory, memory_first, "swap", std::to_string(*limits.memory->swap));
        }
        memory.push_back('}');
        append_field(out, first, "memory", memory);
    }

    if (limits.pids) {
        std::string pids{"{"};
        bool        pids_first = true;
        if (limits.pids->limit) {
            append_field(pids, pids_first, "limit", std::to_string(*limits.pids->limit));
        }
        pids.push_back('}');
        append_field(out, first, "pids", pids);
    }

    out.push_back('}');
    return out;
}
} // namespace

Result<void> validate_container_spec(const ContainerSpec &spec) {
    if (!valid_container_name(spec.name)) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "container name must match [A-Za-z0-9][A-Za-z0-9_.-]{0,127}"));
    }
    if (spec.image.empty() || has_control(spec.image)) {
        return std::unexpected(
            make_error(ErrorKind::invalid_argument, "container image must be non-empty and contain no control characters"));
    }

    for (const auto &[key, value] : spec.env) {
        (void)value;
        if (!valid_key(key)) {
            return std::unexpected(
                make_error(ErrorKind::invalid_argument, "environment keys must be non-empty and contain no control characters"));
        }
    }

    for (const auto &[key, value] : spec.labels) {
        (void)value;
        if (!valid_key(key)) {
            return std::unexpected(
                make_error(ErrorKind::invalid_argument, "label keys must be non-empty and contain no control characters"));
        }
    }

    if (spec.resource_limits) {
        if (spec.resource_limits->cpu) {
            if (auto result = require_positive(spec.resource_limits->cpu->period, "cpu.period"); !result) {
                return result;
            }
            if (auto result = require_positive(spec.resource_limits->cpu->quota, "cpu.quota"); !result) {
                return result;
            }
            if (auto result = require_positive(spec.resource_limits->cpu->shares, "cpu.shares"); !result) {
                return result;
            }
            if (spec.resource_limits->cpu->cpus && !valid_cpuset(*spec.resource_limits->cpu->cpus)) {
                return std::unexpected(make_error(ErrorKind::policy, "cpu.cpus must contain only digits, commas, and dashes"));
            }
        }
        if (spec.resource_limits->memory) {
            if (auto result = require_positive(spec.resource_limits->memory->limit, "memory.limit"); !result) {
                return result;
            }
            if (auto result = require_positive(spec.resource_limits->memory->swap, "memory.swap"); !result) {
                return result;
            }
        }
        if (spec.resource_limits->pids) {
            if (auto result = require_positive(spec.resource_limits->pids->limit, "pids.limit"); !result) {
                return result;
            }
        }
    }

    return {};
}

Result<std::string> to_podman_create_json(const ContainerSpec &spec) {
    if (auto result = validate_container_spec(spec); !result) {
        return std::unexpected(result.error());
    }

    std::string out{"{"};
    bool        first = true;

    append_field(out, first, "name", json_string(spec.name));
    append_field(out, first, "image", json_string(spec.image));

    if (!spec.command.empty()) {
        append_field(out, first, "command", json_string_array(spec.command));
    }
    if (!spec.env.empty()) {
        append_field(out, first, "env", json_string_map(spec.env));
    }
    if (!spec.labels.empty()) {
        append_field(out, first, "labels", json_string_map(spec.labels));
    }

    append_field(out, first, "read_only_filesystem", spec.read_only_filesystem ? "true" : "false");

    if (!spec.cap_drop.empty()) {
        append_field(out, first, "cap_drop", json_string_array(spec.cap_drop));
    }
    if (!spec.security_opt.empty()) {
        append_field(out, first, "security_opt", json_string_array(spec.security_opt));
    }
    if (spec.resource_limits) {
        append_field(out, first, "resource_limits", resource_limits_json(*spec.resource_limits));
    }

    out.push_back('}');
    return out;
}
} // namespace pod_installer
