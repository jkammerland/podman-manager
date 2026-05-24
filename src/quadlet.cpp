#include "podman_manager/quadlet.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace podman_manager {
namespace {
class FileDescriptor {
  public:
    explicit FileDescriptor(int fd = -1) noexcept : fd_{fd} {}

    FileDescriptor(const FileDescriptor &)            = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    FileDescriptor(FileDescriptor &&other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    FileDescriptor &operator=(FileDescriptor &&other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~FileDescriptor() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_{};
};

std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

std::string lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool truthy(std::string_view value) {
    const auto normalized = lower_ascii(trim(value));
    return normalized == "1" || normalized == "yes" || normalized == "true" || normalized == "on";
}

bool contains_control_or_slash(std::string_view value) {
    return std::ranges::any_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f || c == '/'; });
}

bool safe_unit_file_char(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@'; }

bool starts_with_option_assignment(std::string_view value, std::string_view option) {
    return value.size() > option.size() && value.starts_with(option) && value[option.size()] == '=';
}

bool equals_key(std::string_view value, std::string_view expected) { return lower_ascii(trim(value)) == lower_ascii(expected); }

std::vector<std::string> split_podman_args(std::string_view value) {
    std::vector<std::string> tokens;
    std::string              current;
    bool                     in_single = false;
    bool                     in_double = false;
    bool                     escaped   = false;

    for (const char c : value) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\' && !in_single) {
            escaped = true;
            continue;
        }
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (escaped) {
        current.push_back('\\');
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::string option_value(std::string_view token, std::string_view option) {
    if (starts_with_option_assignment(token, option)) {
        return std::string{token.substr(option.size() + 1)};
    }
    return {};
}

bool token_has_value(std::string_view token, std::string_view next, std::string_view option, std::string_view denied_value) {
    const auto assigned = lower_ascii(option_value(token, option));
    if (!assigned.empty()) {
        return assigned == lower_ascii(denied_value);
    }
    return lower_ascii(token) == lower_ascii(option) && lower_ascii(next) == lower_ascii(denied_value);
}

bool denied_podman_arg(std::string_view value, const QuadletPolicy &policy) {
    const auto parts = split_podman_args(value);
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        const auto  next = i + 1 < parts.size() ? std::string_view{parts[i + 1]} : std::string_view{};
        if (part.empty()) {
            continue;
        }
        if (!policy.allow_privileged &&
            (lower_ascii(part) == "--privileged" || starts_with_option_assignment(lower_ascii(part), "--privileged"))) {
            return true;
        }
        if (!policy.allow_host_network &&
            (token_has_value(part, next, "--network", "host") || token_has_value(part, next, "--net", "host"))) {
            return true;
        }
        if (!policy.allow_host_pid && token_has_value(part, next, "--pid", "host")) {
            return true;
        }
        if (!policy.allow_host_ipc && token_has_value(part, next, "--ipc", "host")) {
            return true;
        }
        if (!policy.allow_host_userns && token_has_value(part, next, "--userns", "host")) {
            return true;
        }
        if (!policy.allow_devices && (lower_ascii(part) == "--device" || starts_with_option_assignment(lower_ascii(part), "--device"))) {
            return true;
        }
        if (!policy.allow_root_mount && (lower_ascii(part) == "--volume" || starts_with_option_assignment(lower_ascii(part), "--volume") ||
                                         lower_ascii(part) == "-v" || starts_with_option_assignment(lower_ascii(part), "-v") ||
                                         lower_ascii(part) == "--mount" || starts_with_option_assignment(lower_ascii(part), "--mount"))) {
            return true;
        }
    }
    return false;
}

bool host_path_volume(std::string_view value) {
    const auto trimmed = trim(value);
    const auto colon   = trimmed.find(':');
    const auto source  = trim(colon == std::string::npos ? trimmed : std::string_view{trimmed}.substr(0, colon));
    if (source.empty()) {
        return false;
    }
    return source.starts_with("/") || source.starts_with(".") || source.starts_with("~") || source.starts_with("$") ||
           source.find('/') != std::string::npos || source.find('\\') != std::string::npos || source.find("..") != std::string::npos ||
           source.find('%') != std::string::npos;
}

bool host_path_mount(std::string_view value) {
    std::istringstream in{trim(value)};
    std::string        part;
    while (std::getline(in, part, ',')) {
        const auto eq          = part.find('=');
        const auto key         = lower_ascii(trim(eq == std::string::npos ? part : std::string_view{part}.substr(0, eq)));
        const auto val         = trim(eq == std::string::npos ? "" : std::string_view{part}.substr(eq + 1));
        const auto lower_value = lower_ascii(val);
        if (key == "type" && lower_value == "bind") {
            return true;
        }
        if (key == "source" || key == "src" || key == "from") {
            if (val.starts_with("/") || val.starts_with(".") || val.starts_with("~") || val.starts_with("$") ||
                val.find('/') != std::string::npos || val.find('\\') != std::string::npos || val.find("..") != std::string::npos ||
                val.find('%') != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool dangerous_service_key(std::string_view key) {
    return key.starts_with("Exec") || key == "EnvironmentFile" || key == "LoadCredential" || key == "SetCredential" ||
           key == "RootDirectory" || key == "RootImage" || key == "BindPaths" || key == "BindReadOnlyPaths" || key == "WorkingDirectory" ||
           key == "User" || key == "Group";
}

Result<void> validate_allowed_keys(const ParsedQuadlet &parsed) {
    static const std::map<std::string, std::set<std::string>> allowed = {
        {"Unit", {"Description", "After", "Wants", "Requires", "Documentation"}},
        {"Container",
         {"Image", "ContainerName", "Label", "Environment", "ReadOnly", "NoNewPrivileges", "DropCapability", "Network", "Volume", "Mount",
          "UserNS", "PID", "IPCHost", "Device", "AddDevice", "Rootfs", "PodmanArgs"}},
        {"Service", {"Restart", "TimeoutStartSec", "TimeoutStopSec"}},
        {"Install", {"WantedBy"}},
    };

    for (const auto &[section, entries] : parsed.sections()) {
        const auto section_it = allowed.find(section);
        if (section_it == allowed.end()) {
            return std::unexpected(make_error(ErrorKind::policy, "unsupported Quadlet section: " + section));
        }
        for (const auto &[key, values] : entries) {
            (void)values;
            if (dangerous_service_key(key)) {
                auto message = "unsupported host-executing service key: " + section;
                message += '.';
                message += key;
                return std::unexpected(make_error(ErrorKind::policy, message));
            }
            if (!section_it->second.contains(key)) {
                auto message = "unsupported Quadlet key: " + section;
                message += '.';
                message += key;
                return std::unexpected(make_error(ErrorKind::policy, message));
            }
        }
    }

    return {};
}

Result<void> ensure_directory_safe(const std::filesystem::path &dir, const QuadletInstallLayout &layout) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to create Quadlet directory '" + dir.string() + "': " + ec.message()));
    }

    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0) {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to stat Quadlet directory '" + dir.string() + "': " + std::strerror(errno), 0, errno));
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet directory is not a real directory: " + dir.string()));
    }
    if (layout.required_owner_uid && st.st_uid != *layout.required_owner_uid) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet directory owner uid " + std::to_string(st.st_uid) +
                                                                     " does not match required uid " +
                                                                     std::to_string(*layout.required_owner_uid)));
    }
    if (layout.required_owner_gid && st.st_gid != *layout.required_owner_gid) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet directory owner gid " + std::to_string(st.st_gid) +
                                                                     " does not match required gid " +
                                                                     std::to_string(*layout.required_owner_gid)));
    }
    if (chmod(dir.c_str(), 0755) != 0) {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to chmod Quadlet directory '" + dir.string() + "': " + std::strerror(errno), 0, errno));
    }
    if (lstat(dir.c_str(), &st) != 0) {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to restat Quadlet directory '" + dir.string() + "': " + std::strerror(errno), 0, errno));
    }
    if ((st.st_mode & 0022) != 0) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet directory must not be group/other writable: " + dir.string()));
    }
    return {};
}

