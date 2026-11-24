#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <esp_http_server.h>

/**
 * HTTP endpoint for serving either one legacy WAV or bounded registry resources.
 * The registry must outlive this server and all HTTP handlers.
 */
class DlnaResourceRegistry;

class DlnaMediaServer {
public:
    DlnaMediaServer() = default;
    ~DlnaMediaServer();

    DlnaMediaServer(const DlnaMediaServer&) = delete;
    DlnaMediaServer& operator=(const DlnaMediaServer&) = delete;

    bool Start(const std::string& host_ipv4, const uint8_t* wav_data,
               size_t wav_size, uint16_t port = 8080);
    bool Start(const std::string& host_ipv4, DlnaResourceRegistry& registry,
               uint16_t port = 8080);
    void Stop();

    bool IsRunning() const { return server_ != nullptr; }
    const std::string& GetMediaUri() const { return media_uri_; }

private:
    static esp_err_t HandleRequest(httpd_req_t* request);
    esp_err_t ServeRequest(httpd_req_t* request);
    esp_err_t ServeResource(httpd_req_t* request, const uint8_t* data,
                            size_t size, const char* path);

    httpd_handle_t server_ = nullptr;
    DlnaResourceRegistry* registry_ = nullptr;
    const uint8_t* wav_data_ = nullptr;
    size_t wav_size_ = 0;
    std::string media_uri_;
};
