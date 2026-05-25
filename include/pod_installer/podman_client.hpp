#pragma once

#include "pod_installer/container_spec.hpp"
#include "pod_installer/http.hpp"
#include "pod_installer/target.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pod_installer {
struct ClientOptions {
    std::chrono::milliseconds timeout{5000};
    std::string               base_url{"http://d"};
};

class PodmanClient {
  public:
    explicit PodmanClient(PodmanTarget target, ClientOptions options = {});

    [[nodiscard]] const PodmanTarget &target() const noexcept;
    [[nodiscard]] std::string         versioned_path(std::string_view libpod_path) const;

    Result<HttpResponse> request(const HttpRequest &request) const;
    Result<HttpResponse> ping() const;
    Result<HttpResponse> info() const;
    Result<HttpResponse> list_containers(bool all, const std::vector<std::string> &label_filters = {}) const;
    Result<HttpResponse> create_container(const ContainerSpec &spec) const;
    Result<HttpResponse> start_container(std::string_view name) const;
    Result<HttpResponse> stop_container(std::string_view name, int timeout_seconds = 10) const;
    Result<HttpResponse> remove_container(std::string_view name, bool force = false) const;
    Result<HttpResponse> load_image_archive(const std::filesystem::path &archive_path) const;
    Result<HttpResponse> load_image_archive_fd(int archive_fd, uintmax_t archive_size) const;

  private:
    PodmanTarget  target_;
    ClientOptions options_;
};
} // namespace pod_installer
