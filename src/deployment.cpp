#include "podman_manager/deployment.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <optional>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace podman_manager
{
namespace
{
class FileDescriptor
{
public:
    explicit FileDescriptor(int fd = -1) noexcept
        : fd_{fd}
    {
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_{std::exchange(other.fd_, -1)}
    {
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~FileDescriptor()
    {
        reset();
    }

    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }

    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
        {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{};
};

bool path_component_is_safe(const std::filesystem::path& component)
{
    const auto value = component.string();
    return !value.empty() && value != "." && value != "..";
}

Result<std::filesystem::path> relative_staged_path(const std::filesystem::path& root,
                                                   const std::filesystem::path& path)
{
    for (const auto& component : path)
    {
        if (!path_component_is_safe(component))
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "image archive path must not contain '.' or '..': " +
                                                  path.string()));
        }
    }

    auto normalized = path.lexically_normal();
    if (normalized.is_absolute())
    {
        const auto normalized_root = root.lexically_normal();
        auto path_it = normalized.begin();
        auto root_it = normalized_root.begin();
        for (; root_it != normalized_root.end(); ++root_it, ++path_it)
        {
            if (path_it == normalized.end() || *path_it != *root_it)
            {
                return std::unexpected(make_error(ErrorKind::policy,
                                                  "image archive path is outside configured staging root: " +
                                                      normalized.string()));
            }
        }

        std::filesystem::path relative;
        for (; path_it != normalized.end(); ++path_it)
        {
            relative /= *path_it;
        }
        normalized = relative;
    }
    if (normalized.empty() || normalized == ".")
    {
        return std::unexpected(make_error(ErrorKind::policy, "image archive path is empty"));
    }
    for (const auto& component : normalized)
    {
        if (!path_component_is_safe(component))
        {
            return std::unexpected(make_error(ErrorKind::policy,
                                              "image archive path must not contain '.' or '..': " +
                                                  normalized.string()));
        }
    }
    return normalized;
}

struct OpenArchive
{
    FileDescriptor fd;
    uintmax_t size{};
};

Result<void> validate_owner_if_required(int fd,
                                        const QuadletInstallLayout& layout,
                                        std::string_view description)
{
    struct stat st
    {
    };
    if (fstat(fd, &st) != 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to stat " + std::string{description} + ": " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }
    if (layout.required_owner_uid && st.st_uid != *layout.required_owner_uid)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          std::string{description} + " owner uid " +
                                              std::to_string(st.st_uid) + " does not match required uid " +
                                              std::to_string(*layout.required_owner_uid)));
    }
    if (layout.required_owner_gid && st.st_gid != *layout.required_owner_gid)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          std::string{description} + " owner gid " +
                                              std::to_string(st.st_gid) + " does not match required gid " +
                                              std::to_string(*layout.required_owner_gid)));
    }
    return {};
}

Result<OpenArchive> open_staged_image_archive(const ImageArchive& archive,
                                              const DeploymentOptions& options)
{
    if (!archive.expected_sha256.empty())
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "ImageArchive::expected_sha256 is not implemented; verify bundle "
                                          "manifest digests before deployment"));
    }

    if (!options.image_archive_root)
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "deployment image loading requires DeploymentOptions::image_archive_root"));
    }

    auto relative = relative_staged_path(*options.image_archive_root, archive.path);
    if (!relative)
    {
        return std::unexpected(relative.error());
    }

    FileDescriptor current{open(options.image_archive_root->c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (current.get() < 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open image archive staging root '" +
                                              options.image_archive_root->string() + "': " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }

    auto component = relative->begin();
    for (; component != relative->end(); ++component)
    {
        const auto next = std::next(component);
        const auto name = component->string();
        const bool final = next == relative->end();
        if (final)
        {
            struct stat st
            {
            };
            if (fstatat(current.get(), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0)
            {
                return std::unexpected(make_error(ErrorKind::filesystem,
                                                  "failed to stat staged image archive component '" + name + "': " +
                                                      std::strerror(errno),
                                                  0,
                                                  errno));
            }
            if (!S_ISREG(st.st_mode))
            {
                return std::unexpected(make_error(ErrorKind::policy,
                                                  "staged image archive must be a regular file"));
            }
            if (st.st_size < 0 || static_cast<uintmax_t>(st.st_size) > options.max_image_archive_bytes)
            {
                return std::unexpected(make_error(ErrorKind::policy,
                                                  "staged image archive is too large"));
            }
        }

        FileDescriptor next_fd{openat(current.get(),
                                      name.c_str(),
                                      final ? (O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
                                            : (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))};
        if (next_fd.get() < 0)
        {
            return std::unexpected(make_error(ErrorKind::filesystem,
                                              "failed to open staged image archive component '" + name + "': " +
                                                  std::strerror(errno),
                                              0,
                                              errno));
        }
        current = std::move(next_fd);
    }

    struct stat st
    {
    };
    if (fstat(current.get(), &st) != 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to stat staged image archive: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }
    if (!S_ISREG(st.st_mode))
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "staged image archive must be a regular file"));
    }
    if (st.st_size < 0 || static_cast<uintmax_t>(st.st_size) > options.max_image_archive_bytes)
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "staged image archive is too large"));
    }
    return OpenArchive{.fd = std::move(current), .size = static_cast<uintmax_t>(st.st_size)};
}

