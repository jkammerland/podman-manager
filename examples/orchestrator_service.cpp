#include "pod_installer/pod_installer.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pm = pod_installer;

namespace {
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

struct Config {
    std::string           host{"127.0.0.1"};
    uint16_t              port{9090};
    std::string           api_version{"5.0.0"};
    std::filesystem::path staging_root{"/var/lib/pod-installer/staging"};
    size_t                max_quadlet_size{static_cast<size_t>(1024) * 1024};
    bool                  dry_run{true};
    bool                  validate_socket{true};
};

struct HttpRequestLine {
    std::string method;
    std::string target;
};

struct Query {
    std::string                                     path;
    std::map<std::string, std::vector<std::string>> params;

    std::optional<std::string> one(std::string_view key) const {
        const auto it = params.find(std::string{key});
        if (it == params.end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second.front();
    }

    std::vector<std::string> many(std::string_view key) const {
        const auto it = params.find(std::string{key});
        if (it == params.end()) {
            return {};
        }
        return it->second;
    }
};

void usage(const char *argv0) {
    std::cerr << "usage: " << argv0
              << " [--listen 127.0.0.1:9090] [--api-version 5.0.0]"
                 " [--staging-root /var/lib/pod-installer/staging]"
                 " [--execute] [--no-socket-validation]\n";
}

pm::Result<Config> parse_args(int argc, char **argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--execute") {
            config.dry_run = false;
            continue;
        }
        if (arg == "--no-socket-validation") {
            config.validate_socket = false;
            continue;
        }
        if ((arg == "--listen" || arg == "--api-version" || arg == "--staging-root") && i + 1 >= argc) {
            return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument, arg + " requires a value"));
        }
        if (arg == "--listen") {
            const std::string value = argv[++i];
            const auto        colon = value.rfind(':');
            if (colon == std::string::npos) {
                return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument, "--listen must be HOST:PORT"));
            }
            config.host           = value.substr(0, colon);
            const auto port_value = std::stoul(value.substr(colon + 1));
            if (port_value == 0 || port_value > 65535) {
                return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument, "listen port is out of range"));
            }
            config.port = static_cast<uint16_t>(port_value);
            continue;
        }
        if (arg == "--api-version") {
            config.api_version = argv[++i];
            continue;
        }
        if (arg == "--staging-root") {
            config.staging_root = argv[++i];
            continue;
        }
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument, "unknown argument: " + arg));
    }
    return config;
}

pm::Result<HttpRequestLine> parse_request_line(std::string_view request) {
    const auto end = request.find("\r\n");
    if (end == std::string_view::npos) {
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument, "request line is missing CRLF"));
    }

    std::istringstream in{std::string{request.substr(0, end)}};
    HttpRequestLine    line;
    std::string        version;
    in >> line.method >> line.target >> version;
    if (line.method.empty() || line.target.empty() || !version.starts_with("HTTP/")) {
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument, "malformed HTTP request line"));
    }
    return line;
}

pm::Result<Query> parse_query(std::string_view target) {
    Query      query;
    const auto question = target.find('?');
    query.path          = std::string{target.substr(0, question)};
    if (question == std::string_view::npos) {
        return query;
    }

    std::string_view remaining = target.substr(question + 1);
    while (!remaining.empty()) {
        const auto amp       = remaining.find('&');
        const auto part      = remaining.substr(0, amp);
        const auto eq        = part.find('=');
        const auto raw_key   = part.substr(0, eq);
        const auto raw_value = eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1);

        auto key   = pm::url_decode_component(raw_key, true);
        auto value = pm::url_decode_component(raw_value, true);
        if (!key) {
            return std::unexpected(key.error());
        }
        if (!value) {
            return std::unexpected(value.error());
        }
        query.params[*key].push_back(*value);

        if (amp == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(amp + 1);
    }
    return query;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string response(int status, std::string_view body, std::string_view content_type = "application/json") {
    const char *reason = "OK";
    if (status == 202) {
        reason = "Accepted";
    } else if (status == 400) {
        reason = "Bad Request";
    } else if (status == 404) {
        reason = "Not Found";
    } else if (status == 500) {
        reason = "Internal Server Error";
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << body;
    return out.str();
}

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

bool path_component_is_safe(const std::filesystem::path &component) {
    const auto value = component.string();
    return !value.empty() && value != "." && value != "..";
}

pm::Result<std::filesystem::path> staged_relative_path(const std::filesystem::path &staging_root, const std::filesystem::path &requested) {
    auto normalized = requested.lexically_normal();
    if (normalized.is_absolute()) {
        const auto normalized_root = staging_root.lexically_normal();
        auto       path_it         = normalized.begin();
        auto       root_it         = normalized_root.begin();
        for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
            if (path_it == normalized.end() || *path_it != *root_it) {
                return std::unexpected(
                    pm::make_error(pm::ErrorKind::policy, "staged file is outside staging root: " + normalized.string()));
            }
        }
        std::filesystem::path relative;
        for (; path_it != normalized.end(); ++path_it) {
            relative /= *path_it;
        }
        normalized = relative;
    }
    if (normalized.empty() || normalized == ".") {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy, "staged file path is empty"));
    }
    for (const auto &component : normalized) {
        if (!path_component_is_safe(component)) {
            return std::unexpected(
                pm::make_error(pm::ErrorKind::policy, "staged file path must not contain '.' or '..': " + normalized.string()));
        }
    }
    return normalized;
}

