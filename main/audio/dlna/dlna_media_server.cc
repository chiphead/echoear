#include "dlna_media_server.h"
#include "dlna_resource_registry.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <string>

#include <esp_log.h>

namespace {
constexpr char kTag[] = "DlnaMediaServer";
constexpr char kMediaPath[] = "/dlna/audio.wav";
constexpr char kResourceWildcard[] = "/dlna/*";

bool ParseSize(const std::string& text, size_t& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(),
                                    [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed > static_cast<unsigned long long>(SIZE_MAX)) {
        return false;
    }
    value = static_cast<size_t>(parsed);
    return true;
}

esp_err_t SendHeadResponse(httpd_req_t* request, const char* status,
                           const char* content_type, size_t content_length,
                           const char* content_range = nullptr,
                           bool accept_ranges = false) {
    std::string headers = "HTTP/1.1 ";
    headers += status;
    headers += "\r\nContent-Type: ";
    headers += content_type;
    headers += "\r\nContent-Length: ";
    headers += std::to_string(content_length);
    headers += "\r\n";
    if (accept_ranges) headers += "Accept-Ranges: bytes\r\n";
    if (content_range != nullptr) {
        headers += "Content-Range: ";
        headers += content_range;
        headers += "\r\n";
    }
    headers += "\r\n";

    size_t sent_total = 0;
    while (sent_total < headers.size()) {
        const int sent = httpd_send(request, headers.data() + sent_total,
                                    headers.size() - sent_total);
        if (sent <= 0) return ESP_FAIL;
        sent_total += static_cast<size_t>(sent);
    }
    return ESP_OK;
}

bool ParseRange(const std::string& header, size_t total,
                size_t& first, size_t& last) {
    if (header.compare(0, 6, "bytes=") != 0 || header.find(',') != std::string::npos ||
        total == 0) {
        return false;
    }
    const std::string spec = header.substr(6);
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;

    const std::string left = spec.substr(0, dash);
    const std::string right = spec.substr(dash + 1);
    if (left.empty()) {
        size_t suffix = 0;
        if (!ParseSize(right, suffix) || suffix == 0) return false;
        first = suffix >= total ? 0 : total - suffix;
        last = total - 1;
        return true;
    }

    if (!ParseSize(left, first) || first >= total) return false;
    if (right.empty()) {
        last = total - 1;
        return true;
    }
    if (!ParseSize(right, last) || last < first) return false;
    last = std::min(last, total - 1);
    return true;
}
}  // namespace

DlnaMediaServer::~DlnaMediaServer() {
    Stop();
}

bool DlnaMediaServer::Start(const std::string& host_ipv4, const uint8_t* wav_data,
                            size_t wav_size, uint16_t port) {
    if (server_ != nullptr || host_ipv4.empty() || wav_data == nullptr ||
        wav_size < 44 || port == 0 || port == UINT16_MAX ||
        wav_size > static_cast<size_t>(INT_MAX)) {
        ESP_LOGE(kTag, "Invalid start parameters or server already running");
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = static_cast<uint16_t>(port + 1);
    config.max_uri_handlers = 4;
    config.stack_size = 6144;

    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        ESP_LOGE(kTag, "Failed to start HTTP server on port %u", port);
        return false;
    }

    wav_data_ = wav_data;
    size = wav_size;

    httpd_uri_t get_uri{};
    get_uri.uri = kMediaPath;
    get_uri.method = HTTP_GET;
    get_uri.handler = HandleRequest;
    get_uri.user_ctx = this;

    if (httpd_register_uri_handler(server_, &get_uri) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register media GET handler");
        Stop();
        return false;
    }

    httpd_uri_t head_uri{};
    head_uri.uri = kMediaPath;
    head_uri.method = HTTP_HEAD;
    head_uri.handler = HandleRequest;
    head_uri.user_ctx = this;
    if (httpd_register_uri_handler(server_, &head_uri) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register media HEAD handler");
        Stop();
        return false;
    }

    media_uri_ = "http://" + host_ipv4 + ":" + std::to_string(port) + kMediaPath;
    ESP_LOGI(kTag, "Serving WAV resource at %s (%u bytes)",
             media_uri_.c_str(), static_cast<unsigned>(size));
    return true;
}


bool DlnaMediaServer::Start(const std::string& host_ipv4,
                            DlnaResourceRegistry& registry, uint16_t port) {
    if (server_ != nullptr || host_ipv4.empty() || port == 0 || port == UINT16_MAX) {
        ESP_LOGE(kTag, "Invalid registry server start parameters");
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = static_cast<uint16_t>(port + 1);
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        ESP_LOGE(kTag, "Failed to start registry HTTP server on port %u", port);
        return false;
    }

    registry_ = &registry;
    httpd_uri_t get_uri{};
    get_uri.uri = kResourceWildcard;
    get_uri.method = HTTP_GET;
    get_uri.handler = HandleRequest;
    get_uri.user_ctx = this;
    if (httpd_register_uri_handler(server_, &get_uri) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register wildcard media GET handler");
        Stop();
        return false;
    }

    httpd_uri_t head_uri{};
    head_uri.uri = kResourceWildcard;
    head_uri.method = HTTP_HEAD;
    head_uri.handler = HandleRequest;
    head_uri.user_ctx = this;
    if (httpd_register_uri_handler(server_, &head_uri) != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register wildcard media HEAD handler");
        Stop();
        return false;
    }

    media_uri_ = "http://" + host_ipv4 + ":" + std::to_string(port) + "/dlna/";
    ESP_LOGI(kTag, "Serving registry resources on %s", media_uri_.c_str());
    return true;
}

void DlnaMediaServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    registry_ = nullptr;
    wav_data_ = nullptr;
    size = 0;
    media_uri_.clear();
}

esp_err_t DlnaMediaServer::HandleRequest(httpd_req_t* request) {
    if (request == nullptr || request->user_ctx == nullptr) return ESP_FAIL;
    return static_cast<DlnaMediaServer*>(request->user_ctx)->ServeRequest(request);
}

esp_err_t DlnaMediaServer::ServeRequest(httpd_req_t* request) {
    if (registry_ != nullptr) {
        DlnaResourceView view;
        if (!registry_->Acquire(request->uri, view)) {
            httpd_resp_set_status(request, "404 Not Found");
            return httpd_resp_send(request, "Unknown media resource", HTTPD_RESP_USE_STRLEN);
        }
        struct LeaseGuard {
            DlnaResourceRegistry* registry;
            uint32_t lease_id;
            ~LeaseGuard() { registry->Release(lease_id); }
        } guard{registry_, view.lease_id};
        return ServeResource(request, view.data, view.size, request->uri);
    }
    return ServeResource(request, wav_data_, size, kMediaPath);
}

esp_err_t DlnaMediaServer::ServeResource(httpd_req_t* request,
                                         const uint8_t* data, size_t size,
                                         const char* path) {
    const char* method = request->method == HTTP_HEAD ? "HEAD" : "GET";
    ESP_LOGI(kTag, "Renderer requested %s %s", method, path);

    if (data == nullptr || size == 0) {
        if (request->method == HTTP_HEAD) {
            return SendHeadResponse(request, "503 Service Unavailable",
                                    "text/plain", sizeof("No audio resource") - 1);
        }
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_send(request, "No audio resource", HTTPD_RESP_USE_STRLEN);
    }

    size_t first = 0;
    size_t last = size - 1;
    bool partial = false;

    const size_t range_len = httpd_req_get_hdr_value_len(request, "Range");
    if (range_len > 0) {
        std::string range(range_len + 1, '\0');
        if (httpd_req_get_hdr_value_str(request, "Range", range.data(),
                                        range.size()) != ESP_OK) {
            const std::string content_range = "bytes */" + std::to_string(size);
            if (request->method == HTTP_HEAD) {
                return SendHeadResponse(request, "416 Range Not Satisfiable",
                                        "text/plain", sizeof("Invalid Range") - 1,
                                        content_range.c_str());
            }
            httpd_resp_set_status(request, "416 Range Not Satisfiable");
            httpd_resp_set_hdr(request, "Content-Range", content_range.c_str());
            return httpd_resp_send(request, "Invalid Range", HTTPD_RESP_USE_STRLEN);
        }
        range.resize(std::char_traits<char>::length(range.c_str()));
        if (!ParseRange(range, size, first, last)) {
            const std::string content_range = "bytes */" + std::to_string(size);
            if (request->method == HTTP_HEAD) {
                return SendHeadResponse(request, "416 Range Not Satisfiable",
                                        "text/plain", sizeof("Invalid Range") - 1,
                                        content_range.c_str());
            }
            httpd_resp_set_status(request, "416 Range Not Satisfiable");
            httpd_resp_set_hdr(request, "Content-Range", content_range.c_str());
            return httpd_resp_send(request, "Invalid Range", HTTPD_RESP_USE_STRLEN);
        }
        partial = true;
    }

    const size_t length = last - first + 1;
    if (request->method == HTTP_HEAD) {
        std::string content_range;
        if (partial) {
            content_range = "bytes " + std::to_string(first) + "-" +
                            std::to_string(last) + "/" + std::to_string(size);
        }
        return SendHeadResponse(request,
                                partial ? "206 Partial Content" : "200 OK",
                                "audio/wav", length,
                                partial ? content_range.c_str() : nullptr,
                                true);
    }

    httpd_resp_set_type(request, "audio/wav");
    httpd_resp_set_hdr(request, "Accept-Ranges", "bytes");
    if (partial) {
        httpd_resp_set_status(request, "206 Partial Content");
        const std::string content_range =
            "bytes " + std::to_string(first) + "-" + std::to_string(last) +
            "/" + std::to_string(size);
        httpd_resp_set_hdr(request, "Content-Range", content_range.c_str());
    }

    const esp_err_t result = httpd_resp_send(
        request, reinterpret_cast<const char*>(data + first),
        static_cast<ssize_t>(length));
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Failed to send media bytes %u..%u",
                 static_cast<unsigned>(first), static_cast<unsigned>(last));
    } else {
        ESP_LOGI(kTag, "Served %u byte(s), range=%s",
                 static_cast<unsigned>(length), partial ? "partial" : "full");
    }
    return result;
}
