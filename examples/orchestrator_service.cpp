#include "podman_manager/podman_manager.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
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
#include <vector>

namespace pm = podman_manager;

namespace
{
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int)
{
    g_stop = 1;
}

struct Config
{
    std::string host{"127.0.0.1"};
    uint16_t port{9090};
    std::string api_version{"5.0.0"};
    std::filesystem::path staging_root{"/var/lib/podman-manager/staging"};
    size_t max_quadlet_size{1024 * 1024};
    bool dry_run{true};
    bool validate_socket{true};
};

struct HttpRequestLine
{
    std::string method;
    std::string target;
};

struct Query
{
    std::string path;
    std::map<std::string, std::vector<std::string>> params;

    std::optional<std::string> one(std::string_view key) const
    {
        const auto it = params.find(std::string{key});
        if (it == params.end() || it->second.empty())
        {
            return std::nullopt;
        }
        return it->second.front();
    }

    std::vector<std::string> many(std::string_view key) const
    {
        const auto it = params.find(std::string{key});
        if (it == params.end())
        {
            return {};
        }
        return it->second;
    }
};

void usage(const char* argv0)
{
    std::cerr << "usage: " << argv0
              << " [--listen 127.0.0.1:9090] [--api-version 5.0.0]"
                 " [--staging-root /var/lib/podman-manager/staging]"
                 " [--execute] [--no-socket-validation]\n";
}

pm::Result<Config> parse_args(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help")
        {
            usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--execute")
        {
            config.dry_run = false;
            continue;
        }
        if (arg == "--no-socket-validation")
        {
            config.validate_socket = false;
            continue;
        }
        if ((arg == "--listen" || arg == "--api-version" || arg == "--staging-root") && i + 1 >= argc)
        {
            return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                                  arg + " requires a value"));
        }
        if (arg == "--listen")
        {
            const std::string value = argv[++i];
            const auto colon = value.rfind(':');
            if (colon == std::string::npos)
            {
                return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                                      "--listen must be HOST:PORT"));
            }
            config.host = value.substr(0, colon);
            const auto port_value = std::stoul(value.substr(colon + 1));
            if (port_value == 0 || port_value > 65535)
            {
                return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                                      "listen port is out of range"));
            }
            config.port = static_cast<uint16_t>(port_value);
            continue;
        }
        if (arg == "--api-version")
        {
            config.api_version = argv[++i];
            continue;
        }
        if (arg == "--staging-root")
        {
            config.staging_root = argv[++i];
            continue;
        }
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                              "unknown argument: " + arg));
    }
    return config;
}

pm::Result<HttpRequestLine> parse_request_line(std::string_view request)
{
    const auto end = request.find("\r\n");
    if (end == std::string_view::npos)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                              "request line is missing CRLF"));
    }

    std::istringstream in{std::string{request.substr(0, end)}};
    HttpRequestLine line;
    std::string version;
    in >> line.method >> line.target >> version;
    if (line.method.empty() || line.target.empty() || version.rfind("HTTP/", 0) != 0)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                              "malformed HTTP request line"));
    }
    return line;
}

pm::Result<Query> parse_query(std::string_view target)
{
    Query query;
    const auto question = target.find('?');
    query.path = std::string{target.substr(0, question)};
    if (question == std::string_view::npos)
    {
        return query;
    }

    std::string_view remaining = target.substr(question + 1);
    while (!remaining.empty())
    {
        const auto amp = remaining.find('&');
        const auto part = remaining.substr(0, amp);
        const auto eq = part.find('=');
        const auto raw_key = part.substr(0, eq);
        const auto raw_value = eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1);

        auto key = pm::url_decode_component(raw_key, true);
        auto value = pm::url_decode_component(raw_value, true);
        if (!key)
        {
            return std::unexpected(key.error());
        }
        if (!value)
        {
            return std::unexpected(value.error());
        }
        query.params[*key].push_back(*value);

        if (amp == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(amp + 1);
    }
    return query;
}

