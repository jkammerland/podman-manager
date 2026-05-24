#include "podman_manager/deployment.hpp"

namespace podman_manager
{
namespace
{
template <typename T>
Result<T> rollback_error(const QuadletInstaller& installer,
                         uid_t uid,
                         const QuadletSnapshot& snapshot,
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

    if (options_.dry_run)
    {
        return result;
    }

    if (options_.validate_socket && options_.load_image_archive && bundle.image_archive)
    {
        if (auto socket = validate_podman_socket(*target); !socket)
        {
            return std::unexpected(socket.error());
        }
    }

    if (options_.load_image_archive && bundle.image_archive)
    {
        PodmanClient podman{*target};
        if (auto ping = podman.ping(); !ping)
        {
            return std::unexpected(ping.error());
        }
        if (auto loaded = podman.load_image_archive(bundle.image_archive->path); !loaded)
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
        return rollback_error<DeploymentResult>(installer_, bundle.target_uid, *snapshot, reload.error());
    }

    if (options_.restart_unit)
    {
        auto operation = systemd_->restart_unit(*target, result.systemd_unit);
        if (!operation)
        {
            return rollback_error<DeploymentResult>(installer_, bundle.target_uid, *snapshot, operation.error());
        }
        result.job_path = operation->job_path;
        result.status = operation->final_status;
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
