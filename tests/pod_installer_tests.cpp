#include "gentest/attributes.h"
#include "gentest/runner.h"
#include "pod_installer/pod_installer.hpp"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pm = pod_installer;

namespace {
void test_check(bool condition, const char *expression, const char *file, int line) {
    if (!condition) {
        std::ostringstream message;
        message << file << ':' << line << ": check failed: " << expression;
        throw std::runtime_error{message.str()};
    }
}

#define PM_CHECK(expr) test_check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#undef assert
#define assert(expr) PM_CHECK(expr)

std::string valid_quadlet_contents() {
    return "[Unit]\n"
           "Description=Demo\n"
           "\n"
           "[Container]\n"
           "Image=localhost/demo:latest\n"
           "Label=com.example.pod-installer.managed=true\n"
           "ReadOnly=true\n"
           "\n"
           "[Service]\n"
           "Restart=on-failure\n";
}

class FakeSystemdController final : public pm::UserSystemdController {
  public:
    mutable std::vector<std::string> calls;
    std::optional<pm::PodmanTarget>  expected_target;

    void verify_target(const pm::PodmanTarget &target) const {
        if (expected_target) {
            assert(target.uid == expected_target->uid);
            assert(target.user_name == expected_target->user_name);
            assert(target.runtime_dir == expected_target->runtime_dir);
            assert(target.socket_path == expected_target->socket_path);
            assert(target.api_version == expected_target->api_version);
            return;
        }
        assert(target.uid == getuid());
        assert(!target.user_name.empty());
        assert(!target.runtime_dir.empty());
        assert(!target.socket_path.empty());
    }

    pm::Result<void> daemon_reload(const pm::PodmanTarget &target) const override {
        verify_target(target);
        calls.emplace_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget &target, std::string_view unit) const override {
        verify_target(target);
        calls.push_back("start " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/start";
        out.final_status =
            pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "active", .sub_state = "running"};
        return out;
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget &target, std::string_view unit) const override {
        verify_target(target);
        calls.push_back("restart " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/restart";
        out.final_status =
            pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "active", .sub_state = "running"};
        return out;
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget &target, std::string_view unit) const override {
        verify_target(target);
        calls.push_back("stop " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/stop";
        out.final_status =
            pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "inactive", .sub_state = "dead"};
        return out;
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget &target, std::string_view unit) const override {
        verify_target(target);
        calls.push_back("status " + std::string{unit});
        return pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "active", .sub_state = "running"};
    }
};

class FailingRestartSystemdController final : public pm::UserSystemdController {
  public:
    mutable std::vector<std::string> calls;
    mutable int                      restart_calls{};

    pm::Result<void> daemon_reload(const pm::PodmanTarget &) const override {
        calls.emplace_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "start failed"));
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget &, std::string_view unit) const override {
        calls.push_back("restart " + std::string{unit});
        ++restart_calls;
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "restart failed"));
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "stop failed"));
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget &, std::string_view unit) const override {
        return pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "failed", .sub_state = "failed"};
    }
};

class UnhealthyRestartSystemdController final : public pm::UserSystemdController {
  public:
    mutable std::vector<std::string> calls;

    pm::Result<void> daemon_reload(const pm::PodmanTarget &) const override {
        calls.emplace_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "start not used"));
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget &, std::string_view unit) const override {
        calls.push_back("restart " + std::string{unit});
        pm::UnitOperationResult out;
        out.job_path = "/org/freedesktop/systemd1/job/restart";
        out.final_status =
            pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "failed", .sub_state = "failed"};
        return out;
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "stop not used"));
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget &, std::string_view unit) const override {
        return pm::UnitStatus{.unit = std::string{unit}, .load_state = "loaded", .active_state = "failed", .sub_state = "failed"};
    }
};

class FailingDaemonReloadSystemdController final : public pm::UserSystemdController {
  public:
    mutable std::vector<std::string> calls;

    pm::Result<void> daemon_reload(const pm::PodmanTarget &) const override {
        calls.emplace_back("daemon-reload");
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "daemon-reload failed"));
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "start not used"));
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "restart not used"));
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "stop not used"));
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "status not used"));
    }
};

class StatusFailingSystemdController final : public pm::UserSystemdController {
  public:
    mutable std::vector<std::string> calls;

    pm::Result<void> daemon_reload(const pm::PodmanTarget &) const override {
        calls.emplace_back("daemon-reload");
        return {};
    }

    pm::Result<pm::UnitOperationResult> start_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "start not used"));
    }

    pm::Result<pm::UnitOperationResult> restart_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "restart not used"));
    }

    pm::Result<pm::UnitOperationResult> stop_unit(const pm::PodmanTarget &, std::string_view) const override {
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "stop not used"));
    }

    pm::Result<pm::UnitStatus> status(const pm::PodmanTarget &, std::string_view unit) const override {
        calls.push_back("status " + std::string{unit});
        return std::unexpected(pm::make_error(pm::ErrorKind::systemd, "status failed"));
    }
};

