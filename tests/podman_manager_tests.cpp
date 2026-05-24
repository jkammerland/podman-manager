#include "podman_manager/podman_manager.hpp"

#ifdef PODMAN_MANAGER_USE_GENTEST
#include "gentest/attributes.h"
#include "gentest/runner.h"
#endif

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pm = podman_manager;

namespace
{
void test_check(bool condition, const char* expression, const char* file, int line)
{
    if (!condition)
    {
        std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
        std::abort();
    }
}

#define PM_CHECK(expr) test_check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#undef assert
#define assert(expr) PM_CHECK(expr)

std::string valid_quadlet_contents()
{
    return "[Unit]\n"
           "Description=Demo\n"
           "\n"
           "[Container]\n"
           "Image=localhost/demo:latest\n"
           "Label=com.example.podman-manager.managed=true\n"
           "ReadOnly=true\n"
           "\n"
           "[Service]\n"
           "Restart=on-failure\n";
}

class FakeSystemdController final : public pm::UserSystemdController
{
public:
    mutable std::vector<std::string> calls;

    pm::Result<void> daemon_reload(const pm::PodmanTarget&) const override
    {
        calls.push_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget&, std::string_view unit) const override
    {
        calls.push_back("start " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/start";
        out.final_status = pm::UnitStatus{.unit = std::string{unit},
                                          .load_state = "loaded",
                                          .active_state = "active",
                                          .sub_state = "running"};
        return out;
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget&, std::string_view unit) const override
    {
        calls.push_back("restart " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/restart";
        out.final_status = pm::UnitStatus{.unit = std::string{unit},
                                          .load_state = "loaded",
                                          .active_state = "active",
                                          .sub_state = "running"};
        return out;
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget&, std::string_view unit) const override
    {
        calls.push_back("stop " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/stop";
        out.final_status = pm::UnitStatus{.unit = std::string{unit},
                                          .load_state = "loaded",
                                          .active_state = "inactive",
                                          .sub_state = "dead"};
        return out;
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget&, std::string_view unit) const override
    {
        calls.push_back("status " + std::string{unit});
        return pm::UnitStatus{.unit = std::string{unit},
                              .load_state = "loaded",
                              .active_state = "active",
                              .sub_state = "running"};
    }
};

class FailingRestartSystemdController final : public pm::UserSystemdController
{
public:
    mutable std::vector<std::string> calls;
    mutable int restart_calls{};

    pm::Result<void> daemon_reload(const pm::PodmanTarget&) const override
    {
        calls.push_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget&, std::string_view) const override
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "start failed"));
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget&, std::string_view unit) const override
    {
        calls.push_back("restart " + std::string{unit});
        ++restart_calls;
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "restart failed"));
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget&, std::string_view) const override
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "stop failed"));
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget&, std::string_view unit) const override
    {
        return pm::UnitStatus{.unit = std::string{unit},
                              .load_state = "loaded",
                              .active_state = "failed",
                              .sub_state = "failed"};
    }
};

class UnhealthyRestartSystemdController final : public pm::UserSystemdController
{
public:
    mutable std::vector<std::string> calls;

    pm::Result<void> daemon_reload(const pm::PodmanTarget&) const override
    {
        calls.push_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget&, std::string_view) const override
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "start not used"));
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget&, std::string_view unit) const override
    {
        calls.push_back("restart " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/restart";
        out.final_status = pm::UnitStatus{.unit = std::string{unit},
                                          .load_state = "loaded",
                                          .active_state = "failed",
                                          .sub_state = "failed"};
        return out;
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget&, std::string_view) const override
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "stop not used"));
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget&, std::string_view unit) const override
    {
        return pm::UnitStatus{.unit = std::string{unit},
                              .load_state = "loaded",
                              .active_state = "failed",
                              .sub_state = "failed"};
    }
};