Result<void> ensure_admin_tree(uid_t uid, const QuadletInstallLayout &layout) {
    if (auto result = ensure_directory_safe(layout.admin_user_root, layout); !result) {
        return result;
    }
    return ensure_directory_safe(layout.user_directory(uid), layout);
}

Result<void> write_all(int fd, std::string_view contents) {
    while (!contents.empty()) {
        const auto written = write(fd, contents.data(), contents.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                make_error(ErrorKind::filesystem, "failed to write Quadlet file: " + std::string{std::strerror(errno)}, 0, errno));
        }
        contents.remove_prefix(static_cast<size_t>(written));
    }
    return {};
}

Result<void> fsync_directory(int dir_fd) {
    if (fsync(dir_fd) != 0) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to fsync Quadlet directory: " + std::string{std::strerror(errno)}, 0, errno));
    }
    return {};
}

Result<void> atomic_write_file(const std::filesystem::path &final_path, std::string_view contents, mode_t mode) {
    const auto     dir        = final_path.parent_path();
    const auto     final_name = final_path.filename().string();
    FileDescriptor dir_fd{open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (dir_fd.get() < 0) {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open Quadlet directory '" + dir.string() + "': " + std::strerror(errno), 0, errno));
    }

    std::optional<std::string> tmp_name;
    FileDescriptor             fd;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = "." + final_name + ".tmp." + std::to_string(getpid()) + "." + std::to_string(attempt);
        fd.reset(openat(dir_fd.get(), candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode));
        if (fd.get() >= 0) {
            tmp_name = candidate;
            break;
        }
        if (errno != EEXIST) {
            return std::unexpected(make_error(ErrorKind::filesystem,
                                              "failed to open temporary Quadlet file in '" + dir.string() + "': " + std::strerror(errno), 0,
                                              errno));
        }
    }
    if (!tmp_name) {
        return std::unexpected(make_error(ErrorKind::filesystem, "failed to allocate unique temporary Quadlet file name"));
    }

    if (auto result = write_all(fd.get(), contents); !result) {
        unlinkat(dir_fd.get(), tmp_name->c_str(), 0);
        return result;
    }

    if (fsync(fd.get()) != 0) {
        unlinkat(dir_fd.get(), tmp_name->c_str(), 0);
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to fsync temporary Quadlet file: " + std::string{std::strerror(errno)}, 0, errno));
    }

    fd.reset();
    if (renameat(dir_fd.get(), tmp_name->c_str(), dir_fd.get(), final_name.c_str()) != 0) {
        unlinkat(dir_fd.get(), tmp_name->c_str(), 0);
        return std::unexpected(make_error(
            ErrorKind::filesystem, "failed to install Quadlet file '" + final_path.string() + "': " + std::strerror(errno), 0, errno));
    }

    return fsync_directory(dir_fd.get());
}