std::string json_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
        else if (c == '\n')
        {
            out += "\\n";
        }
        else if (c == '\r')
        {
            out += "\\r";
        }
        else if (c == '\t')
        {
            out += "\\t";
        }
        else
        {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

std::string response(int status, std::string_view body, std::string_view content_type = "application/json")
{
    const char* reason = "OK";
    if (status == 202)
    {
        reason = "Accepted";
    }
    else if (status == 400)
    {
        reason = "Bad Request";
    }
    else if (status == 404)
    {
        reason = "Not Found";
    }
    else if (status == 500)
    {
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

bool path_starts_with(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    auto child_it = child.begin();
    auto parent_it = parent.begin();
    for (; parent_it != parent.end(); ++parent_it, ++child_it)
    {
        if (child_it == child.end() || *child_it != *parent_it)
        {
            return false;
        }
    }
    return true;
}

pm::Result<std::string> read_staged_regular_file(const std::filesystem::path& staging_root,
                                                 const std::filesystem::path& path,
                                                 size_t max_size)
{
    std::error_code ec;
    const auto canonical_root = std::filesystem::weakly_canonical(staging_root, ec);
    if (ec)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                              "failed to resolve staging root: " + ec.message()));
    }
    const auto canonical_path = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                              "failed to resolve staged file: " + ec.message()));
    }
    if (!path_starts_with(canonical_path, canonical_root))
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy,
                                              "staged file is outside staging root: " + canonical_path.string()));
    }

    struct stat lst
    {
    };
    if (lstat(path.c_str(), &lst) != 0)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                              "failed to lstat staged file: " + std::string{std::strerror(errno)},
                                              0,
                                              errno));
    }
    if (S_ISLNK(lst.st_mode))
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy,
                                              "staged file must not be a symlink"));
    }

    const int raw_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                              "failed to open staged file: " + std::string{std::strerror(errno)},
                                              0,
                                              errno));
    }
    struct CloseFd
    {
        void operator()(int* fd) const noexcept
        {
            if (fd != nullptr && *fd >= 0)
            {
                close(*fd);
            }
            delete fd;
        }
    };
    std::unique_ptr<int, CloseFd> fd{new int{raw_fd}};

    struct stat st
    {
    };
    if (fstat(*fd, &st) != 0)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                              "failed to stat staged file: " + std::string{std::strerror(errno)},
                                              0,
                                              errno));
    }
    if (!S_ISREG(st.st_mode))
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy,
                                              "staged file must be a regular file"));
    }
    if (st.st_size < 0 || static_cast<uintmax_t>(st.st_size) > max_size)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::policy,
                                              "staged Quadlet file is too large"));
    }

    std::string out(static_cast<size_t>(st.st_size), '\0');
    size_t offset = 0;
    while (offset < out.size())
    {
        const auto n = read(*fd, out.data() + offset, out.size() - offset);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                                  "failed to read staged file: " +
                                                      std::string{std::strerror(errno)},
                                                  0,
                                                  errno));
        }
        if (n == 0)
        {
            return std::unexpected(pm::make_error(pm::ErrorKind::filesystem,
                                                  "staged file changed while reading"));
        }
        offset += static_cast<size_t>(n);
    }
    return out;
}

pm::Result<std::string> service_name_from_quadlet_path(const std::filesystem::path& path)
{
    const auto file_name = path.filename().string();
    auto unit = pm::service_unit_name_from_quadlet(file_name);
    if (!unit)
    {
        return std::unexpected(unit.error());
    }
    constexpr std::string_view suffix = ".service";
    return unit->substr(0, unit->size() - suffix.size());
}