class TempDir
{
public:
    TempDir()
    {
        auto pattern = std::filesystem::temp_directory_path() / "podman-manager-test-XXXXXX";
        auto value = pattern.string();
        char* created = mkdtemp(value.data());
        assert(created != nullptr);
        path_ = created;
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct UnixListener
{
    int fd{-1};
    std::filesystem::path path;

    explicit UnixListener(std::filesystem::path socket_path)
        : path{std::move(socket_path)}
    {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        assert(fd >= 0);

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const auto path_string = path.string();
        assert(path_string.size() < sizeof(addr.sun_path));
        std::strncpy(addr.sun_path, path_string.c_str(), sizeof(addr.sun_path) - 1);

        std::filesystem::create_directories(path.parent_path());
        unlink(path_string.c_str());
        assert(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        assert(listen(fd, 8) == 0);
    }

    ~UnixListener()
    {
        if (fd >= 0)
        {
            close(fd);
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::string read_http_request(int client)
{
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos)
    {
        const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
        assert(n > 0);
        request.append(buffer, static_cast<size_t>(n));
    }

    const auto content_length_key = std::string{"Content-Length:"};
    const auto header_end = request.find("\r\n\r\n");
    const auto length_pos = request.find(content_length_key);
    if (length_pos != std::string::npos)
    {
        const auto line_end = request.find("\r\n", length_pos);
        auto raw_length = request.substr(length_pos + content_length_key.size(),
                                         line_end - (length_pos + content_length_key.size()));
        const auto length = static_cast<size_t>(std::stoul(raw_length));
        const auto have = request.size() - (header_end + 4);
        while (have < length && request.size() < header_end + 4 + length)
        {
            const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
            assert(n > 0);
            request.append(buffer, static_cast<size_t>(n));
        }
    }

    return request;
}

void send_response(int client, int status, std::string body = "OK", std::string extra_headers = {})
{
    const char* reason = status == 200 ? "OK" : status == 201 ? "Created" : "No Content";
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
    out << extra_headers;
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << body;
    const auto response = out.str();
    assert(send(client, response.data(), response.size(), MSG_NOSIGNAL) > 0);
}
}

[[using gentest: test]]
void test_container_spec_json()
{
    pm::ContainerSpec spec;
    spec.name = "svc-u1000-demo-r42";
    spec.image = "localhost/acme/demo:1.2.3";
    spec.command = {"/bin/sh", "-lc", "echo \"hello\" && sleep 1"};
    spec.env = {{"ACME_MODE", "production\nnext"}};
    spec.labels = {{"com.example.managed", "true"}};
    spec.resource_limits = pm::ResourceLimits{
        .cpu = pm::CpuLimits{.period = 100000, .quota = 50000, .shares = 128, .cpus = "2-3"},
        .memory = pm::MemoryLimits{.limit = 268435456, .swap = 268435456},
        .pids = pm::PidsLimits{.limit = 128},
    };

    auto json = pm::to_podman_create_json(spec);
    assert(json);
    assert(json->find(R"("name":"svc-u1000-demo-r42")") != std::string::npos);
    assert(json->find(R"("command":["/bin/sh","-lc","echo \"hello\" && sleep 1"])") != std::string::npos);
    assert(json->find(R"("ACME_MODE":"production\nnext")") != std::string::npos);
    assert(json->find(R"("cpus":"2-3")") != std::string::npos);
}

[[using gentest: test]]
void test_container_spec_validation_edges()
{
    pm::ContainerSpec invalid_name;
    invalid_name.name = "../bad";
    invalid_name.image = "busybox";
    assert(!pm::to_podman_create_json(invalid_name));

    pm::ContainerSpec invalid_cpu;
    invalid_cpu.name = "ok";
    invalid_cpu.image = "busybox";
    pm::CpuLimits cpu;
    cpu.quota = -1;
    pm::ResourceLimits limits;
    limits.cpu = cpu;
    invalid_cpu.resource_limits = limits;
    auto result = pm::to_podman_create_json(invalid_cpu);
    assert(!result);
    assert(result.error().kind == pm::ErrorKind::policy);
}

[[using gentest: test]]
void test_url_codec()
{
    assert(pm::url_encode_component("a b/c") == "a%20b%2Fc");
    auto decoded = pm::url_decode_component("a+b%2Fc", true);
    assert(decoded);
    assert(*decoded == "a b/c");
    assert(!pm::url_decode_component("%X0"));
}

[[using gentest: test]]
void test_socket_validation()
{
    TempDir temp;
    UnixListener listener{temp.path() / "podman.sock"};
    pm::PodmanTarget target{.uid = getuid(),
                            .user_name = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};

    pm::SocketValidationOptions options;
    options.require_default_path = false;
    assert(pm::validate_podman_socket(target, options));

    const auto symlink_path = temp.path() / "link.sock";
    std::filesystem::create_symlink(listener.path.filename(), symlink_path);
    target.socket_path = symlink_path;
    assert(!pm::validate_podman_socket(target, options));
}

[[using gentest: test]]
void test_systemd_args()
{
    pm::UserSlicePolicy policy{.uid = 1000,
                               .cpu_quota = "200%",
                               .cpu_weight = 100,
                               .memory_max = "1G",
                               .tasks_max = 512,
                               .allowed_cpus = "2-3"};
    auto args = pm::build_systemctl_set_property_args(policy);
    assert(args);
    assert((*args)[0] == "systemctl");
    assert((*args)[2] == "--runtime");
    assert((*args)[3] == "user-1000.slice");
    assert(std::ranges::find(*args, "AllowedCPUs=2-3") != args->end());

    pm::PodmanTarget target;
    target.uid = 1000;
    target.user_name = "alice";
    auto user_args = pm::build_systemctl_user_args(target, "restart", "demo.service");
    assert(user_args);
    assert((*user_args)[0] == "systemctl");
    assert((*user_args)[1] == "--user");
    assert((*user_args)[2] == "--machine=alice@.host");
    assert((*user_args)[3] == "restart");
    assert((*user_args)[4] == "demo.service");
    assert(!pm::build_systemctl_user_args(target, "restart", "../bad.service"));
    assert(!pm::build_systemctl_user_args(target, "restart", "-demo.service"));
}

[[using gentest: test]]
void test_quadlet_policy_and_install()
{
    pm::QuadletFile quadlet;
    quadlet.file_name = "demo.container";
    quadlet.contents = valid_quadlet_contents();

    assert(pm::validate_quadlet_file_name(quadlet.file_name));
    auto unit = pm::service_unit_name_from_quadlet(quadlet.file_name);
    assert(unit);
    assert(*unit == "demo.service");
    assert(pm::validate_quadlet_policy(quadlet));

    pm::QuadletFile bad = quadlet;
    bad.file_name = "../demo.container";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.file_name = "-demo.container";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nPrivileged=true\n"
                   "Label=com.example.podman-manager.managed=true\n";
    auto privileged = pm::validate_quadlet_policy(bad);
    assert(!privileged);
    assert(privileged.error().kind == pm::ErrorKind::policy);

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nNetwork=host\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nAddDevice=/dev/kvm\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nMount=type=bind,source=/,target=/host\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nVolume=../../home/alice/.ssh:/loot:ro\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nMount=type=bind,source=../../host,target=/host\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nRootfs=/\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\nPodmanArgs=--network host\n"
                   "Label=com.example.podman-manager.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    pm::QuadletPolicy podman_args_policy;
    podman_args_policy.allow_podman_args = true;
    assert(!pm::validate_quadlet_policy(bad, podman_args_policy));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\n"
                   "Label=com.example.podman-manager.managed=true\n"
                   "Label=com.example.podman-manager.managed=false\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\n"
                   "Label=com.example.podman-manager.managed=true\n"
                   "\n[Service]\nExecStartPre=/bin/sh -c id\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad = quadlet;
    bad.contents = "[Container]\nImage=busybox\n";
    assert(!pm::validate_quadlet_policy(bad));

    TempDir temp;
    pm::QuadletInstallLayout layout;
    layout.admin_user_root = temp.path();
    layout.required_owner_uid = getuid();
    pm::QuadletInstaller installer{layout};
    auto installed = installer.install_for_user(getuid(), quadlet);
    assert(installed);
    assert(installed->systemd_unit == "demo.service");
    assert(std::filesystem::exists(installed->path));

    std::ifstream in{installed->path};
    std::stringstream contents;
    contents << in.rdbuf();
    assert(contents.str() == quadlet.contents);

    layout.max_quadlet_bytes = valid_quadlet_contents().size();
    pm::QuadletInstaller limited_installer{layout};
    assert(limited_installer.expected_install(getuid(), quadlet));
    bad = quadlet;
    bad.contents.push_back('\n');
    assert(!limited_installer.expected_install(getuid(), bad));
}

[[using gentest: test]]
void test_deployment_orchestrator_installs_and_restarts()
{
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root = temp.path();
    layout.required_owner_uid = getuid();

    pm::DeploymentBundle bundle;
    bundle.target_uid = getuid();
    bundle.service_name = "demo";
    bundle.revision = "42";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents = valid_quadlet_contents();

    auto systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket = false;
    options.load_image_archive = false;
    options.dry_run = false;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto deployed = orchestrator.deploy(bundle);
    assert(deployed);
    assert(deployed->installed_quadlet_path == temp.path() / std::to_string(getuid()) / "demo.container");
    assert(deployed->systemd_unit == "demo.service");
    assert(deployed->job_path == "/org/freedesktop/systemd1/job/restart");
    assert(deployed->status);
    assert(systemd->calls.size() == 2);
    assert(systemd->calls[0] == "daemon-reload");
    assert(systemd->calls[1] == "restart demo.service");
}

[[using gentest: test]]
void test_deployment_rolls_back_when_restart_fails()
{
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root = temp.path();
    layout.required_owner_uid = getuid();

    pm::QuadletInstaller installer{layout};
    pm::QuadletFile old_quadlet;
    old_quadlet.file_name = "demo.container";
    old_quadlet.contents = valid_quadlet_contents();
    assert(installer.install_for_user(getuid(), old_quadlet));

    pm::DeploymentBundle bundle;
    bundle.target_uid = getuid();
    bundle.service_name = "demo";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents =
        "[Unit]\nDescription=New Demo\n\n[Container]\nImage=localhost/demo:new\n"
        "Label=com.example.podman-manager.managed=true\nReadOnly=true\n";

    auto systemd = std::make_shared<FailingRestartSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket = false;
    options.load_image_archive = false;

    pm::DeploymentOrchestrator orchestrator{installer, systemd, options};
    auto deployed = orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("rolled back") != std::string::npos);
    assert(systemd->calls.size() == 4);
    assert(systemd->calls[0] == "daemon-reload");
    assert(systemd->calls[1] == "restart demo.service");
    assert(systemd->calls[2] == "daemon-reload");
    assert(systemd->calls[3] == "restart demo.service");

    std::ifstream in{layout.quadlet_path(getuid(), "demo.container")};
    std::stringstream contents;
    contents << in.rdbuf();
    assert(contents.str() == old_quadlet.contents);
}

[[using gentest: test]]
void test_deployment_rolls_back_when_restart_status_is_unhealthy()
{
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root = temp.path();
    layout.required_owner_uid = getuid();

    pm::QuadletInstaller installer{layout};
    pm::QuadletFile old_quadlet;
    old_quadlet.file_name = "demo.container";
    old_quadlet.contents = valid_quadlet_contents();
    assert(installer.install_for_user(getuid(), old_quadlet));

    pm::DeploymentBundle bundle;
    bundle.target_uid = getuid();
    bundle.service_name = "demo";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents =
        "[Unit]\nDescription=New Demo\n\n[Container]\nImage=localhost/demo:new\n"
        "Label=com.example.podman-manager.managed=true\nReadOnly=true\n";

    auto systemd = std::make_shared<UnhealthyRestartSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket = false;
    options.load_image_archive = false;

    pm::DeploymentOrchestrator orchestrator{installer, systemd, options};
    auto deployed = orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("unhealthy") != std::string::npos);
    assert(deployed.error().message.find("rolled back") != std::string::npos);
    assert(systemd->calls.size() == 4);

    std::ifstream in{layout.quadlet_path(getuid(), "demo.container")};
    std::stringstream contents;
    contents << in.rdbuf();
    assert(contents.str() == old_quadlet.contents);
}

[[using gentest: test]]
void test_deployment_rejects_unverified_or_unstaged_archive()
{
    TempDir temp;
    const auto archive = temp.path() / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }

    pm::QuadletInstallLayout layout;
    layout.admin_user_root = temp.path() / "quadlets";
    layout.required_owner_uid = getuid();

    pm::DeploymentBundle bundle;
    bundle.target_uid = getuid();
    bundle.service_name = "demo";
    bundle.image_archive = pm::ImageArchive{.path = archive, .expected_sha256 = ""};
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents = valid_quadlet_contents();

    auto systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket = false;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto deployed = orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("image_archive_root") != std::string::npos);

