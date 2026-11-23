#include "dlna_control.h"
#include "dlna_xml.h"

#include <algorithm>
#include <string>

#include <esp_http_client.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "DlnaControl";
constexpr size_t kMaxSoapResponseBytes = 8 * 1024;

struct SoapResponse {
    std::string body;
    bool overflow = false;
};

esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr) return ESP_OK;
    auto* response = static_cast<SoapResponse*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        const size_t length = static_cast<size_t>(event->data_len);
        if (response->body.size() + length > kMaxSoapResponseBytes) {
            response->overflow = true;
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char*>(event->data), length);
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

    esp_err_t request_result = setup;
    if (setup == ESP_OK) request_result = esp_http_client_perform(client);
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
                 "%s failed: err=%s HTTP=%d response_bytes=%u overflow=%d fault=%d body=%.*s",
                 action, esp_err_to_name(request_result), status,
                 static_cast<unsigned>(response.body.size()), response.overflow,
                 soap_fault, logged_length, response.body.c_str());
        return false;
    }

    if (response_body != nullptr) *response_body = response.body;
    ESP_LOGI(kTag, "%s accepted by renderer (HTTP %d)", action, status);
    return true;
}
}  // namespace

bool DlnaControl::SetUriAndPlay(const DlnaRenderer& renderer,
                                const std::string& media_uri,
                                const std::string& title,
                                uint32_t timeout_ms) {
    if (media_uri.empty()) {
        ESP_LOGE(kTag, "Media URI is empty");
        return false;
    }

    const std::string metadata = DlnaXml::BuildDidlMetadata(media_uri, title);

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
                             uint32_t timeout_ms) {
    if (!renderer.supports_set_next_av_transport_uri || media_uri.empty()) {
        ESP_LOGW(kTag, "Renderer does not advertise SetNextAVTransportURI");
        return false;
    }

    const std::string metadata =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"0\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>" + DlnaXml::Escape(title.empty() ? "EchoEar audio" : title) + "</dc:title>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/wav:*\">" +
        DlnaXml::Escape(media_uri) + "</res></item></DIDL-Lite>";

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
    if (!DlnaXml::ExtractValue(response, "CurrentURI", uri)) {
        uri.clear();
        return false;
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
    else if (raw == "STOPPED" || raw == "NO_MEDIA_PRESENT")
        state = DlnaTransportState::kStopped;
    else if (raw == "PAUSED_PLAYBACK" || raw == "PAUSED_RECORDING")
        state = DlnaTransportState::kPaused;
    else if (raw == "TRANSITIONING")
        state = DlnaTransportState::kTransitioning;
    else state = DlnaTransportState::kUnknown;
    return true;
}