struct OpenStagedFile {
    FileDescriptor        fd;
    size_t                size{};
    std::filesystem::path relative_path;
};

pm::Result<OpenStagedFile> open_staged_regular_file(const std::filesystem::path &staging_root, const std::filesystem::path &requested,
                                                    size_t max_size) {
    auto relative = staged_relative_path(staging_root, requested);
    if (!relative) {
        return std::unexpected(relative.error());
    }

    FileDescriptor current{open(staging_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (current.get() < 0) {
        return std::unexpected(
            pm::make_error(pm::ErrorKind::filesystem, "failed to open staging root: " + std::string{std::strerror(errno)}, 0, errno));
    }

    auto component = relative->begin();
    for (; component != relative->end(); ++component) {
        const auto     next  = std::next(component);
        const auto     name  = component->string();
        const bool     final = next == relative->end();
        FileDescriptor next_fd{openat(current.get(), name.c_str(),
                                      final ? (O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))};
        if (next_fd.get() < 0) {
            return std::unexpected(
                pm::make_error(pm::ErrorKind::filesystem,
                               "failed to open staged file component '" + name + "': " + std::string{std::strerror(errno)}, 0, errno));
        }
        current = std::move(next_fd);
    }

    struct stat st{};
    if (fstat(current.get(), &st) != 0) {
        return std::unexpected(
            pm::make_error(pm::ErrorKind::filesystem, "failed to stat staged file: " + std::string{std::strerror(errno)}, 0, errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy, "staged file must be a regular file"));
    }
    if (st.st_size < 0 || std::cmp_greater(st.st_size, max_size)) {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy, "staged file is too large"));
    }
    return OpenStagedFile{.fd = std::move(current), .size = static_cast<size_t>(st.st_size), .relative_path = *std::move(relative)};
}

pm::Result<std::string> read_staged_regular_file(const std::filesystem::path &staging_root, const std::filesystem::path &path,
                                                 size_t max_size) {
    auto file = open_staged_regular_file(staging_root, path, max_size);
    if (!file) {
        return std::unexpected(file.error());
    }

    std::string out(file->size, '\0');
    size_t      offset = 0;
    while (offset < out.size()) {
        const auto n = read(file->fd.get(), out.data() + offset, out.size() - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(
                pm::make_error(pm::ErrorKind::filesystem, "failed to read staged file: " + std::string{std::strerror(errno)}, 0, errno));
        }
        if (n == 0) {
            return std::unexpected(pm::make_error(pm::ErrorKind::filesystem, "staged file changed while reading"));
        }
        offset += static_cast<size_t>(n);
    }
    return out;
}

pm::Result<std::string> service_name_from_quadlet_path(const std::filesystem::path &path) {
    const auto file_name = path.filename().string();
    auto       unit      = pm::service_unit_name_from_quadlet(file_name);
    if (!unit) {
        return std::unexpected(unit.error());
    }
    constexpr std::string_view suffix = ".service";
    return unit->substr(0, unit->size() - suffix.size());
}