    bundle.image_archive->expected_sha256 = "abc";
    options.image_archive_root = temp.path();
    pm::DeploymentOrchestrator digest_orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    deployed = digest_orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("expected_sha256") != std::string::npos);
}

[[using gentest: test]]
void test_deployment_loads_staged_archive_with_custom_runtime_layout()
{
    TempDir temp;

    pm::RuntimeDirectoryLayout runtime_layout;
    runtime_layout.root = temp.path() / "run-user";
    const auto socket_path = runtime_layout.podman_socket_for(getuid());
    UnixListener listener{socket_path};

    const auto staging_root = temp.path() / "staging";
    std::filesystem::create_directories(staging_root / "images");
    const auto archive = staging_root / "images" / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }

    std::promise<void> ready;
    std::vector<std::string> requests;
    std::jthread server{[&](std::stop_token) {
        ready.set_value();
        for (int i = 0; i < 2; ++i)
        {
            const int client = accept(listener.fd, nullptr, nullptr);
            assert(client >= 0);
            auto request = read_http_request(client);
            requests.push_back(request);
            if (request.starts_with("GET /_ping "))
            {
                send_response(client, 200, "OK", "Libpod-API-Version: 5.0.0\r\n");
            }
            else if (request.starts_with("POST /v5.0.0/libpod/images/load "))
            {
                send_response(client, 200, R"({"Names":["localhost/demo:latest"]})");
            }
            else
            {
                send_response(client, 500, request);
            }
            close(client);
        }
    }};
    ready.get_future().wait();

    pm::QuadletInstallLayout layout;
    layout.admin_user_root = temp.path() / "quadlets";
    layout.required_owner_uid = getuid();

    pm::DeploymentBundle bundle;
    bundle.target_uid = getuid();
    bundle.service_name = "demo";
    bundle.image_archive = pm::ImageArchive{.path = archive, .expected_sha256 = ""};
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents = valid_quadlet_contents();

    auto systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.runtime_layout = runtime_layout;
    options.image_archive_root = staging_root;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto deployed = orchestrator.deploy(bundle);
    assert(deployed);
    server.join();
    assert(requests.size() == 2);
    assert(requests[1].find("\r\n\r\nfake-tar") != std::string::npos);
}