Result<std::string> read_regular_file_no_symlink(const std::filesystem::path &path, size_t max_size) {
    FileDescriptor fd{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.get() < 0) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to open Quadlet file '" + path.string() + "': " + std::strerror(errno), 0, errno));
    }

    struct stat st{};
    if (fstat(fd.get(), &st) != 0) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to stat Quadlet file '" + path.string() + "': " + std::strerror(errno), 0, errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet path is not a regular file: " + path.string()));
    }
    if (st.st_size < 0 || std::cmp_greater(st.st_size, max_size)) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet file is too large: " + path.string()));
    }

    std::string out(static_cast<size_t>(st.st_size), '\0');
    size_t      offset = 0;
    while (offset < out.size()) {
        const auto n = read(fd.get(), out.data() + offset, out.size() - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(make_error(ErrorKind::filesystem,
                                              "failed to read Quadlet file '" + path.string() + "': " + std::strerror(errno), 0, errno));
        }
        if (n == 0) {
            return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet file changed while reading: " + path.string()));
        }
        offset += static_cast<size_t>(n);
    }
    return out;
}

Result<void> unlink_quadlet_file(const std::filesystem::path &path) {
    const auto     dir       = path.parent_path();
    const auto     file_name = path.filename().string();
    FileDescriptor dir_fd{open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (dir_fd.get() < 0) {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open Quadlet directory for remove: " + std::string{std::strerror(errno)}, 0, errno));
    }
    if (unlinkat(dir_fd.get(), file_name.c_str(), 0) != 0 && errno != ENOENT) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to remove Quadlet file '" + path.string() + "': " + std::strerror(errno), 0, errno));
    }
    return fsync_directory(dir_fd.get());
}