pm::Result<std::string> handle_deploy_bundle(const Config &config, const Query &query) {
    const auto user         = query.one("user");
    const auto quadlet_path = query.one("quadletPath");
    if (!user || !quadlet_path) {
        return std::unexpected(
            pm::make_error(pm::ErrorKind::invalid_argument, "deploy-bundle requires user and quadletPath query parameters"));
    }

    auto target = pm::resolve_user(*user, {}, config.api_version);
    if (!target) {
        return std::unexpected(target.error());
    }

    auto contents = read_staged_regular_file(config.staging_root, *quadlet_path, config.max_quadlet_size);
    if (!contents) {
        return std::unexpected(contents.error());
    }

    auto inferred_service = service_name_from_quadlet_path(*quadlet_path);
    if (!inferred_service) {
        return std::unexpected(inferred_service.error());
    }

    pm::DeploymentBundle bundle;
    bundle.target_uid        = target->uid;
    bundle.service_name      = query.one("service").value_or(*inferred_service);
    bundle.revision          = query.one("revision").value_or("");
    bundle.quadlet.file_name = std::filesystem::path{*quadlet_path}.filename().string();
    bundle.quadlet.contents  = *std::move(contents);

    if (const auto image_archive = query.one("imageArchive")) {
        auto staged_image = open_staged_regular_file(config.staging_root, *image_archive, 8ULL * 1024ULL * 1024ULL * 1024ULL);
        if (!staged_image) {
            return std::unexpected(staged_image.error());
        }
        pm::ImageArchive archive;
        archive.path         = staged_image->relative_path;
        bundle.image_archive = archive;
    }

    pm::QuadletInstaller  installer;
    auto                  systemd = std::make_shared<pm::SystemctlUserSystemdController>(config.dry_run);
    pm::DeploymentOptions options;
    options.api_version        = config.api_version;
    options.validate_socket    = config.validate_socket;
    options.image_archive_root = config.staging_root;
    options.load_image_archive = query.one("loadImage").value_or("true") != "false";
    options.restart_unit       = query.one("restart").value_or("true") != "false";
    options.dry_run            = config.dry_run;

    pm::DeploymentOrchestrator deployer{installer, systemd, options};
    auto                       deployed = deployer.deploy(bundle);
    if (!deployed) {
        return std::unexpected(deployed.error());
    }

    return std::string{"{\"dryRun\":"} + (deployed->dry_run ? "true" : "false") + ",\"targetUser\":" + json_escape(target->user_name) +
           ",\"installedQuadletPath\":" + json_escape(deployed->installed_quadlet_path.string()) +
           ",\"systemdUnit\":" + json_escape(deployed->systemd_unit) + ",\"jobPath\":" + json_escape(deployed->job_path.value_or("")) +
           ",\"statusError\":" + json_escape(deployed->status_error.value_or("")) + "}\n";
}

std::string handle_request(const Config &config, std::string_view raw) {
    auto line = parse_request_line(raw);
    if (!line) {
        return response(400, "{\"error\":" + json_escape(line.error().message) + "}\n");
    }

    auto query = parse_query(line->target);
    if (!query) {
        return response(400, "{\"error\":" + json_escape(query.error().message) + "}\n");
    }

    if (line->method == "GET" && query->path == "/healthz") {
        return response(200, "{\"status\":\"ok\"}\n");
    }

    if (line->method == "POST" && query->path == "/v1/deploy-bundle") {
        auto result = handle_deploy_bundle(config, *query);
        if (!result) {
            return response(400, "{\"error\":" + json_escape(result.error().message) + "}\n");
        }
        return response(config.dry_run ? 200 : 202, *result);
    }

    return response(404, "{\"error\":\"not found\"}\n");
}

int create_listener(const Config &config) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string{"socket failed: "} + std::strerror(errno));
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(config.port);
    if (inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("listen host must be an IPv4 address");
    }

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        const auto message = std::string{"bind failed: "} + std::strerror(errno);
        close(fd);
        throw std::runtime_error(message);
    }
    if (listen(fd, 32) != 0) {
        const auto message = std::string{"listen failed: "} + std::strerror(errno);
        close(fd);
        throw std::runtime_error(message);
    }

    return fd;
}
} // namespace

int main(int argc, char **argv) {
    auto config = parse_args(argc, argv);
    if (!config) {
        std::cerr << config.error().message << '\n';
        usage(argv[0]);
        return 2;
    }

    (void)std::signal(SIGINT, handle_signal);
    (void)std::signal(SIGTERM, handle_signal);

    int listener{};
    try {
        listener = create_listener(*config);
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    std::cerr << "pod-installer example listening on " << config->host << ':' << config->port
              << (config->dry_run ? " in dry-run mode" : " in execute mode") << '\n';

    while (!g_stop) {
        pollfd ready{};
        ready.fd          = listener;
        ready.events      = POLLIN;
        const int poll_rc = poll(&ready, 1, 250);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (poll_rc == 0) {
            continue;
        }

        sockaddr_in peer{};
        socklen_t   peer_len = sizeof(peer);
        const int   client   = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            break;
        }

        std::string   raw;
        char          buffer[4096];
        const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
        if (n > 0) {
            raw.assign(buffer, static_cast<size_t>(n));
            const auto out = handle_request(*config, raw);
            send(client, out.data(), out.size(), MSG_NOSIGNAL);
        }
        close(client);
    }

    close(listener);
    return 0;
}
