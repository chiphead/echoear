#include "dlna_media_server.h"
#include "dlna_resource_registry.h"
#include "dlna_http_range.h"

#include <algorithm>
#include <climits>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr char kTag[] = "DlnaMediaServer";
constexpr char kMediaPath[] = "/dlna/audio.wav";
constexpr char kResourceWildcard[] = "/dlna/*";

constexpr int64_t kHttpSendDeadlineUs = 5 * 1000 * 1000;
constexpr size_t kHttpSendChunkBytes = 4096;

// HTTPD executes ordinary URI handlers serially. Bound the complete response,
// not only each socket send(), so a slow client cannot pin a resource lease and
// stall subsequent media requests indefinitely.
bool SendAllWithDeadline(httpd_req_t* request, const char* data, size_t size,
                         int64_t deadline_us, size_t* sent_total = nullptr) {
    size_t offset = 0;
    while (offset < size) {
        if (esp_timer_get_time() >= deadline_us) return false;
        const size_t chunk = std::min(size - offset, kHttpSendChunkBytes);
        const int sent = httpd_send(request, data + offset, chunk);
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
        if (sent_total != nullptr) *sent_total += static_cast<size_t>(sent);
    }
    return true;
}

esp_err_t SendHttpResponse(httpd_req_t* request, const char* status,
                           const char* content_type, size_t content_length,
                           const char* body, size_t body_length,
                           const char* content_range = nullptr,
                           bool accept_ranges = false,
                           size_t* body_bytes_sent = nullptr) {
    if (body_bytes_sent != nullptr) *body_bytes_sent = 0;
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

    const int64_t deadline_us = esp_timer_get_time() + kHttpSendDeadlineUs;
    if (!SendAllWithDeadline(request, headers.data(), headers.size(), deadline_us)) {
        return ESP_FAIL;
    }
    if (body != nullptr && body_length > 0 &&
        !SendAllWithDeadline(request, body, body_length, deadline_us, body_bytes_sent)) {
        return ESP_FAIL;
    }
    return ESP_OK;
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
    // Keep slow/idle clients from monopolizing the single HTTPD task.
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 1;
    config.lru_purge_enable = true;

    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        ESP_LOGE(kTag, "Failed to start HTTP server on port %u", port);
        return false;
    }

    wav_data_ = wav_data;
    wav_size_ = wav_size;

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
             media_uri_.c_str(), static_cast<unsigned>(wav_size_));
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
    // Keep slow/idle clients from monopolizing the single HTTPD task.
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 1;
    config.lru_purge_enable = true;
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
        const esp_err_t stop_result = httpd_stop(server_);
        if (stop_result != ESP_OK) {
            // Keep the handle/context intact: a failed stop must not make the
            // caller believe handlers have exited and free their registry.
            ESP_LOGE(kTag, "HTTP server stop failed (%s); retaining server context",
                     esp_err_to_name(stop_result));
            return;
        }
        server_ = nullptr;
    }
    registry_ = nullptr;
    wav_data_ = nullptr;
    wav_size_ = 0;
    media_uri_.clear();
}

esp_err_t DlnaMediaServer::HandleRequest(httpd_req_t* request) {
    if (request == nullptr || request->user_ctx == nullptr) return ESP_FAIL;
    return static_cast<DlnaMediaServer*>(request->user_ctx)->ServeRequest(request);
}

esp_err_t DlnaMediaServer::ServeRequest(httpd_req_t* request) {
    if (registry_ != nullptr) {
        // req->uri may include a query string; registry keys are path-only.
        std::string request_path(request->uri);
        const size_t query = request_path.find('?');
        if (query != std::string::npos) request_path.resize(query);

        DlnaResourceView view;
        if (!registry_->Acquire(request_path, view)) {
            return SendHttpResponse(
                request, "404 Not Found", "text/plain",
                sizeof("Unknown media resource") - 1,
                "Unknown media resource", sizeof("Unknown media resource") - 1);
        }
        struct LeaseGuard {
            DlnaResourceRegistry* registry;
            uint32_t lease_id;
            ~LeaseGuard() { registry->Release(lease_id); }
        } guard{registry_, view.lease_id};
        return ServeResource(request, view.data, view.size, request_path.c_str());
    }
    return ServeResource(request, wav_data_, wav_size_, kMediaPath);
}