Result<void> validate_managed_label(const ParsedQuadlet &parsed, const QuadletPolicy &policy) {
    size_t matches        = 0;
    size_t managed_labels = 0;
    for (const auto &label : parsed.values("Container", "Label")) {
        const auto eq    = label.find('=');
        const auto key   = trim(eq == std::string::npos ? label : std::string_view{label}.substr(0, eq));
        const auto value = trim(eq == std::string::npos ? "" : std::string_view{label}.substr(eq + 1));
        if (key == policy.managed_label_key) {
            ++managed_labels;
            if (value == policy.managed_label_value) {
                ++matches;
            }
        }
    }
    if (managed_labels != 1 || matches != 1) {
        return std::unexpected(make_error(ErrorKind::policy, "Quadlet must carry exactly one managed Label=" + policy.managed_label_key +
                                                                 "=" + policy.managed_label_value));
    }
    return {};
}
} // namespace

std::filesystem::path QuadletInstallLayout::user_directory(uid_t uid) const { return admin_user_root / std::to_string(uid); }

std::filesystem::path QuadletInstallLayout::quadlet_path(uid_t uid, std::string_view file_name) const {
    return user_directory(uid) / std::string{file_name};
}

bool ParsedQuadlet::has_section(std::string_view section) const { return sections_.contains(std::string{section}); }

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<std::string> ParsedQuadlet::values(std::string_view section, std::string_view key) const {
    const auto section_it = sections_.find(std::string{section});
    if (section_it == sections_.end()) {
        return {};
    }
    const auto key_it = section_it->second.find(std::string{key});
    if (key_it == section_it->second.end()) {
        return {};
    }
    return key_it->second;
}

const std::map<std::string, ParsedQuadlet::Section> &ParsedQuadlet::sections() const noexcept { return sections_; }

void ParsedQuadlet::add(std::string section, std::string key, std::string value) {
    sections_[std::move(section)][std::move(key)].push_back(std::move(value));
}

Result<void> validate_quadlet_file_name(std::string_view file_name) {
    if (file_name.empty() || file_name.size() > 255) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet file name must be 1 to 255 bytes"));
    }
    if (file_name == "." || file_name == ".." || file_name.starts_with('.') || file_name.starts_with('-')) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet file name must not be hidden, relative, or option-like"));
    }
    if (contains_control_or_slash(file_name)) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet file name must not contain slashes or control characters"));
    }
    if (!std::ranges::all_of(file_name, safe_unit_file_char)) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet file name contains unsupported characters"));
    }
    if (!file_name.ends_with(".container")) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "MVP deployment bundles require a .container Quadlet file"));
    }
    return {};
}

Result<std::string> service_unit_name_from_quadlet(std::string_view file_name) {
    if (auto result = validate_quadlet_file_name(file_name); !result) {
        return std::unexpected(result.error());
    }
    constexpr std::string_view suffix = ".container";
    return std::string{file_name.substr(0, file_name.size() - suffix.size())} + ".service";
}