[[using gentest: test]]
void test_podman_client_against_fake_unix_server()
{
    TempDir temp;
    UnixListener listener{temp.path() / "podman.sock"};

    std::promise<void> ready;
    std::vector<std::string> requests;
    std::jthread server{[&](std::stop_token) {
        ready.set_value();
        for (int i = 0; i < 3; ++i)
        {
            const int client = accept(listener.fd, nullptr, nullptr);
            assert(client >= 0);
            auto request = read_http_request(client);
            requests.push_back(request);
            if (request.starts_with("GET /_ping "))
            {
                send_response(client, 200, "OK", "Libpod-API-Version: 5.0.0\r\n");
            }
            else if (request.starts_with("POST /v5.0.0/libpod/containers/create "))
            {
                send_response(client, 201, R"({"Id":"abc"})");
            }
            else if (request.starts_with("POST /v5.0.0/libpod/containers/demo/start "))
            {
                send_response(client, 204, "");
            }
            else
            {
                send_response(client, 500, request);
            }
            close(client);
        }
    }};
    ready.get_future().wait();

    pm::PodmanTarget target{.uid = getuid(),
                            .user_name = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};
    pm::PodmanClient client{target, pm::ClientOptions{.timeout = std::chrono::milliseconds{2000}}};
    assert(client.ping());

    pm::ContainerSpec spec;
    spec.name = "demo";
    spec.image = "busybox";
    assert(client.create_container(spec));
    assert(client.start_container("demo"));

    server.join();
    assert(requests.size() == 3);
    assert(requests[1].find(R"("image":"busybox")") != std::string::npos);
}