class DeploymentLock
{
public:
    DeploymentLock() = default;

    explicit DeploymentLock(FileDescriptor fd) noexcept
        : fd_{std::move(fd)}
    {
    }

    DeploymentLock(const DeploymentLock&) = delete;
    DeploymentLock& operator=(const DeploymentLock&) = delete;
    DeploymentLock(DeploymentLock&&) noexcept = default;
    DeploymentLock& operator=(DeploymentLock&&) noexcept = default;

private:
    FileDescriptor fd_;
};

Result<DeploymentLock> lock_deployment(const QuadletInstallLayout& layout,
                                       uid_t uid,
                                       std::string_view unit)
{
    std::error_code ec;
    std::filesystem::create_directories(layout.admin_user_root, ec);
    if (ec)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to create deployment lock root '" +
                                              layout.admin_user_root.string() + "': " + ec.message()));
    }

    FileDescriptor root_fd{open(layout.admin_user_root.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (root_fd.get() < 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open deployment lock root '" +
                                              layout.admin_user_root.string() + "': " + std::strerror(errno),
                                          0,
                                          errno));
    }
    if (auto owner = validate_owner_if_required(root_fd.get(), layout, "deployment lock root"); !owner)
    {
        return std::unexpected(owner.error());
    }

    if (mkdirat(root_fd.get(), ".locks", 0755) != 0 && errno != EEXIST)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to create deployment lock directory: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }

    FileDescriptor dir_fd{openat(root_fd.get(), ".locks", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (dir_fd.get() < 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open deployment lock directory: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }
    if (auto owner = validate_owner_if_required(dir_fd.get(), layout, "deployment lock directory"); !owner)
    {
        return std::unexpected(owner.error());
    }
    if (fchmod(dir_fd.get(), 0755) != 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to chmod deployment lock directory: " +
                                              std::string{std::strerror(errno)},
                                          0,
                                          errno));
    }

    const auto lock_name = std::to_string(uid) + "-" + std::string{unit} + ".lock";
    FileDescriptor lock_fd{openat(dir_fd.get(),
                                  lock_name.c_str(),
                                  O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                  0644)};
    if (lock_fd.get() < 0)
    {
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to open deployment lock '" + lock_name + "': " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }
    for (;;)
    {
        if (flock(lock_fd.get(), LOCK_EX) == 0)
        {
            return DeploymentLock{std::move(lock_fd)};
        }
        if (errno == EINTR)
        {
            continue;
        }
        return std::unexpected(make_error(ErrorKind::filesystem,
                                          "failed to lock deployment '" + lock_name + "': " +
                                              std::strerror(errno),
                                          0,
                                          errno));
    }
}

bool deployed_status_is_healthy(const UnitStatus& status)
{
    return status.load_state == "loaded" && status.active_state == "active";
}

std::string describe_status(const UnitStatus& status)
{
    return "LoadState=" + status.load_state + " ActiveState=" + status.active_state +
           " SubState=" + status.sub_state;
}

template <typename T>
Result<T> rollback_error(const QuadletInstaller& installer,
                         const UserSystemdController& systemd,
                         const PodmanTarget& target,
                         uid_t uid,
                         const QuadletSnapshot& snapshot,
                         std::string_view unit,
                         bool restart_restored_unit,
                         Error original)
{
    auto rollback = installer.restore_for_user(uid, snapshot);
    if (!rollback)
    {
        original.message += "; rollback failed: " + rollback.error().message;
    }
    else
    {
        original.message += "; rolled back installed Quadlet";
        if (auto reload = systemd.daemon_reload(target); !reload)
        {
            original.message += "; rollback daemon-reload failed: " + reload.error().message;
        }
        else if (restart_restored_unit && snapshot.existed)
        {
            auto restart = systemd.restart_unit(target, unit);
            if (!restart)
            {
                original.message += "; rollback restart failed: " + restart.error().message;
            }
            else if (!deployed_status_is_healthy(restart->final_status))
            {
                original.message += "; rollback restart left unit unhealthy: " +
                                    describe_status(restart->final_status);
            }
        }
    }
    return std::unexpected(std::move(original));
}
}

Result<void> validate_deployment_bundle(const DeploymentBundle& bundle)
{
    if (bundle.target_uid == 0)
    {
        return std::unexpected(make_error(ErrorKind::policy,
                                          "deployment target must be a non-root user uid"));
    }
    if (bundle.service_name.empty())
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "deployment service_name is empty"));
    }
    if (auto unit = service_unit_name_from_quadlet(bundle.quadlet.file_name); !unit)
    {
        return std::unexpected(unit.error());
    }
    else if (*unit != bundle.service_name + ".service")
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "deployment service_name '" + bundle.service_name +
                                              "' does not match Quadlet file '" +
                                              bundle.quadlet.file_name + "'"));
    }
    if (bundle.image_archive && bundle.image_archive->path.empty())
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "image archive path is empty"));
    }
    return {};
}

