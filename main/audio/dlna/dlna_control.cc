#include "dlna_control.h"
#include "dlna_xml.h"

#include <algorithm>
#include <string>

#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "DlnaControl";
constexpr size_t kMaxSoapResponseBytes = 8 * 1024;

struct SoapResponse {
    std::string body;
    bool overflow = false;
    int64_t connected_us = 0;
    int64_t header_sent_us = 0;
    int64_t first_header_us = 0;
    int64_t first_data_us = 0;
    int64_t finished_us = 0;
};

esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr) return ESP_OK;
    auto* response = static_cast<SoapResponse*>(event->user_data);
    const int64_t now_us = esp_timer_get_time();
    switch (event->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            if (response->connected_us == 0) response->connected_us = now_us;
            break;
        case HTTP_EVENT_HEADER_SENT:
            if (response->header_sent_us == 0) response->header_sent_us = now_us;
            break;
        case HTTP_EVENT_ON_HEADER:
            if (response->first_header_us == 0) response->first_header_us = now_us;
            break;
        case HTTP_EVENT_ON_DATA:
            if (event->data != nullptr && event->data_len > 0) {
                if (response->first_data_us == 0) response->first_data_us = now_us;
                const size_t length = static_cast<size_t>(event->data_len);
                if (response->body.size() + length > kMaxSoapResponseBytes) {
                    response->overflow = true;
                    return ESP_FAIL;
                }
                response->body.append(static_cast<const char*>(event->data), length);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            if (response->finished_us == 0) response->finished_us = now_us;
            break;
        default:
            break;
    }
    return ESP_OK;
}


bool Invoke(const DlnaRenderer& renderer, const char* action,
            const std::string& arguments, uint32_t timeout_ms,
            std::string* response_body = nullptr) {
    if (renderer.av_transport_control_url.empty() ||
        renderer.av_transport_service_type.empty()) {
        ESP_LOGE(kTag, "Renderer has no AVTransport control endpoint");
        return false;
    }

    const std::string soap_action =
        "\"" + renderer.av_transport_service_type + "#" + action + "\"";
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" + std::string(action) + " xmlns:u=\"" +
        DlnaXml::Escape(renderer.av_transport_service_type) + "\">" +
        arguments + "</u:" + action + "></s:Body></s:Envelope>";

    SoapResponse response;
    esp_http_client_config_t config{};
    config.url = renderer.av_transport_control_url.c_str();
    config.timeout_ms = static_cast<int>(timeout_ms);
    config.event_handler = HttpEvent;
    config.user_data = &response;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "HTTP client initialization failed");
        return false;
    }

    esp_err_t setup = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (setup == ESP_OK) {
        setup = esp_http_client_set_header(client, "Content-Type",
                                           "text/xml; charset=\"utf-8\"");
    }
    if (setup == ESP_OK) {
        setup = esp_http_client_set_header(client, "SOAPAction", soap_action.c_str());
    }
    if (setup == ESP_OK) {
        setup = esp_http_client_set_post_field(client, body.c_str(),
                                               static_cast<int>(body.size()));
    }

    const int64_t action_started_us = esp_timer_get_time();
    esp_err_t request_result = setup;
    if (setup == ESP_OK) request_result = esp_http_client_perform(client);
    const int64_t action_finished_us = esp_timer_get_time();
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    const bool soap_fault = response.body.find(":Fault") != std::string::npos ||
                            response.body.find("<Fault") != std::string::npos;
    if (request_result != ESP_OK || status < 200 || status >= 300 ||
        response.overflow || soap_fault) {
        constexpr int kMaxLoggedResponse = 256;
        const int logged_length = static_cast<int>(
            std::min(response.body.size(), static_cast<size_t>(kMaxLoggedResponse)));
        ESP_LOGW(kTag,
                 "%s failed: err=%s HTTP=%d elapsed=%lldms response_bytes=%u overflow=%d fault=%d body=%.*s",
                 action, esp_err_to_name(request_result), status,
                 static_cast<long long>((action_finished_us - action_started_us) / 1000),
                 static_cast<unsigned>(response.body.size()), response.overflow,
                 soap_fault, logged_length, response.body.c_str());
        return false;
    }

    if (response_body != nullptr) *response_body = response.body;
    const auto elapsed_ms = [action_started_us](int64_t timestamp_us) -> long long {
        return timestamp_us == 0 ? -1 :
            static_cast<long long>((timestamp_us - action_started_us) / 1000);
    };
    ESP_LOGI(kTag,
             "%s timing: connect=%lldms headers_sent=%lldms first_header=%lldms first_data=%lldms finish=%lldms total=%lldms",
             action, elapsed_ms(response.connected_us), elapsed_ms(response.header_sent_us),
             elapsed_ms(response.first_header_us), elapsed_ms(response.first_data_us),
             elapsed_ms(response.finished_us),
             static_cast<long long>((action_finished_us - action_started_us) / 1000));
    ESP_LOGI(kTag, "%s accepted by renderer (HTTP %d, elapsed=%lldms)", action, status,
             static_cast<long long>((action_finished_us - action_started_us) / 1000));
    return true;
}
}  // namespace