esp_err_t DlnaMediaServer::ServeResource(httpd_req_t* request,
                                         const uint8_t* data, size_t size,
                                         const char* path) {
    const char* method = request->method == HTTP_HEAD ? "HEAD" : "GET";
    const int64_t request_started_us = esp_timer_get_time();
    size_t range_header_len = httpd_req_get_hdr_value_len(request, "Range");
    std::string request_range;
    if (range_header_len > 0) {
        request_range.resize(range_header_len + 1, '\0');
        if (httpd_req_get_hdr_value_str(request, "Range", request_range.data(),
                                        request_range.size()) == ESP_OK) {
            request_range.resize(std::char_traits<char>::length(request_range.c_str()));
        } else {
            request_range.clear();
        }
    }
    auto log_result = [&](int status, size_t bytes) {
        const int64_t elapsed_us = esp_timer_get_time() - request_started_us;
        ESP_LOGI(kTag, "HTTP %s %s status=%d range=%s bytes=%u elapsed=%lldms",
                 method, path, status,
                 request_range.empty() ? "-" : request_range.c_str(),
                 static_cast<unsigned>(bytes),
                 static_cast<long long>(elapsed_us / 1000));
    };
    ESP_LOGI(kTag, "HTTP request %s %s range=%s",
             method, path, request_range.empty() ? "-" : request_range.c_str());

    if (data == nullptr || size == 0) {
        if (request->method == HTTP_HEAD) {
            const esp_err_t result = SendHttpResponse(
                request, "503 Service Unavailable", "text/plain",
                sizeof("No audio resource") - 1, nullptr, 0);
            log_result(503, 0);
            return result;
        }
        const esp_err_t result = SendHttpResponse(
            request, "503 Service Unavailable", "text/plain",
            sizeof("No audio resource") - 1,
            "No audio resource", sizeof("No audio resource") - 1);
        log_result(503, 0);
        return result;
    }

    size_t first = 0;
    size_t last = size - 1;
    bool partial = false;

    if (!request_range.empty()) {
        const auto range_result =
            DlnaHttpRange::Parse(request_range, size, first, last);
        if (range_result == DlnaHttpRange::Result::kUnsatisfiable) {
            const std::string content_range = "bytes */" + std::to_string(size);
            if (request->method == HTTP_HEAD) {
                const esp_err_t result = SendHttpResponse(
                    request, "416 Range Not Satisfiable", "text/plain",
                    sizeof("Invalid Range") - 1, nullptr, 0,
                    content_range.c_str());
                log_result(416, 0);
                return result;
            }
            const esp_err_t result = SendHttpResponse(
                request, "416 Range Not Satisfiable", "text/plain",
                sizeof("Invalid Range") - 1,
                "Invalid Range", sizeof("Invalid Range") - 1,
                content_range.c_str());
            log_result(416, 0);
            return result;
        }
        // For malformed, unsupported-unit, or multipart Range headers, ignore
        // the field and return the full WAV rather than incorrectly rejecting
        // an otherwise valid media request.
        partial = range_result == DlnaHttpRange::Result::kValid;
        if (!partial) {
            first = 0;
            last = size - 1;
        }
    }

    const size_t length = last - first + 1;
    if (request->method == HTTP_HEAD) {
        std::string content_range;
        if (partial) {
            content_range = "bytes " + std::to_string(first) + "-" +
                            std::to_string(last) + "/" + std::to_string(size);
        }
        const esp_err_t result = SendHttpResponse(
            request, partial ? "206 Partial Content" : "200 OK",
            "audio/wav", length, nullptr, 0,
            partial ? content_range.c_str() : nullptr, true);
        log_result(partial ? 206 : 200, 0);
        return result;
    }

    std::string content_range;
    if (partial) {
        content_range = "bytes " + std::to_string(first) + "-" +
                        std::to_string(last) + "/" + std::to_string(size);
    }
    size_t body_bytes_sent = 0;
    const esp_err_t result = SendHttpResponse(
        request, partial ? "206 Partial Content" : "200 OK",
        "audio/wav", length,
        reinterpret_cast<const char*>(data + first), length,
        partial ? content_range.c_str() : nullptr, true, &body_bytes_sent);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Media response stopped after %u/%u body bytes",
                 static_cast<unsigned>(body_bytes_sent),
                 static_cast<unsigned>(length));
    }
    log_result(partial ? 206 : 200, body_bytes_sent);
    return result;
}