Result<void> BundleVerifier::verify(const DeploymentBundle& bundle) const
{
    // TODO: verify bundle signature, signer identity, manifest digest, and
    // artifact-to-manifest binding before deployment. The current MVP assumes
    // this verification happened before DeploymentOrchestrator::deploy().
    (void)bundle;
    return {};
}

DeploymentOrchestrator::DeploymentOrchestrator(QuadletInstaller installer,
                                               std::shared_ptr<UserSystemdController> systemd,
                                               DeploymentOptions options)
    : installer_{std::move(installer)}
    , systemd_{std::move(systemd)}
    , options_{std::move(options)}
{
}

Result<DeploymentResult> DeploymentOrchestrator::deploy(const DeploymentBundle& bundle) const
{
    if (!systemd_)
    {
        return std::unexpected(make_error(ErrorKind::invalid_argument,
                                          "deployment orchestrator requires a systemd controller"));
    }
    if (auto verified = verifier_.verify(bundle); !verified)
    {
        return std::unexpected(verified.error());
    }
    if (auto result = validate_deployment_bundle(bundle); !result)
    {
        return std::unexpected(result.error());
    }

    auto target = resolve_uid(bundle.target_uid, options_.runtime_layout, options_.api_version);
    if (!target)
    {
        return std::unexpected(target.error());
    }

    auto expected = installer_.expected_install(bundle.target_uid, bundle.quadlet);
    if (!expected)
    {
        return std::unexpected(expected.error());
    }

    DeploymentResult result;
    result.installed_quadlet_path = expected->path;
    result.systemd_unit = expected->systemd_unit;
    result.dry_run = options_.dry_run;

    std::optional<OpenArchive> archive;
    if (options_.load_image_archive && bundle.image_archive)
    {
        auto opened = open_staged_image_archive(*bundle.image_archive, options_);
        if (!opened)
        {
            return std::unexpected(opened.error());
        }
        archive = std::move(*opened);
    }

    if (options_.dry_run)
    {
        return result;
    }

    auto deployment_lock = lock_deployment(installer_.layout(), bundle.target_uid, expected->systemd_unit);
    if (!deployment_lock)
    {
        return std::unexpected(deployment_lock.error());
    }
    (void)deployment_lock;

    if (options_.validate_socket && options_.load_image_archive && bundle.image_archive)
    {
        SocketValidationOptions socket_options;
        socket_options.layout = options_.runtime_layout;
        if (auto socket = validate_podman_socket(*target, socket_options); !socket)
        {
            return std::unexpected(socket.error());
        }
    }

    if (archive)
    {
        PodmanClient podman{*target};
        if (auto ping = podman.ping(); !ping)
        {
            return std::unexpected(ping.error());
        }
        if (auto loaded = podman.load_image_archive_fd(archive->fd.get(), archive->size); !loaded)
        {
            return std::unexpected(loaded.error());
        }
    }

    auto snapshot = installer_.snapshot_for_user(bundle.target_uid, bundle.quadlet.file_name);
    if (!snapshot)
    {
        return std::unexpected(snapshot.error());
    }

    auto installed = installer_.install_for_user(bundle.target_uid, bundle.quadlet);
    if (!installed)
    {
        return std::unexpected(installed.error());
    }
    result.installed_quadlet_path = installed->path;
    result.systemd_unit = installed->systemd_unit;

    if (auto reload = systemd_->daemon_reload(*target); !reload)
    {
        return rollback_error<DeploymentResult>(installer_,
                                                *systemd_,
                                                *target,
                                                bundle.target_uid,
                                                *snapshot,
                                                result.systemd_unit,
                                                false,
                                                reload.error());
    }

    if (options_.restart_unit)
    {
        auto operation = systemd_->restart_unit(*target, result.systemd_unit);
        if (!operation)
        {
            return rollback_error<DeploymentResult>(installer_,
                                                    *systemd_,
                                                    *target,
                                                    bundle.target_uid,
                                                    *snapshot,
                                                    result.systemd_unit,
                                                    true,
                                                    operation.error());
        }
        result.job_path = operation->job_path;
        result.status = operation->final_status;
        if (!deployed_status_is_healthy(operation->final_status))
        {
            return rollback_error<DeploymentResult>(
                installer_,
                *systemd_,
                *target,
                bundle.target_uid,
                *snapshot,
                result.systemd_unit,
                true,
                make_error(ErrorKind::systemd,
                           "deployment unit is unhealthy after restart: " +
                               describe_status(operation->final_status)));
        }
        return result;
    }

    auto status = systemd_->status(*target, result.systemd_unit);
    if (status)
    {
        result.status = *status;
    }
    else
    {
        result.status_error = status.error().message;
    }

    return result;
}
}