bool DlnaControl::SetUriAndPlay(const DlnaRenderer& renderer,
                                const std::string& media_uri,
                                const std::string& title,
                                uint32_t duration_ms,
                                uint32_t timeout_ms) {
    if (media_uri.empty()) {
        ESP_LOGE(kTag, "Media URI is empty");
        return false;
    }

    const std::string metadata =
        DlnaXml::BuildDidlMetadata(media_uri, title, duration_ms);

    const std::string set_uri_args =
        "<InstanceID>0</InstanceID><CurrentURI>" + DlnaXml::Escape(media_uri) +
        "</CurrentURI><CurrentURIMetaData>" + DlnaXml::Escape(metadata) +
        "</CurrentURIMetaData>";

    if (!Invoke(renderer, "SetAVTransportURI", set_uri_args, timeout_ms)) return false;
    return Invoke(renderer, "Play",
                  "<InstanceID>0</InstanceID><Speed>1</Speed>", timeout_ms);
}

bool DlnaControl::SetNextUri(const DlnaRenderer& renderer,
                             const std::string& media_uri,
                             const std::string& title,
                             uint32_t duration_ms,
                             uint32_t timeout_ms) {
    if (!renderer.supports_set_next_av_transport_uri || media_uri.empty()) {
        ESP_LOGW(kTag, "Renderer does not advertise SetNextAVTransportURI");
        return false;
    }

    const std::string metadata =
        DlnaXml::BuildDidlMetadata(media_uri, title, duration_ms);

    const std::string arguments =
        "<InstanceID>0</InstanceID><NextURI>" + DlnaXml::Escape(media_uri) +
        "</NextURI><NextURIMetaData>" + DlnaXml::Escape(metadata) +
        "</NextURIMetaData>";
    return Invoke(renderer, "SetNextAVTransportURI", arguments, timeout_ms);
}

bool DlnaControl::Stop(const DlnaRenderer& renderer, uint32_t timeout_ms) {
    return Invoke(renderer, "Stop", "<InstanceID>0</InstanceID>", timeout_ms);
}

bool DlnaControl::GetCurrentUri(const DlnaRenderer& renderer,
                                std::string& uri,
                                uint32_t timeout_ms) {
    std::string response;
    if (!Invoke(renderer, "GetMediaInfo",
                "<InstanceID>0</InstanceID>", timeout_ms, &response)) {
        uri.clear();
        return false;
    }
    // An empty CurrentURI is meaningful when AVTransport reports
    // NO_MEDIA_PRESENT; preserve it instead of conflating it with parse failure.
    if (!DlnaXml::ExtractValue(response, "CurrentURI", uri, true)) {
        uri.clear();
        return false;
    }
    return true;
}


bool DlnaControl::GetPositionInfo(const DlnaRenderer& renderer,
                                  std::string& track_duration,
                                  std::string& relative_time,
                                  std::string& track_uri,
                                  uint32_t timeout_ms) {
    track_duration.clear();
    relative_time.clear();
    track_uri.clear();

    std::string response;
    if (!Invoke(renderer, "GetPositionInfo",
                "<InstanceID>0</InstanceID>", timeout_ms, &response)) {
        return false;
    }

    const bool duration_ok =
        DlnaXml::ExtractValue(response, "TrackDuration", track_duration);
    const bool relative_time_ok =
        DlnaXml::ExtractValue(response, "RelTime", relative_time);
    const bool track_uri_ok =
        DlnaXml::ExtractValue(response, "TrackURI", track_uri, true);
    if (!duration_ok || !relative_time_ok || !track_uri_ok) {
        ESP_LOGW(kTag, "GetPositionInfo response missing fields: duration=%d rel_time=%d track_uri=%d",
                 duration_ok, relative_time_ok, track_uri_ok);
        return false;
    }

    // A successful SOAP response with zero timestamps is ambiguous: some
    // renderers really report 00:00:00, while malformed/nested XML can also
    // defeat a lightweight field extractor. Preserve a bounded raw sample so
    // device behavior can be distinguished from a parsing problem.
    if (track_duration == "00:00:00" || relative_time == "00:00:00" ||
        relative_time == "0:00:00") {
        constexpr size_t kDiagnosticResponseBytes = 1200;
        const size_t logged_length =
            std::min(response.size(), kDiagnosticResponseBytes);
        ESP_LOGW(kTag,
                 "GetPositionInfo returned zero time field(s): duration=%s rel_time=%s response_bytes=%u response=%.*s",
                 track_duration.c_str(), relative_time.c_str(),
                 static_cast<unsigned>(response.size()),
                 static_cast<int>(logged_length), response.c_str());
    }
    return true;
}

bool DlnaControl::GetTransportState(const DlnaRenderer& renderer,
                                    DlnaTransportState& state,
                                    uint32_t timeout_ms) {
    std::string response;
    if (!Invoke(renderer, "GetTransportInfo",
                "<InstanceID>0</InstanceID>", timeout_ms, &response)) {
        state = DlnaTransportState::kUnknown;
        return false;
    }
    std::string raw;
    if (!DlnaXml::ExtractValue(response, "CurrentTransportState", raw)) {
        state = DlnaTransportState::kUnknown;
        return false;
    }
    if (raw == "PLAYING") state = DlnaTransportState::kPlaying;
    else if (raw == "STOPPED") state = DlnaTransportState::kStopped;
    else if (raw == "NO_MEDIA_PRESENT") state = DlnaTransportState::kNoMediaPresent;
    else if (raw == "PAUSED_PLAYBACK" || raw == "PAUSED_RECORDING")
        state = DlnaTransportState::kPaused;
    else if (raw == "TRANSITIONING")
        state = DlnaTransportState::kTransitioning;
    else state = DlnaTransportState::kUnknown;
    return true;
}
