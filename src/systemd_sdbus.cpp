#include "pod_installer/systemd.hpp"

#if POD_INSTALLER_HAS_SDBUS

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace pod_installer {
namespace {
constexpr auto systemd_service      = "org.freedesktop.systemd1";
constexpr auto systemd_path         = "/org/freedesktop/systemd1";
constexpr auto manager_interface    = "org.freedesktop.systemd1.Manager";
constexpr auto unit_interface       = "org.freedesktop.systemd1.Unit";
constexpr auto properties_interface = "org.freedesktop.DBus.Properties";

std::string user_bus_address(const PodmanTarget &target) {
    const auto runtime_dir =
        target.runtime_dir.empty() ? std::filesystem::path{"/run/user"} / std::to_string(target.uid) : target.runtime_dir;
    return "unix:path=" + (runtime_dir / "bus").string();
}

std::unique_ptr<sdbus::IConnection> user_bus_connection(const PodmanTarget &target) {
    return sdbus::createSessionBusConnectionWithAddress(user_bus_address(target));
}

std::unique_ptr<sdbus::IProxy> manager_proxy(std::unique_ptr<sdbus::IConnection> &&connection) {
    return sdbus::createProxy(std::move(connection), sdbus::ServiceName{systemd_service}, sdbus::ObjectPath{systemd_path},
                              sdbus::dont_run_event_loop_thread);
}

std::unique_ptr<sdbus::IProxy> manager_proxy(const PodmanTarget &target) { return manager_proxy(user_bus_connection(target)); }

Result<UnitOperationResult> unit_job_and_wait(const PodmanTarget &target, std::string_view method, std::string_view unit,
                                              std::chrono::milliseconds timeout) {
    if (auto result = validate_systemd_unit_name(unit); !result) {
        return std::unexpected(result.error());
    }

    try {
        auto connection   = user_bus_connection(target);
        auto proxy        = sdbus::createProxy(*connection, sdbus::ServiceName{systemd_service}, sdbus::ObjectPath{systemd_path});
        auto job_result   = std::make_shared<std::promise<std::string>>();
        auto job_future   = job_result->get_future();
        auto expected_job = std::make_shared<std::optional<std::string>>();

        auto slot = proxy->uponSignal("JobRemoved")
                        .onInterface(manager_interface)
                        .call(
                            [expected_job, job_result, done = std::make_shared<bool>(false)](
                                uint32_t, const sdbus::ObjectPath &job, const std::string &, const std::string &result) {
                                if (*done) {
                                    return;
                                }
                                if (!*expected_job || std::string{job} != **expected_job) {
                                    return;
                                }
                                *done = true;
                                job_result->set_value(result);
                            },
                            sdbus::return_slot);

        sdbus::ObjectPath job;
        proxy->callMethod(std::string{method})
            .onInterface(manager_interface)
            .withArguments(std::string{unit}, std::string{"replace"})
            .storeResultsTo(job);
        *expected_job = std::string{job};
        connection->enterEventLoopAsync();

        if (job_future.wait_for(timeout) != std::future_status::ready) {
            connection->leaveEventLoop();
            return std::unexpected(
                make_error(ErrorKind::systemd, "systemd D-Bus " + std::string{method} + " timed out waiting for JobRemoved"));
        }

        const auto result = job_future.get();
        connection->leaveEventLoop();
        if (result != "done") {
            return std::unexpected(
                make_error(ErrorKind::systemd, "systemd D-Bus " + std::string{method} + " job finished with result " + result));
        }

        UnitOperationResult out;
        out.job_path = std::string{job};
        SdbusUserSystemdController status_reader{timeout};
        auto                       status = status_reader.status(target, unit);
        if (!status) {
            return std::unexpected(status.error());
        }
        out.final_status = *status;
        return out;
    } catch (const sdbus::Error &error) {
        return std::unexpected(make_error(ErrorKind::systemd, std::string{"systemd D-Bus "} + std::string{method} +
                                                                  " failed: " + error.getName() + ": " + error.getMessage()));
    }
}

Result<std::string> get_unit_property_string(sdbus::IProxy &proxy, std::string_view property) {
    try {
        sdbus::Variant value;
        proxy.callMethod("Get")
            .onInterface(properties_interface)
            .withArguments(std::string{unit_interface}, std::string{property})
            .storeResultsTo(value);
        return value.get<std::string>();
    } catch (const sdbus::Error &error) {
        return std::unexpected(
            make_error(ErrorKind::systemd, "systemd D-Bus property read failed: " + error.getName() + ": " + error.getMessage()));
    }
}
} // namespace

SdbusUserSystemdController::SdbusUserSystemdController(std::chrono::milliseconds job_timeout) : job_timeout_{job_timeout} {}

Result<void> SdbusUserSystemdController::daemon_reload(const PodmanTarget &target) const {
    try {
        auto proxy = manager_proxy(target);
        proxy->callMethod("Reload").onInterface(manager_interface);
        return {};
    } catch (const sdbus::Error &error) {
        return std::unexpected(
            make_error(ErrorKind::systemd, "systemd D-Bus Reload failed: " + error.getName() + ": " + error.getMessage()));
    }
}

Result<UnitOperationResult> SdbusUserSystemdController::start_unit(const PodmanTarget &target, std::string_view unit) const {
    return unit_job_and_wait(target, "StartUnit", unit, job_timeout_);
}

Result<UnitOperationResult> SdbusUserSystemdController::restart_unit(const PodmanTarget &target, std::string_view unit) const {
    return unit_job_and_wait(target, "RestartUnit", unit, job_timeout_);
}

Result<UnitOperationResult> SdbusUserSystemdController::stop_unit(const PodmanTarget &target, std::string_view unit) const {
    return unit_job_and_wait(target, "StopUnit", unit, job_timeout_);
}

Result<UnitStatus> SdbusUserSystemdController::status(const PodmanTarget &target, std::string_view unit) const {
    if (auto result = validate_systemd_unit_name(unit); !result) {
        return std::unexpected(result.error());
    }

    try {
        auto              manager = manager_proxy(target);
        sdbus::ObjectPath unit_path;
        manager->callMethod("LoadUnit").onInterface(manager_interface).withArguments(std::string{unit}).storeResultsTo(unit_path);

        auto connection = sdbus::createSessionBusConnectionWithAddress(user_bus_address(target));
        auto unit_proxy =
            sdbus::createProxy(std::move(connection), sdbus::ServiceName{systemd_service}, unit_path, sdbus::dont_run_event_loop_thread);

        UnitStatus out;
        out.unit = std::string{unit};

        auto load = get_unit_property_string(*unit_proxy, "LoadState");
        if (!load) {
            return std::unexpected(load.error());
        }
        auto active = get_unit_property_string(*unit_proxy, "ActiveState");
        if (!active) {
            return std::unexpected(active.error());
        }
        auto sub = get_unit_property_string(*unit_proxy, "SubState");
        if (!sub) {
            return std::unexpected(sub.error());
        }

        out.load_state   = *load;
        out.active_state = *active;
        out.sub_state    = *sub;
        return out;
    } catch (const sdbus::Error &error) {
        return std::unexpected(
            make_error(ErrorKind::systemd, "systemd D-Bus status failed: " + error.getName() + ": " + error.getMessage()));
    }
}
} // namespace pod_installer

#endif
