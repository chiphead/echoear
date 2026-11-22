#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <esp_http_server.h>

/**
 * Small HTTP endpoint for serving one immutable WAV resource to a DLNA renderer.
 *
 * The caller owns the buffer and must keep it valid and unchanged until Stop().
 * Start/Stop and resource changes must run outside the HTTPD handler task.
 */
class DlnaMediaServer {
public:
    DlnaMediaServer() = default;
    ~DlnaMediaServer();

    DlnaMediaServer(const DlnaMediaServer&) = delete;
    DlnaMediaServer& operator=(const DlnaMediaServer&) = delete;

    bool Start(const std::string& host_ipv4, const uint8_t* wav_data,
               size_t wav_size, uint16_t port = 8080);
    void Stop();

    bool IsRunning() const { return server_ != nullptr; }
    const std::string& GetMediaUri() const { return media_uri_; }

private:
    static esp_err_t HandleRequest(httpd_req_t* request);
    esp_err_t ServeRequest(httpd_req_t* request);

    httpd_handle_t server_ = nullptr;
    const uint8_t* wav_data_ = nullptr;
    size_t wav_size_ = 0;
    std::string media_uri_;
};