Result<ParsedQuadlet> parse_quadlet(std::string_view contents) {
    ParsedQuadlet parsed;
    std::string   section;
    size_t        line_no = 0;

    while (!contents.empty()) {
        ++line_no;
        const auto newline = contents.find('\n');
        auto       line    = newline == std::string_view::npos ? contents : contents.substr(0, newline);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        contents = newline == std::string_view::npos ? std::string_view{} : contents.substr(newline + 1);

        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#') || trimmed.starts_with(';')) {
            continue;
        }
        if (trimmed.starts_with('[') && trimmed.ends_with(']')) {
            section = trim(std::string_view{trimmed}.substr(1, trimmed.size() - 2));
            if (section.empty()) {
                return std::unexpected(
                    make_error(ErrorKind::invalid_argument, "empty section name in Quadlet at line " + std::to_string(line_no)));
            }
            continue;
        }

        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(
                make_error(ErrorKind::invalid_argument, "expected key=value in Quadlet at line " + std::to_string(line_no)));
        }
        if (section.empty()) {
            return std::unexpected(
                make_error(ErrorKind::invalid_argument, "Quadlet key appears before any section at line " + std::to_string(line_no)));
        }
        auto key   = trim(std::string_view{trimmed}.substr(0, eq));
        auto value = trim(std::string_view{trimmed}.substr(eq + 1));
        if (key.empty()) {
            return std::unexpected(make_error(ErrorKind::invalid_argument, "empty key in Quadlet at line " + std::to_string(line_no)));
        }
        parsed.add(section, std::move(key), std::move(value));
    }

    return parsed;
}

Result<void> validate_quadlet_policy(const QuadletFile &quadlet, const QuadletPolicy &policy) {
    if (auto result = validate_quadlet_file_name(quadlet.file_name); !result) {
        return result;
    }
    if (quadlet.contents.empty()) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet contents are empty"));
    }
    if (quadlet.contents.find('\0') != std::string::npos) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "Quadlet contents must not contain NUL bytes"));
    }

    auto parsed = parse_quadlet(quadlet.contents);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (policy.require_container_section && !parsed->has_section("Container")) {
        return std::unexpected(make_error(ErrorKind::policy, "Quadlet must contain a [Container] section"));
    }
    if (policy.require_image && parsed->values("Container", "Image").empty()) {
        return std::unexpected(make_error(ErrorKind::policy, "Quadlet [Container] section must contain Image="));
    }
    if (auto result = validate_allowed_keys(*parsed); !result) {
        return std::unexpected(result.error());
    }
    if (policy.require_managed_label) {
        if (auto result = validate_managed_label(*parsed, policy); !result) {
            return std::unexpected(result.error());
        }
    }

    for (const auto &value : parsed->values("Container", "Network")) {
        if (!policy.allow_host_network && equals_key(value, "host")) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet Network=host is not allowed"));
        }
    }
    for (const auto &value : parsed->values("Container", "PID")) {
        if (!policy.allow_host_pid && equals_key(value, "host")) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet PID=host is not allowed"));
        }
    }
    for (const auto &value : parsed->values("Container", "IPCHost")) {
        if (!policy.allow_host_ipc && truthy(value)) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet IPCHost=true is not allowed"));
        }
    }
    for (const auto &value : parsed->values("Container", "UserNS")) {
        if (!policy.allow_host_userns && equals_key(value, "host")) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet UserNS=host is not allowed"));
        }
    }
    for (const auto &value : parsed->values("Container", "Device")) {
        if (!policy.allow_devices && !trim(value).empty()) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet Device= is not allowed by default"));
        }
    }
    for (const auto &value : parsed->values("Container", "AddDevice")) {
        if (!policy.allow_devices && !trim(value).empty()) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet AddDevice= is not allowed by default"));
        }
    }
    for (const auto &value : parsed->values("Container", "Volume")) {
        if (!policy.allow_root_mount && host_path_volume(value)) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet host path Volume= mounts are not allowed"));
        }
    }
    for (const auto &value : parsed->values("Container", "Mount")) {
        if (!policy.allow_root_mount && host_path_mount(value)) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet host path Mount= mounts are not allowed"));
        }
    }
    for (const auto &value : parsed->values("Container", "Rootfs")) {
        if (!policy.allow_root_mount && !trim(value).empty()) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet Rootfs= is not allowed by default"));
        }
    }
    for (const auto &value : parsed->values("Container", "PodmanArgs")) {
        if (!policy.allow_podman_args) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet PodmanArgs= is not allowed by default"));
        }
        if (denied_podman_arg(value, policy)) {
            return std::unexpected(make_error(ErrorKind::policy, "Quadlet PodmanArgs contains a denied argument"));
        }
    }

    return {};
}