class TempDir {
  public:
    TempDir() {
        auto  pattern = std::filesystem::temp_directory_path() / "pod-installer-test-XXXXXX";
        auto  value   = pattern.string();
        char *created = mkdtemp(value.data());
        assert(created != nullptr);
        path_ = created;
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

struct UnixListener {
    int                   fd{-1};
    std::filesystem::path path;

    explicit UnixListener(std::filesystem::path socket_path) : path{std::move(socket_path)} {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        assert(fd >= 0);

        sockaddr_un addr{};
        addr.sun_family        = AF_UNIX;
        const auto path_string = path.string();
        assert(path_string.size() < sizeof(addr.sun_path));
        std::strncpy(addr.sun_path, path_string.c_str(), sizeof(addr.sun_path) - 1);

        std::filesystem::create_directories(path.parent_path());
        unlink(path_string.c_str());
        assert(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
        assert(listen(fd, 8) == 0);
    }

    ~UnixListener() {
        if (fd >= 0) {
            close(fd);
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

int accept_with_timeout(int listener_fd) {
    pollfd pfd{.fd = listener_fd, .events = POLLIN, .revents = 0};
    for (;;) {
        const int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        assert(ready > 0);
        assert((pfd.revents & POLLIN) != 0);
        const int client = accept(listener_fd, nullptr, nullptr);
        assert(client >= 0);
        return client;
    }
}

void rethrow_if_set(const std::exception_ptr &error) {
    if (error) {
        std::rethrow_exception(error);
    }
}

std::string read_http_request(int client) {
    std::string request;
    char        buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
        assert(n > 0);
        request.append(buffer, static_cast<size_t>(n));
    }

    const auto content_length_key = std::string{"Content-Length:"};
    const auto header_end         = request.find("\r\n\r\n");
    const auto length_pos         = request.find(content_length_key);
    if (length_pos != std::string::npos) {
        const auto line_end   = request.find("\r\n", length_pos);
        auto       raw_length = request.substr(length_pos + content_length_key.size(), line_end - (length_pos + content_length_key.size()));
        const auto length     = static_cast<size_t>(std::stoul(raw_length));
        const auto have       = request.size() - (header_end + 4);
        while (have < length && request.size() < header_end + 4 + length) {
            const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
            assert(n > 0);
            request.append(buffer, static_cast<size_t>(n));
        }
    }

    return request;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void send_response(int client, int status, std::string_view body = "OK", std::string_view extra_headers = {}) {
    const char        *reason = status == 200 ? "OK" : status == 201 ? "Created" : "No Content";
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
    out << extra_headers;
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << body;
    const auto response = out.str();
    assert(send(client, response.data(), response.size(), MSG_NOSIGNAL) > 0);
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream     in{path};
    std::stringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

pm::DeploymentBundle demo_bundle(uid_t uid = getuid()) {
    pm::DeploymentBundle bundle;
    bundle.target_uid        = uid;
    bundle.service_name      = "demo";
    bundle.revision          = "42";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents  = valid_quadlet_contents();
    return bundle;
}
} // namespace

[[using gentest: test]]
void test_container_spec_json() {
    pm::ContainerSpec spec;
    spec.name            = "svc-u1000-demo-r42";
    spec.image           = "localhost/acme/demo:1.2.3";
    spec.command         = {"/bin/sh", "-lc", "echo \"hello\" && sleep 1"};
    spec.env             = {{"ACME_MODE", "production\nnext"}};
    spec.labels          = {{"com.example.managed", "true"}};
    spec.resource_limits = pm::ResourceLimits{
        .cpu    = pm::CpuLimits{.period = 100000, .quota = 50000, .shares = 128, .cpus = "2-3"},
        .memory = pm::MemoryLimits{.limit = 268435456, .swap = 268435456},
        .pids   = pm::PidsLimits{.limit = 128},
    };

    auto json = pm::to_podman_create_json(spec);
    assert(json);
    assert(json->find(R"("name":"svc-u1000-demo-r42")") != std::string::npos);
    assert(json->find(R"("command":["/bin/sh","-lc","echo \"hello\" && sleep 1"])") != std::string::npos);
    assert(json->find(R"("ACME_MODE":"production\nnext")") != std::string::npos);
    assert(json->find(R"("cpus":"2-3")") != std::string::npos);
}

[[using gentest: test]]
void test_container_spec_validation_edges() {
    pm::ContainerSpec invalid_name;
    invalid_name.name  = "../bad";
    invalid_name.image = "busybox";
    assert(!pm::to_podman_create_json(invalid_name));

    pm::ContainerSpec invalid_cpu;
    invalid_cpu.name  = "ok";
    invalid_cpu.image = "busybox";
    pm::CpuLimits cpu;
    cpu.quota = -1;
    pm::ResourceLimits limits;
    limits.cpu                  = cpu;
    invalid_cpu.resource_limits = limits;
    auto result                 = pm::to_podman_create_json(invalid_cpu);
    assert(!result);
    assert(result.error().kind == pm::ErrorKind::policy);
}

[[using gentest: test]]
void test_url_codec() {
    assert(pm::url_encode_component("a b/c") == "a%20b%2Fc");
    auto decoded = pm::url_decode_component("a+b%2Fc", true);
    assert(decoded);
    assert(*decoded == "a b/c");
    assert(!pm::url_decode_component("%X0"));
}

[[using gentest: test]]
void test_socket_validation() {
    TempDir          temp;
    UnixListener     listener{temp.path() / "podman.sock"};
    pm::PodmanTarget target{.uid         = getuid(),
                            .user_name   = "self",
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
void test_socket_validation_edges() {
    TempDir          temp;
    pm::PodmanTarget target{.uid = getuid(), .user_name = "self", .runtime_dir = temp.path(), .socket_path = {}, .api_version = "5.0.0"};
    pm::SocketValidationOptions options;
    options.require_default_path = false;
    assert(!pm::validate_podman_socket(target, options));

    target.socket_path = temp.path() / "missing.sock";
    assert(!pm::validate_podman_socket(target, options));

    const auto regular_path = temp.path() / "regular.sock";
    {
        std::ofstream out{regular_path};
        out << "not a socket";
    }
    target.socket_path = regular_path;
    assert(!pm::validate_podman_socket(target, options));
    options.require_socket = false;
    assert(pm::validate_podman_socket(target, options));

    UnixListener listener{temp.path() / std::to_string(getuid()) / "podman" / "podman.sock"};
    target.socket_path     = listener.path;
    options.require_socket = true;
    target.uid             = getuid() + 1;
    assert(!pm::validate_podman_socket(target, options));

    target.uid                   = getuid();
    options.require_default_path = true;
    pm::RuntimeDirectoryLayout layout;
    layout.root    = temp.path();
    options.layout = layout;
    assert(pm::validate_podman_socket(target, options));
    target.socket_path = temp.path() / "other.sock";
    assert(!pm::validate_podman_socket(target, options));

    const auto symlink_path = temp.path() / "link.sock";
    std::filesystem::create_symlink(listener.path.filename(), symlink_path);
    target.socket_path           = symlink_path;
    options.require_default_path = false;
    options.require_socket       = false;
    options.reject_symlink       = false;
    assert(pm::validate_podman_socket(target, options));
}

[[using gentest: test]]
void test_systemd_args() {
    pm::UserSlicePolicy policy{.uid          = 1000,
                               .cpu_quota    = "200%",
                               .cpu_weight   = 100,
                               .memory_max   = "1G",
                               .tasks_max    = 512,
                               .allowed_cpus = "2-3"};
    auto                args = pm::build_systemctl_set_property_args(policy);
    assert(args);
    assert((*args)[0] == "systemctl");
    assert((*args)[2] == "--runtime");
    assert((*args)[3] == "user-1000.slice");
    assert(std::ranges::find(*args, "AllowedCPUs=2-3") != args->end());

    pm::PodmanTarget target;
    target.uid       = 1000;
    target.user_name = "alice";
    auto user_args   = pm::build_systemctl_user_args(target, "restart", "demo.service");
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
void test_systemd_validation_edges() {
    pm::UserSlicePolicy empty;
    empty.uid = 1000;
    assert(!pm::validate_user_slice_policy(empty));

    pm::UserSlicePolicy bad_weight;
    bad_weight.uid        = 1000;
    bad_weight.cpu_weight = 0;
    assert(!pm::validate_user_slice_policy(bad_weight));
    bad_weight.cpu_weight = 10001;
    assert(!pm::validate_user_slice_policy(bad_weight));

    pm::UserSlicePolicy bad_tasks;
    bad_tasks.uid       = 1000;
    bad_tasks.tasks_max = 0;
    assert(!pm::validate_user_slice_policy(bad_tasks));

    pm::UserSlicePolicy bad_control;
    bad_control.uid       = 1000;
    bad_control.cpu_quota = std::string{"100%\nMemoryMax=1"};
    assert(!pm::build_systemctl_set_property_args(bad_control));

    assert(pm::validate_systemd_unit_name("demo.service"));
    assert(pm::validate_systemd_unit_name("demo.socket"));
    assert(pm::validate_systemd_unit_name("demo.timer"));
    assert(pm::validate_systemd_unit_name("multi-user.target"));
    assert(!pm::validate_systemd_unit_name(""));
    assert(!pm::validate_systemd_unit_name("bad/path.service"));
    assert(!pm::validate_systemd_unit_name("bad\n.service"));

    pm::PodmanTarget target;
    target.uid = 1000;
    assert(!pm::build_systemctl_user_args(target, "restart", "demo.service"));
    target.user_name = "alice";
    assert(!pm::build_systemctl_user_args(target, "restart\nbad", "demo.service"));

    pm::SystemctlUserSystemdController dry_run{true};
    auto                               started = dry_run.start_unit(target, "demo.service");
    assert(started);
    assert(started->final_status.unit == "demo.service");
}

[[using gentest: test]]
void test_quadlet_policy_and_install() {
    pm::QuadletFile quadlet;
    quadlet.file_name = "demo.container";
    quadlet.contents  = valid_quadlet_contents();

    assert(pm::validate_quadlet_file_name(quadlet.file_name));
    auto unit = pm::service_unit_name_from_quadlet(quadlet.file_name);
    assert(unit);
    assert(*unit == "demo.service");
    assert(pm::validate_quadlet_policy(quadlet));

    pm::QuadletFile bad = quadlet;
    bad.file_name       = "../demo.container";
    assert(!pm::validate_quadlet_policy(bad));

    bad           = quadlet;
    bad.file_name = "-demo.container";
    assert(!pm::validate_quadlet_policy(bad));

    bad             = quadlet;
    bad.contents    = "[Container]\nImage=busybox\nPrivileged=true\n"
                      "Label=com.example.pod-installer.managed=true\n";
    auto privileged = pm::validate_quadlet_policy(bad);
    assert(!privileged);
    assert(privileged.error().kind == pm::ErrorKind::policy);

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nNetwork=host\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nAddDevice=/dev/kvm\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nMount=type=bind,source=/,target=/host\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nVolume=../../home/alice/.ssh:/loot:ro\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nMount=type=bind,source=../../host,target=/host\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nRootfs=/\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\nPodmanArgs=--network host\n"
                   "Label=com.example.pod-installer.managed=true\n";
    assert(!pm::validate_quadlet_policy(bad));

    pm::QuadletPolicy podman_args_policy;
    podman_args_policy.allow_podman_args = true;
    assert(!pm::validate_quadlet_policy(bad, podman_args_policy));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\n"
                   "Label=com.example.pod-installer.managed=true\n"
                   "Label=com.example.pod-installer.managed=false\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\n"
                   "Label=com.example.pod-installer.managed=true\n"
                   "\n[Service]\nExecStartPre=/bin/sh -c id\n";
    assert(!pm::validate_quadlet_policy(bad));

    bad          = quadlet;
    bad.contents = "[Container]\nImage=busybox\n";
    assert(!pm::validate_quadlet_policy(bad));

    TempDir                  temp;
    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();
    pm::QuadletInstaller installer{layout};
    auto                 installed = installer.install_for_user(getuid(), quadlet);
    assert(installed);
    assert(installed->systemd_unit == "demo.service");
    assert(std::filesystem::exists(installed->path));

    std::ifstream     in{installed->path};
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
void test_quadlet_parser_policy_edges() {
    auto parsed = pm::parse_quadlet("# comment\r\n"
                                    "[Container]\r\n"
                                    "Image = busybox\r\n"
                                    "Label=com.example.pod-installer.managed=true\r\n"
                                    "; another comment\r\n");
    assert(parsed);
    assert(parsed->values("Container", "Image").front() == "busybox");

    assert(!pm::parse_quadlet("Image=busybox\n"));
    assert(!pm::parse_quadlet("[]\nImage=busybox\n"));
    assert(!pm::parse_quadlet("[Container]\n=busybox\n"));

    pm::QuadletFile quadlet;
    quadlet.file_name = "demo.container";
    quadlet.contents  = valid_quadlet_contents();
    quadlet.contents.push_back('\0');
    assert(!pm::validate_quadlet_policy(quadlet));

    quadlet.contents = "[Container]\nImage=busybox\n"
                       "Label=com.example.pod-installer.managed=true\n"
                       "\n[Timer]\nOnBootSec=1\n";
    assert(!pm::validate_quadlet_policy(quadlet));

    quadlet.contents = "[Container]\nImage=busybox\n"
                       "Label=com.example.pod-installer.managed=true\n"
                       "EnvironmentFile=/tmp/secrets\n";
    assert(!pm::validate_quadlet_policy(quadlet));

    quadlet.contents = "[Container]\nImage=busybox\n"
                       "Label=com.example.pod-installer.managed=true\n"
                       "PodmanArgs=--log-driver journald --pull never\n";
    pm::QuadletPolicy podman_args_policy;
    podman_args_policy.allow_podman_args = true;
    assert(pm::validate_quadlet_policy(quadlet, podman_args_policy));

    const std::vector<std::string> denied_args{
        "--net=host",        "--pid host",         "--userns \"host\"",  "--privileged=1", "--privileged=t",
        "--privileged=true", "--privileged=false", "--device=/dev/fuse", "-v /:/host",
    };
    for (const auto &denied : denied_args) {
        quadlet.contents = "[Container]\nImage=busybox\n"
                           "Label=com.example.pod-installer.managed=true\n"
                           "PodmanArgs=" +
                           denied + "\n";
        assert(!pm::validate_quadlet_policy(quadlet, podman_args_policy));
    }

    quadlet.contents = "[Container]\nImage=busybox\n"
                       "Label=com.example.pod-installer.managed=true\n"
                       "PodmanArgs=--privileged\n";
    assert(!pm::validate_quadlet_policy(quadlet, podman_args_policy));
    podman_args_policy.allow_privileged = true;
    assert(pm::validate_quadlet_policy(quadlet, podman_args_policy));
    quadlet.contents = "[Container]\nImage=busybox\n"
                       "Label=com.example.pod-installer.managed=true\n"
                       "PodmanArgs=--privileged=true\n";
    assert(pm::validate_quadlet_policy(quadlet, podman_args_policy));

    quadlet.contents = "[Container]\nImage=busybox\n"
                       "Label=com.example.pod-installer.managed=true\n"
                       "Network=host\n"
                       "PID=host\n"
                       "IPCHost=true\n"
                       "UserNS=host\n"
                       "Device=/dev/fuse\n"
                       "Volume=/tmp:/host:ro\n"
                       "Mount=type=bind,source=/tmp,target=/host\n"
                       "Rootfs=/tmp/rootfs\n";
    pm::QuadletPolicy permissive;
    permissive.allow_privileged   = true;
    permissive.allow_host_network = true;
    permissive.allow_host_pid     = true;
    permissive.allow_host_ipc     = true;
    permissive.allow_host_userns  = true;
    permissive.allow_devices      = true;
    permissive.allow_root_mount   = true;
    assert(pm::validate_quadlet_policy(quadlet, permissive));
}

[[using gentest: test]]
void test_quadlet_installer_snapshot_restore_edges() {
    TempDir                  temp;
    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();
    pm::QuadletInstaller installer{layout};

    pm::QuadletFile quadlet;
    quadlet.file_name = "demo.container";
    quadlet.contents  = valid_quadlet_contents();
    auto installed    = installer.install_for_user(getuid(), quadlet);
    assert(installed);

    auto snapshot = installer.snapshot_for_user(getuid(), quadlet.file_name);
    assert(snapshot);
    assert(snapshot->existed);
    assert(snapshot->contents == quadlet.contents);

    pm::QuadletFile changed = quadlet;
    changed.contents        = "[Container]\nImage=busybox:changed\n"
                              "Label=com.example.pod-installer.managed=true\n";
    assert(installer.install_for_user(getuid(), changed));
    assert(installer.restore_for_user(getuid(), *snapshot));
    assert(read_file(installed->path) == quadlet.contents);

    auto bad_snapshot = *snapshot;
    bad_snapshot.path = temp.path() / "elsewhere.container";
    assert(!installer.restore_for_user(getuid(), bad_snapshot));

    auto absent = installer.snapshot_for_user(getuid(), "absent.container");
    assert(absent);
    assert(!absent->existed);
    pm::QuadletFile temporary = quadlet;
    temporary.file_name       = "absent.container";
    assert(installer.install_for_user(getuid(), temporary));
    assert(std::filesystem::exists(layout.quadlet_path(getuid(), temporary.file_name)));
    assert(installer.restore_for_user(getuid(), *absent));
    assert(!std::filesystem::exists(layout.quadlet_path(getuid(), temporary.file_name)));

    assert(installer.remove_for_user(getuid(), quadlet.file_name));
    assert(!std::filesystem::exists(installed->path));
    assert(installer.remove_for_user(getuid(), quadlet.file_name));

    const auto symlink_path = layout.quadlet_path(getuid(), "link.container");
    std::filesystem::create_symlink("demo.container", symlink_path);
    assert(!installer.snapshot_for_user(getuid(), "link.container"));
}

[[using gentest: test]]
void test_deployment_orchestrator_installs_and_restarts() {
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();

    pm::DeploymentBundle bundle;
    bundle.target_uid        = getuid();
    bundle.service_name      = "demo";
    bundle.revision          = "42";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents  = valid_quadlet_contents();

    auto                  systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket    = false;
    options.load_image_archive = false;
    options.dry_run            = false;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       deployed = orchestrator.deploy(bundle);
    assert(deployed);
    assert(deployed->installed_quadlet_path == temp.path() / std::to_string(getuid()) / "demo.container");
    assert(deployed->systemd_unit == "demo.service");
    assert(deployed->job_path == "/org/freedesktop/systemd1/job/restart");
    assert(deployed->status);
    const auto &status = *deployed->status;
    assert(status.unit == "demo.service");
    assert(status.load_state == "loaded");
    assert(status.active_state == "active");
    assert(status.sub_state == "running");
    assert(systemd->calls.size() == 2);
    assert(systemd->calls[0] == "daemon-reload");
    assert(systemd->calls[1] == "restart demo.service");
}

[[using gentest: test]]
void test_deployment_bundle_validation_edges() {
    auto bundle = demo_bundle(0);
    assert(!pm::validate_deployment_bundle(bundle));

    bundle = demo_bundle();
    bundle.service_name.clear();
    assert(!pm::validate_deployment_bundle(bundle));

    bundle              = demo_bundle();
    bundle.service_name = "other";
    assert(!pm::validate_deployment_bundle(bundle));

    bundle               = demo_bundle();
    bundle.image_archive = pm::ImageArchive{};
    assert(!pm::validate_deployment_bundle(bundle));
}

[[using gentest: test]]
void test_deployment_dry_run_has_no_side_effects() {
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path() / "quadlets";
    layout.required_owner_uid = getuid();

    auto                  systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.dry_run = true;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       deployed = orchestrator.deploy(demo_bundle());
    assert(deployed);
    assert(deployed->dry_run);
    assert(deployed->installed_quadlet_path == layout.quadlet_path(getuid(), "demo.container"));
    assert(systemd->calls.empty());
    assert(!std::filesystem::exists(layout.admin_user_root));

    auto archive_bundle          = demo_bundle();
    archive_bundle.image_archive = pm::ImageArchive{.path = "missing.oci.tar", .expected_sha256 = "abc"};
    options.load_image_archive   = true;
    options.image_archive_root   = temp.path() / "staging";
    pm::DeploymentOrchestrator archive_orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       rejected = archive_orchestrator.deploy(archive_bundle);
    assert(!rejected);
    assert(rejected.error().message.find("expected_sha256") != std::string::npos);
    assert(systemd->calls.empty());
    assert(!std::filesystem::exists(layout.admin_user_root));
}

[[using gentest: test]]
void test_deployment_restart_disabled_records_status_error() {
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();

    auto                  systemd = std::make_shared<StatusFailingSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket    = false;
    options.load_image_archive = false;
    options.restart_unit       = false;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       deployed = orchestrator.deploy(demo_bundle());
    assert(deployed);
    assert(!deployed->job_path);
    assert(!deployed->status);
    assert(deployed->status_error);
    const auto &status_error = *deployed->status_error;
    assert(status_error.find("status failed") != std::string::npos);
    assert(systemd->calls.size() == 2);
    assert(systemd->calls[0] == "daemon-reload");
    assert(systemd->calls[1] == "status demo.service");
    assert(std::filesystem::exists(layout.quadlet_path(getuid(), "demo.container")));
}

[[using gentest: test]]
void test_deployment_daemon_reload_failure_rolls_back_new_install() {
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();

    auto                  systemd = std::make_shared<FailingDaemonReloadSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket    = false;
    options.load_image_archive = false;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       deployed = orchestrator.deploy(demo_bundle());
    assert(!deployed);
    assert(deployed.error().message.find("daemon-reload failed") != std::string::npos);
    assert(deployed.error().message.find("rolled back") != std::string::npos);
    assert(deployed.error().message.find("rollback daemon-reload failed") != std::string::npos);
    assert(systemd->calls.size() == 2);
    assert(!std::filesystem::exists(layout.quadlet_path(getuid(), "demo.container")));
}

[[using gentest: test]]
void test_deployment_archive_path_hardening() {
    TempDir    temp;
    const auto staging_root = temp.path() / "staging";
    std::filesystem::create_directories(staging_root / "images");
    const auto archive = staging_root / "images" / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }
    const auto outside = temp.path() / "outside.oci.tar";
    {
        std::ofstream out{outside, std::ios::binary};
        out << "fake-tar";
    }
    std::filesystem::create_symlink("demo.oci.tar", staging_root / "images" / "link.oci.tar");
    const auto fifo = staging_root / "images" / "pipe.oci.tar";
    assert(mkfifo(fifo.c_str(), 0600) == 0);

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path() / "quadlets";
    layout.required_owner_uid = getuid();

    auto                  systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket    = false;
    options.image_archive_root = staging_root;

    auto deploy_with_archive = [&](std::filesystem::path image_path, std::optional<uintmax_t> max_size = std::nullopt) {
        auto bundle          = demo_bundle();
        bundle.image_archive = pm::ImageArchive{.path = std::move(image_path), .expected_sha256 = ""};
        auto local_options   = options;
        if (max_size) {
            local_options.max_image_archive_bytes = *max_size;
        }
        pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, local_options};
        return orchestrator.deploy(bundle);
    };

    auto outside_result = deploy_with_archive(outside);
    assert(!outside_result);
    assert(outside_result.error().message.find("outside configured staging root") != std::string::npos);

    auto dotdot_result = deploy_with_archive("../demo.oci.tar");
    assert(!dotdot_result);
    assert(dotdot_result.error().message.find("must not contain") != std::string::npos);

    auto embedded_dotdot_result = deploy_with_archive("images/../demo.oci.tar");
    assert(!embedded_dotdot_result);
    assert(embedded_dotdot_result.error().message.find("must not contain") != std::string::npos);

    auto absolute_embedded_dotdot_result = deploy_with_archive(staging_root / "images" / ".." / "demo.oci.tar");
    assert(!absolute_embedded_dotdot_result);
    assert(absolute_embedded_dotdot_result.error().message.find("must not contain") != std::string::npos);

    auto directory_result = deploy_with_archive("images");
    assert(!directory_result);
    assert(directory_result.error().message.find("regular file") != std::string::npos);

    auto symlink_result = deploy_with_archive("images/link.oci.tar");
    assert(!symlink_result);
    assert(symlink_result.error().message.find("regular file") != std::string::npos);

    auto fifo_result = deploy_with_archive("images/pipe.oci.tar");
    assert(!fifo_result);
    assert(fifo_result.error().message.find("regular file") != std::string::npos);

    auto too_large_result = deploy_with_archive("images/demo.oci.tar", 1);
    assert(!too_large_result);
    assert(too_large_result.error().message.find("too large") != std::string::npos);

    assert(systemd->calls.empty());
    assert(!std::filesystem::exists(layout.quadlet_path(getuid(), "demo.container")));
}

[[using gentest: test]]
void test_deployment_rolls_back_when_restart_fails() {
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();

    pm::QuadletInstaller installer{layout};
    pm::QuadletFile      old_quadlet;
    old_quadlet.file_name = "demo.container";
    old_quadlet.contents  = valid_quadlet_contents();
    assert(installer.install_for_user(getuid(), old_quadlet));

    pm::DeploymentBundle bundle;
    bundle.target_uid        = getuid();
    bundle.service_name      = "demo";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents  = "[Unit]\nDescription=New Demo\n\n[Container]\nImage=localhost/demo:new\n"
                               "Label=com.example.pod-installer.managed=true\nReadOnly=true\n";

    auto                  systemd = std::make_shared<FailingRestartSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket    = false;
    options.load_image_archive = false;

    pm::DeploymentOrchestrator orchestrator{installer, systemd, options};
    auto                       deployed = orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("rolled back") != std::string::npos);
    assert(systemd->calls.size() == 4);
    assert(systemd->calls[0] == "daemon-reload");
    assert(systemd->calls[1] == "restart demo.service");
    assert(systemd->calls[2] == "daemon-reload");
    assert(systemd->calls[3] == "restart demo.service");

    std::ifstream     in{layout.quadlet_path(getuid(), "demo.container")};
    std::stringstream contents;
    contents << in.rdbuf();
    assert(contents.str() == old_quadlet.contents);
}

[[using gentest: test]]
void test_deployment_rolls_back_when_restart_status_is_unhealthy() {
    TempDir temp;

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path();
    layout.required_owner_uid = getuid();

    pm::QuadletInstaller installer{layout};
    pm::QuadletFile      old_quadlet;
    old_quadlet.file_name = "demo.container";
    old_quadlet.contents  = valid_quadlet_contents();
    assert(installer.install_for_user(getuid(), old_quadlet));

    pm::DeploymentBundle bundle;
    bundle.target_uid        = getuid();
    bundle.service_name      = "demo";
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents  = "[Unit]\nDescription=New Demo\n\n[Container]\nImage=localhost/demo:new\n"
                               "Label=com.example.pod-installer.managed=true\nReadOnly=true\n";

    auto                  systemd = std::make_shared<UnhealthyRestartSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket    = false;
    options.load_image_archive = false;

    pm::DeploymentOrchestrator orchestrator{installer, systemd, options};
    auto                       deployed = orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("unhealthy") != std::string::npos);
    assert(deployed.error().message.find("rolled back") != std::string::npos);
    assert(systemd->calls.size() == 4);

    std::ifstream     in{layout.quadlet_path(getuid(), "demo.container")};
    std::stringstream contents;
    contents << in.rdbuf();
    assert(contents.str() == old_quadlet.contents);
}

[[using gentest: test]]
void test_deployment_rejects_unverified_or_unstaged_archive() {
    TempDir    temp;
    const auto archive = temp.path() / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path() / "quadlets";
    layout.required_owner_uid = getuid();

    pm::DeploymentBundle bundle;
    bundle.target_uid        = getuid();
    bundle.service_name      = "demo";
    bundle.image_archive     = pm::ImageArchive{.path = archive, .expected_sha256 = ""};
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents  = valid_quadlet_contents();

    auto                  systemd = std::make_shared<FakeSystemdController>();
    pm::DeploymentOptions options;
    options.validate_socket = false;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       deployed = orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("image_archive_root") != std::string::npos);
    assert(systemd->calls.empty());
    assert(!std::filesystem::exists(layout.quadlet_path(getuid(), "demo.container")));

    bundle.image_archive->expected_sha256 = "abc";
    options.image_archive_root            = temp.path();
    pm::DeploymentOrchestrator digest_orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    deployed = digest_orchestrator.deploy(bundle);
    assert(!deployed);
    assert(deployed.error().message.find("expected_sha256") != std::string::npos);
    assert(systemd->calls.empty());
    assert(!std::filesystem::exists(layout.quadlet_path(getuid(), "demo.container")));
}

[[using gentest: test]]
void test_deployment_loads_staged_archive_with_custom_runtime_layout() {
    TempDir temp;

    pm::RuntimeDirectoryLayout runtime_layout;
    runtime_layout.root      = temp.path() / "run-user";
    const auto   socket_path = runtime_layout.podman_socket_for(getuid());
    UnixListener listener{socket_path};

    const auto staging_root = temp.path() / "staging";
    std::filesystem::create_directories(staging_root / "images");
    const auto archive = staging_root / "images" / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }

    std::promise<void>       ready;
    std::vector<std::string> requests;
    std::exception_ptr       server_error;
    std::jthread             server{[&] {
        try {
            ready.set_value();
            for (int i = 0; i < 2; ++i) {
                const int client  = accept_with_timeout(listener.fd);
                auto      request = read_http_request(client);
                requests.push_back(request);
                if (request.starts_with("GET /_ping ")) {
                    send_response(client, 200, "OK", "Libpod-API-Version: 5.0.0\r\n");
                } else if (request.starts_with("POST /v5.0.0/libpod/images/load ")) {
                    send_response(client, 200, R"({"Names":["localhost/demo:latest"]})");
                } else {
                    send_response(client, 500, request);
                }
                close(client);
            }
        } catch (...) { server_error = std::current_exception(); }
    }};
    ready.get_future().wait();

    pm::QuadletInstallLayout layout;
    layout.admin_user_root    = temp.path() / "quadlets";
    layout.required_owner_uid = getuid();

    pm::DeploymentBundle bundle;
    bundle.target_uid        = getuid();
    bundle.service_name      = "demo";
    bundle.image_archive     = pm::ImageArchive{.path = archive, .expected_sha256 = ""};
    bundle.quadlet.file_name = "demo.container";
    bundle.quadlet.contents  = valid_quadlet_contents();

    auto systemd         = std::make_shared<FakeSystemdController>();
    auto expected_target = pm::resolve_uid(getuid(), runtime_layout, "5.0.0");
    assert(expected_target);
    systemd->expected_target = *expected_target;
    pm::DeploymentOptions options;
    options.runtime_layout     = runtime_layout;
    options.image_archive_root = staging_root;

    pm::DeploymentOrchestrator orchestrator{pm::QuadletInstaller{layout}, systemd, options};
    auto                       deployed = orchestrator.deploy(bundle);
    assert(deployed);
    server.join();
    rethrow_if_set(server_error);
    assert(requests.size() == 2);
    assert(requests[1].find("\r\n\r\nfake-tar") != std::string::npos);
}

[[using gentest: test]]
void test_podman_client_against_fake_unix_server() {
    TempDir      temp;
    UnixListener listener{temp.path() / "podman.sock"};

    std::promise<void>       ready;
    std::vector<std::string> requests;
    std::exception_ptr       server_error;
    std::jthread             server{[&] {
        try {
            ready.set_value();
            for (int i = 0; i < 3; ++i) {
                const int client  = accept_with_timeout(listener.fd);
                auto      request = read_http_request(client);
                requests.push_back(request);
                if (request.starts_with("GET /_ping ")) {
                    send_response(client, 200, "OK", "Libpod-API-Version: 5.0.0\r\n");
                } else if (request.starts_with("POST /v5.0.0/libpod/containers/create ")) {
                    send_response(client, 201, R"({"Id":"abc"})");
                } else if (request.starts_with("POST /v5.0.0/libpod/containers/demo/start ")) {
                    send_response(client, 204, "");
                } else {
                    send_response(client, 500, request);
                }
                close(client);
            }
        } catch (...) { server_error = std::current_exception(); }
    }};
    ready.get_future().wait();

    pm::PodmanTarget target{.uid         = getuid(),
                            .user_name   = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};
    pm::PodmanClient client{target, pm::ClientOptions{.timeout = std::chrono::milliseconds{2000}}};
    assert(client.ping());

    pm::ContainerSpec spec;
    spec.name  = "demo";
    spec.image = "busybox";
    assert(client.create_container(spec));
    assert(client.start_container("demo"));

    server.join();
    rethrow_if_set(server_error);
    assert(requests.size() == 3);
    assert(requests[1].starts_with("POST /v5.0.0/libpod/containers/create "));
    assert(requests[1].find("Content-Type: application/json") != std::string::npos);
    assert(requests[1].find(R"("name":"demo")") != std::string::npos);
    assert(requests[1].find(R"("image":"busybox")") != std::string::npos);
}

[[using gentest: test]]
void test_podman_client_load_image_archive() {
    TempDir    temp;
    const auto archive = temp.path() / "demo.oci.tar";
    {
        std::ofstream out{archive, std::ios::binary};
        out << "fake-tar";
    }

    UnixListener       listener{temp.path() / "podman.sock"};
    std::promise<void> ready;
    std::string        request;
    std::exception_ptr server_error;
    std::jthread       server{[&] {
        try {
            ready.set_value();
            const int client = accept_with_timeout(listener.fd);
            request          = read_http_request(client);
            send_response(client, 200, R"({"Names":["localhost/demo:latest"]})");
            close(client);
        } catch (...) { server_error = std::current_exception(); }
    }};
    ready.get_future().wait();

    pm::PodmanTarget target{.uid         = getuid(),
                            .user_name   = "self",
                            .runtime_dir = temp.path(),
                            .socket_path = listener.path,
                            .api_version = "5.0.0"};
    pm::PodmanClient client{target, pm::ClientOptions{.timeout = std::chrono::milliseconds{2000}}};
    auto             loaded = client.load_image_archive(archive);
    assert(loaded);

    server.join();
    rethrow_if_set(server_error);
    assert(request.starts_with("POST /v5.0.0/libpod/images/load "));
    assert(request.find("Content-Type: application/x-tar") != std::string::npos);
    assert(request.find("\r\n\r\nfake-tar") != std::string::npos);
}