[[using gentest: test]]
void test_podman_client_load_image_archive()
{
    TempDir temp;
    const auto archive = temp.path() / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }

    UnixListener listener{temp.path() / "podman.sock"};
    std::promise<void> ready;
    std::string request;
    std::jthread server{[&](std::stop_token) {
        ready.set_value();
        const int client = accept(listener.fd, nullptr, nullptr);
        assert(client >= 0);
        request = read_http_request(client);
        send_response(client, 200, R"({"Names":["localhost/demo:latest"]})");
        close(client);
    }};
    ready.get_future().wait();

    pm::PodmanTarget target{.uid = getuid(),
                            .user_name = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};
    pm::PodmanClient client{target, pm::ClientOptions{.timeout = std::chrono::milliseconds{2000}}};
    auto loaded = client.load_image_archive(archive);
    assert(loaded);

    server.join();
    assert(request.starts_with("POST /v5.0.0/libpod/images/load "));
    assert(request.find("Content-Type: application/x-tar") != std::string::npos);
    assert(request.find("\r\n\r\nfake-tar") != std::string::npos);
}

#ifndef PODMAN_MANAGER_USE_GENTEST
int main()
{
    test_container_spec_json();
    test_container_spec_validation_edges();
    test_url_codec();
    test_socket_validation();
    test_systemd_args();
    test_quadlet_policy_and_install();
    test_deployment_orchestrator_installs_and_restarts();
    test_deployment_rolls_back_when_restart_fails();
    test_deployment_rolls_back_when_restart_status_is_unhealthy();
    test_deployment_rejects_unverified_or_unstaged_archive();
    test_deployment_loads_staged_archive_with_custom_runtime_layout();
    test_podman_client_against_fake_unix_server();
    test_podman_client_load_image_archive();

    std::cout << "podman_manager_tests passed\n";
    return 0;
}
#endif
