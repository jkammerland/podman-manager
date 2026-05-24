#include "podman_manager/podman_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <curl/curl.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace podman_manager {
namespace {
class CurlGlobal {
  public:
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }

    ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal &curl_global() {
    static CurlGlobal global;
    return global;
}

struct CurlHandleDeleter {
    void operator()(CURL *handle) const noexcept { curl_easy_cleanup(handle); }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

struct SlistDeleter {
    void operator()(curl_slist *list) const noexcept { curl_slist_free_all(list); }
};

using HeaderList = std::unique_ptr<curl_slist, SlistDeleter>;

size_t write_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *body = static_cast<std::string *>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string trim_header_value(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

size_t write_header(char *ptr, size_t size, size_t nmemb, void *userdata) {
    const std::string_view line{ptr, size * nmemb};
    auto                  *headers = static_cast<std::vector<std::pair<std::string, std::string>> *>(userdata);
    const auto             colon   = line.find(':');
    if (colon != std::string_view::npos) {
        headers->emplace_back(std::string{line.substr(0, colon)}, trim_header_value(line.substr(colon + 1)));
    }
    return size * nmemb;
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

struct FdUpload {
    int       fd{};
    uintmax_t offset{};
    uintmax_t size{};
};

size_t read_fd_upload(char *buffer, size_t size, size_t nitems, void *userdata) {
    auto      *upload    = static_cast<FdUpload *>(userdata);
    const auto requested = size * nitems;
    if (requested == 0 || upload->offset >= upload->size) {
        return 0;
    }

    const auto remaining = upload->size - upload->offset;
    const auto limit     = static_cast<uintmax_t>(std::numeric_limits<ssize_t>::max());
    const auto to_read   = static_cast<size_t>(std::min<uintmax_t>({requested, remaining, limit}));
    for (;;) {
        const auto n = pread(upload->fd, buffer, to_read, static_cast<off_t>(upload->offset));
        if (n >= 0) {
            upload->offset += static_cast<uintmax_t>(n);
            return static_cast<size_t>(n);
        }
        if (errno == EINTR) {
            continue;
        }
        return CURL_READFUNC_ABORT;
    }
}

HeaderList make_headers(const std::vector<std::string> &headers) {
    curl_slist *list = nullptr;
    for (const auto &header : headers) {
        list = curl_slist_append(list, header.c_str());
    }
    return HeaderList{list};
}

Result<HttpResponse> finish_perform(CURL *curl, HttpResponse response, CURLcode rc) {
    if (rc != CURLE_OK) {
        return std::unexpected(
            make_error(ErrorKind::transport, std::string{"curl request failed: "} + curl_easy_strerror(rc), 0, 0, static_cast<int>(rc)));
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}

Result<HttpResponse> require_success(Result<HttpResponse> response, std::string_view operation) {
    if (!response) {
        return response;
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(make_error(
            ErrorKind::http, std::string(operation) + " failed with HTTP " + std::to_string(response->status) + ": " + response->body,
            response->status));
    }
    return response;
}

std::string filters_query(const std::vector<std::string> &label_filters) {
    if (label_filters.empty()) {
        return {};
    }

    std::string json  = R"({"label":[)";
    bool        first = true;
    for (const auto &filter : label_filters) {
        if (!first) {
            json.push_back(',');
        }
        first = false;
        json.push_back('"');
        for (char c : filter) {
            if (c == '"' || c == '\\') {
                json.push_back('\\');
            }
            json.push_back(c);
        }
        json.push_back('"');
    }
    json += "]}";

    return "&filters=" + url_encode_component(json);
}
} // namespace

PodmanClient::PodmanClient(PodmanTarget target, ClientOptions options) : target_{std::move(target)}, options_{std::move(options)} {}

const PodmanTarget &PodmanClient::target() const noexcept { return target_; }

std::string PodmanClient::versioned_path(std::string_view libpod_path) const {
    std::string version = target_.api_version;
    if (!version.empty() && version.front() == 'v') {
        version.erase(version.begin());
    }

    std::string path;
    if (version.empty()) {
        path = "";
    } else {
        path = "/v" + version;
    }

    if (libpod_path.empty() || libpod_path.front() != '/') {
        path.push_back('/');
    }
    path += libpod_path;
    return path;
}

Result<HttpResponse> PodmanClient::request(const HttpRequest &req) const {
    (void)curl_global();

    CurlHandle curl{curl_easy_init()};
    if (!curl) {
        return std::unexpected(make_error(ErrorKind::transport, "curl_easy_init failed"));
    }

    HttpResponse response;
    const auto   socket_path = target_.socket_path.string();
    const auto   url         = options_.base_url + req.path;
    HeaderList   headers     = make_headers(req.headers);

    curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, req.method.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(options_.timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response.headers);

    if (headers) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    }

    if (req.method == "HEAD") {
        curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
    }

    if (req.body) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, req.body->data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body->size()));
    }

    return finish_perform(curl.get(), std::move(response), curl_easy_perform(curl.get()));
}

