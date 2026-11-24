#include "dlna_discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <utility>
#include <unistd.h>

#include <esp_timer.h>

#include <esp_log.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

namespace {
constexpr char kTag[] = "DlnaDiscovery";
constexpr char kSsdpAddress[] = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;
constexpr char kSearchRequest[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
    "\r\n";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string HeaderValue(const std::string& response, const char* name) {
    const std::string needle = Lower(std::string(name) + ":");
    size_t line_start = 0;
    while (line_start < response.size()) {
        const size_t line_end = response.find("\r\n", line_start);
        const size_t end = line_end == std::string::npos ? response.size() : line_end;
        const std::string line = response.substr(line_start, end - line_start);
        if (Lower(line).compare(0, needle.size(), needle) == 0) {
            size_t value_start = line.find(':') + 1;
            while (value_start < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[value_start]))) {
                ++value_start;
            }
            return line.substr(value_start);
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_start = line_end + 2;
    }
    return {};
}
}  // namespace

bool DlnaDiscovery::Discover(uint32_t timeout_ms, std::vector<DlnaRenderer>& renderers) {
    renderers.clear();

    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(kTag, "Failed to create SSDP socket: errno=%d", errno);
        return false;
    }

    timeval receive_timeout{};
    receive_timeout.tv_sec = 0;
    receive_timeout.tv_usec = 250 * 1000;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                   sizeof(receive_timeout)) < 0) {
        ESP_LOGE(kTag, "Failed to set SSDP receive timeout: errno=%d", errno);
        close(socket_fd);
        return false;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kSsdpPort);
    destination.sin_addr.s_addr = inet_addr(kSsdpAddress);

    const int sent = sendto(socket_fd, kSearchRequest, sizeof(kSearchRequest) - 1, 0,
                            reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    if (sent != static_cast<int>(sizeof(kSearchRequest) - 1)) {
        ESP_LOGE(kTag, "Failed to send SSDP M-SEARCH: errno=%d", errno);
        close(socket_fd);
        return false;
    }

    ESP_LOGI(kTag, "SSDP M-SEARCH sent; waiting up to %lu ms",
             static_cast<unsigned long>(timeout_ms));

    const uint32_t wait_ms = std::max<uint32_t>(timeout_ms, 2500);
    const int64_t deadline_us = esp_timer_get_time() +
                                static_cast<int64_t>(wait_ms) * 1000;
    std::array<char, 2048> buffer{};

    while (true) {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            break;
        }

        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(remaining_us / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remaining_us % 1000000);

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);
        const int ready = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready == 0) {
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(kTag, "SSDP select failed: errno=%d", errno);
            close(socket_fd);
            return false;
        }

        sockaddr_in source{};
        socklen_t source_length = sizeof(source);
        const int received = recvfrom(socket_fd, buffer.data(), buffer.size() - 1, 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_length);
        if (received < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                continue;
            }
            ESP_LOGE(kTag, "SSDP receive failed: errno=%d", errno);
            close(socket_fd);
            return false;
        }

        const std::string response(buffer.data(), static_cast<size_t>(received));
        const std::string search_target = Lower(HeaderValue(response, "ST"));
        const std::string location = HeaderValue(response, "LOCATION");
        if (location.empty() ||
            search_target.find("urn:schemas-upnp-org:device:mediarenderer") ==
                std::string::npos) {
            continue;
        }

        const bool duplicate = std::any_of(
            renderers.begin(), renderers.end(),
            [&location](const DlnaRenderer& item) { return item.location == location; });
        if (duplicate) {
            continue;
        }

        DlnaRenderer renderer;
        renderer.location = location;
        renderer.usn = HeaderValue(response, "USN");
        renderer.server = HeaderValue(response, "SERVER");

        char address[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)) != nullptr) {
            renderer.source_address = address;
        }

        ESP_LOGI(kTag, "Found UPnP renderer at %s (%s)",
                 renderer.source_address.c_str(), renderer.location.c_str());
        renderers.push_back(std::move(renderer));
    }

    close(socket_fd);
    ESP_LOGI(kTag, "SSDP discovery finished: %u renderer(s)",
             static_cast<unsigned>(renderers.size()));
    return true;
}