pm::Result<std::string> handle_deploy_bundle(const Config& config, const Query& query)
{
    const auto user = query.one("user");
    const auto quadlet_path = query.one("quadletPath");
    if (!user || !quadlet_path)
    {
        return std::unexpected(pm::make_error(pm::ErrorKind::invalid_argument,
                                              "deploy-bundle requires user and quadletPath query parameters"));
    }

    auto target = pm::resolve_user(*user, {}, config.api_version);
    if (!target)
    {
        return std::unexpected(target.error());
    }

    auto contents = read_staged_regular_file(config.staging_root, *quadlet_path, config.max_quadlet_size);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }

    auto inferred_service = service_name_from_quadlet_path(*quadlet_path);
    if (!inferred_service)
    {
        return std::unexpected(inferred_service.error());
    }

    pm::DeploymentBundle bundle;
    bundle.target_uid = target->uid;
    bundle.service_name = query.one("service").value_or(*inferred_service);
    bundle.revision = query.one("revision").value_or("");
    bundle.quadlet.file_name = std::filesystem::path{*quadlet_path}.filename().string();
    bundle.quadlet.contents = *std::move(contents);

    if (const auto image_archive = query.one("imageArchive"))
    {
        pm::ImageArchive archive;
        archive.path = *image_archive;
        bundle.image_archive = archive;
    }

    pm::QuadletInstaller installer;
    auto systemd = std::make_shared<pm::SystemctlUserSystemdController>(config.dry_run);
    pm::DeploymentOptions options;
    options.api_version = config.api_version;
    options.validate_socket = config.validate_socket;
    options.load_image_archive = query.one("loadImage").value_or("true") != "false";
    options.restart_unit = query.one("restart").value_or("true") != "false";
    options.dry_run = config.dry_run;

    pm::DeploymentOrchestrator deployer{installer, systemd, options};
    auto deployed = deployer.deploy(bundle);
    if (!deployed)
    {
        return std::unexpected(deployed.error());
    }

    return std::string{"{\"dryRun\":"} + (deployed->dry_run ? "true" : "false") +
           ",\"targetUser\":" + json_escape(target->user_name) +
           ",\"installedQuadletPath\":" + json_escape(deployed->installed_quadlet_path.string()) +
           ",\"systemdUnit\":" + json_escape(deployed->systemd_unit) +
           ",\"jobPath\":" + json_escape(deployed->job_path.value_or("")) +
           ",\"statusError\":" + json_escape(deployed->status_error.value_or("")) + "}\n";
}

std::string handle_request(const Config& config, std::string_view raw)
{
    auto line = parse_request_line(raw);
    if (!line)
    {
        return response(400, "{\"error\":" + json_escape(line.error().message) + "}\n");
    }

    auto query = parse_query(line->target);
    if (!query)
    {
        return response(400, "{\"error\":" + json_escape(query.error().message) + "}\n");
    }

    if (line->method == "GET" && query->path == "/healthz")
    {
        return response(200, "{\"status\":\"ok\"}\n");
    }

    if (line->method == "POST" && query->path == "/v1/deploy-bundle")
    {
        auto result = handle_deploy_bundle(config, *query);
        if (!result)
        {
            return response(400, "{\"error\":" + json_escape(result.error().message) + "}\n");
        }
        return response(config.dry_run ? 200 : 202, *result);
    }

    return response(404, "{\"error\":\"not found\"}\n");
}

int create_listener(const Config& config)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        throw std::runtime_error(std::string{"socket failed: "} + std::strerror(errno));
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1)
    {
        close(fd);
        throw std::runtime_error("listen host must be an IPv4 address");
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        const auto message = std::string{"bind failed: "} + std::strerror(errno);
        close(fd);
        throw std::runtime_error(message);
    }
    if (listen(fd, 32) != 0)
    {
        const auto message = std::string{"listen failed: "} + std::strerror(errno);
        close(fd);
        throw std::runtime_error(message);
    }

    return fd;
}
}

int main(int argc, char** argv)
{
    auto config = parse_args(argc, argv);
    if (!config)
    {
        std::cerr << config.error().message << '\n';
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    int listener{};
    try
    {
        listener = create_listener(*config);
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    std::cerr << "podman-manager example listening on " << config->host << ':' << config->port
              << (config->dry_run ? " in dry-run mode" : " in execute mode") << '\n';

    while (!g_stop)
    {
        pollfd ready{};
        ready.fd = listener;
        ready.events = POLLIN;
        const int poll_rc = poll(&ready, 1, 250);
        if (poll_rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "poll failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (poll_rc == 0)
        {
            continue;
        }

        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            break;
        }

        std::string raw;
        char buffer[4096];
        const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
        if (n > 0)
        {
            raw.assign(buffer, static_cast<size_t>(n));
            const auto out = handle_request(*config, raw);
            send(client, out.data(), out.size(), MSG_NOSIGNAL);
        }
        close(client);
    }

    close(listener);
    return 0;
}