Result<HttpResponse> PodmanClient::ping() const {
    HttpRequest req;
    req.method = "GET";
    req.path   = "/_ping";
    return require_success(request(req), "podman ping");
}

Result<HttpResponse> PodmanClient::info() const {
    HttpRequest req;
    req.method = "GET";
    req.path   = versioned_path("/libpod/info");
    return require_success(request(req), "podman info");
}

Result<HttpResponse> PodmanClient::list_containers(bool all, const std::vector<std::string> &label_filters) const {
    std::string path = versioned_path("/libpod/containers/json");
    path += all ? "?all=true" : "?all=false";
    path += filters_query(label_filters);

    HttpRequest req;
    req.method = "GET";
    req.path   = std::move(path);
    return require_success(request(req), "list containers");
}

Result<HttpResponse> PodmanClient::create_container(const ContainerSpec &spec) const {
    auto body = to_podman_create_json(spec);
    if (!body) {
        return std::unexpected(body.error());
    }

    HttpRequest req;
    req.method  = "POST";
    req.path    = versioned_path("/libpod/containers/create");
    req.headers = {"Content-Type: application/json"};
    req.body    = *std::move(body);
    return require_success(request(req), "create container");
}

Result<HttpResponse> PodmanClient::start_container(std::string_view name) const {
    const auto  encoded_name = url_encode_component(name);
    HttpRequest req;
    req.method = "POST";
    req.path   = versioned_path("/libpod/containers/" + encoded_name + "/start");
    return require_success(request(req), "start container");
}

Result<HttpResponse> PodmanClient::stop_container(std::string_view name, int timeout_seconds) const {
    if (timeout_seconds < 0) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "stop timeout must be non-negative"));
    }

    const auto  encoded_name = url_encode_component(name);
    HttpRequest req;
    req.method = "POST";
    req.path   = versioned_path("/libpod/containers/" + encoded_name + "/stop?timeout=" + std::to_string(timeout_seconds));
    return require_success(request(req), "stop container");
}

Result<HttpResponse> PodmanClient::remove_container(std::string_view name, bool force) const {
    const auto  encoded_name = url_encode_component(name);
    HttpRequest req;
    req.method = "DELETE";
    req.path   = versioned_path("/libpod/containers/" + encoded_name + "?force=" + (force ? "true" : "false"));
    return require_success(request(req), "remove container");
}

Result<HttpResponse> PodmanClient::load_image_archive(const std::filesystem::path &archive_path) const {
    FileDescriptor fd{open(archive_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (fd.get() < 0) {
        return std::unexpected(make_error(
            ErrorKind::filesystem, "failed to open image archive '" + archive_path.string() + "': " + std::strerror(errno), 0, errno));
    }

    struct stat st{};
    if (fstat(fd.get(), &st) != 0) {
        return std::unexpected(make_error(
            ErrorKind::filesystem, "failed to stat image archive '" + archive_path.string() + "': " + std::strerror(errno), 0, errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(make_error(ErrorKind::filesystem, "image archive is not a regular file: " + archive_path.string()));
    }
    if (st.st_size < 0) {
        return std::unexpected(make_error(ErrorKind::filesystem, "image archive has invalid size: " + archive_path.string()));
    }

    return load_image_archive_fd(fd.get(), static_cast<uintmax_t>(st.st_size));
}

Result<HttpResponse> PodmanClient::load_image_archive_fd(int archive_fd, uintmax_t archive_size) const {
    (void)curl_global();

    if (archive_fd < 0) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "image archive fd is invalid"));
    }
    struct stat st{};
    if (fstat(archive_fd, &st) != 0) {
        return std::unexpected(
            make_error(ErrorKind::filesystem, "failed to stat image archive fd: " + std::string{std::strerror(errno)}, 0, errno));
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(make_error(ErrorKind::filesystem, "image archive fd is not a regular file"));
    }
    if (st.st_size < 0 || std::cmp_greater(archive_size, st.st_size)) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "image archive fd size is invalid"));
    }
    if (archive_size > static_cast<uintmax_t>(std::numeric_limits<curl_off_t>::max())) {
        return std::unexpected(make_error(ErrorKind::invalid_argument, "image archive is too large for curl"));
    }

    CurlHandle curl{curl_easy_init()};
    if (!curl) {
        return std::unexpected(make_error(ErrorKind::transport, "curl_easy_init failed"));
    }

    HttpResponse response;
    const auto   socket_path = target_.socket_path.string();
    const auto   url         = options_.base_url + versioned_path("/libpod/images/load");
    HeaderList   headers     = make_headers({"Content-Type: application/x-tar"});
    FdUpload     upload{.fd = archive_fd, .size = archive_size};

    curl_easy_setopt(curl.get(), CURLOPT_UNIX_SOCKET_PATH, socket_path.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(archive_size));
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, read_fd_upload);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(options_.timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());

    return require_success(finish_perform(curl.get(), std::move(response), curl_easy_perform(curl.get())), "load image archive");
}
} // namespace podman_manager