QuadletInstaller::QuadletInstaller(QuadletInstallLayout layout, QuadletPolicy policy)
    : layout_{std::move(layout)}, policy_{std::move(policy)} {}

const QuadletInstallLayout &QuadletInstaller::layout() const noexcept { return layout_; }

const QuadletPolicy &QuadletInstaller::policy() const noexcept { return policy_; }

Result<InstalledQuadlet> QuadletInstaller::expected_install(uid_t uid, const QuadletFile &quadlet) const {
    if (quadlet.contents.size() > layout_.max_quadlet_bytes) {
        return std::unexpected(make_error(ErrorKind::policy, "Quadlet contents exceed max_quadlet_bytes"));
    }
    if (auto result = validate_quadlet_policy(quadlet, policy_); !result) {
        return std::unexpected(result.error());
    }
    auto unit = service_unit_name_from_quadlet(quadlet.file_name);
    if (!unit) {
        return std::unexpected(unit.error());
    }
    return InstalledQuadlet{
        .path         = layout_.quadlet_path(uid, quadlet.file_name),
        .systemd_unit = *unit,
    };
}

Result<QuadletSnapshot> QuadletInstaller::snapshot_for_user(uid_t uid, std::string_view file_name) const {
    if (auto result = validate_quadlet_file_name(file_name); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = ensure_admin_tree(uid, layout_); !result) {
        return std::unexpected(result.error());
    }

    QuadletSnapshot snapshot;
    snapshot.file_name = std::string{file_name};
    snapshot.path      = layout_.quadlet_path(uid, file_name);

    struct stat st{};
    if (lstat(snapshot.path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            snapshot.existed = false;
            return snapshot;
        }
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to stat Quadlet snapshot path '" + snapshot.path.string() + "': " + std::strerror(errno),
                                          0, errno));
    }

    snapshot.existed = true;
    auto contents    = read_regular_file_no_symlink(snapshot.path, layout_.max_quadlet_bytes);
    if (!contents) {
        return std::unexpected(contents.error());
    }
    snapshot.contents = *std::move(contents);
    return snapshot;
}

Result<void> QuadletInstaller::restore_for_user(uid_t uid, const QuadletSnapshot &snapshot) const {
    if (auto result = validate_quadlet_file_name(snapshot.file_name); !result) {
        return result;
    }
    if (auto result = ensure_admin_tree(uid, layout_); !result) {
        return result;
    }
    const auto path = layout_.quadlet_path(uid, snapshot.file_name);
    if (path != snapshot.path) {
        return std::unexpected(make_error(ErrorKind::filesystem, "Quadlet snapshot path does not match installer layout"));
    }
    if (!snapshot.existed) {
        return unlink_quadlet_file(path);
    }
    return atomic_write_file(path, snapshot.contents, 0644);
}

Result<void> QuadletInstaller::remove_for_user(uid_t uid, std::string_view file_name) const {
    if (auto result = validate_quadlet_file_name(file_name); !result) {
        return result;
    }
    if (auto result = ensure_admin_tree(uid, layout_); !result) {
        return result;
    }
    return unlink_quadlet_file(layout_.quadlet_path(uid, file_name));
}

Result<InstalledQuadlet> QuadletInstaller::install_for_user(uid_t uid, const QuadletFile &quadlet) const {
    auto expected = expected_install(uid, quadlet);
    if (!expected) {
        return std::unexpected(expected.error());
    }

    if (auto result = ensure_admin_tree(uid, layout_); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = atomic_write_file(expected->path, quadlet.contents, 0644); !result) {
        return std::unexpected(result.error());
    }

    return expected;
}
} // namespace podman_manager
