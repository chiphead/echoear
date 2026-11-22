#include "dlna_device_description.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include <esp_http_client.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "DlnaDescription";
constexpr size_t kMaxDescriptionBytes = 24 * 1024;

struct HttpBody {
    std::string data;
    bool overflow = false;
};

esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr) return ESP_OK;
    auto* body = static_cast<HttpBody*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        const size_t length = static_cast<size_t>(event->data_len);
        if (body->data.size() + length > kMaxDescriptionBytes) {
            body->overflow = true;
            return ESP_FAIL;
        }
        body->data.append(static_cast<const char*>(event->data), length);
    }
    return ESP_OK;
}

std::string DecodeXml(std::string value) {
    const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}
    };
    for (const auto& entity : entities) {
        size_t pos = 0;
        while ((pos = value.find(entity.first, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(entity.first), entity.second);
            pos += std::strlen(entity.second);
        }
    }
    return value;
}

std::string Trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return {};
    return DecodeXml(std::string(first, last));
}

// Find a simple XML element by local name, accepting an optional namespace prefix.
std::string Element(const std::string& xml, const char* local_name, size_t from = 0) {
    const std::string wanted = std::string(":") + local_name;
    size_t open = from;
    while ((open = xml.find('<', open)) != std::string::npos) {
        if (open + 1 >= xml.size() || xml[open + 1] == '/' || xml[open + 1] == '?' ||
            xml[open + 1] == '!') {
            ++open;
            continue;
        }
        const size_t name_end = xml.find_first_of(" />\t\r\n", open + 1);
        if (name_end == std::string::npos) return {};
        const std::string tag = xml.substr(open + 1, name_end - open - 1);
        const bool match = tag == local_name ||
            (tag.size() > wanted.size() && tag.compare(tag.size() - wanted.size(), wanted.size(), wanted) == 0);
        if (!match) {
            open = name_end;
            continue;
        }
        const std::string close_tag = "</" + tag + ">";
        const size_t close = xml.find(close_tag, name_end);
        if (close == std::string::npos) return {};
        return Trim(xml.substr(name_end, close - name_end));
    }
    return {};
}

std::string ResolveUrl(const std::string& base, const std::string& control) {
    if (control.empty()) return {};
    if (control.compare(0, 7, "http://") == 0 || control.compare(0, 8, "https://") == 0) {
        return control;
    }
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return {};
    const size_t authority_end = base.find('/', scheme + 3);
    const std::string origin = authority_end == std::string::npos ? base : base.substr(0, authority_end);
    if (control[0] == '/') return origin + control;

    const size_t query = base.find_first_of("?#");
    const std::string clean_base = query == std::string::npos ? base : base.substr(0, query);
    const size_t slash = clean_base.rfind('/');
    return (slash == std::string::npos ? clean_base + "/" : clean_base.substr(0, slash + 1)) + control;
}
} // namespace

bool DlnaDeviceDescription::FetchAndParse(DlnaRenderer& renderer, uint32_t timeout_ms) {
    if (renderer.location.empty()) return false;

    HttpBody body;
    esp_http_client_config_t config{};
    config.url = renderer.location.c_str();
    config.timeout_ms = static_cast<int>(timeout_ms);
    config.event_handler = HttpEvent;
    config.user_data = &body;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "HTTP client initialization failed");
        return false;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300 || body.overflow || body.data.empty()) {
        ESP_LOGW(kTag, "Description request failed: err=%s status=%d bytes=%u overflow=%d",
                 esp_err_to_name(err), status, static_cast<unsigned>(body.data.size()), body.overflow);
        return false;
    }

    renderer.friendly_name = Element(body.data, "friendlyName");
    renderer.manufacturer = Element(body.data, "manufacturer");
    renderer.model_name = Element(body.data, "modelName");
    renderer.udn = Element(body.data, "UDN");

    size_t service_pos = 0;
    while ((service_pos = body.data.find("<service", service_pos)) != std::string::npos) {
        const size_t service_end = body.data.find("</service>", service_pos);
        if (service_end == std::string::npos) break;
        const std::string service = body.data.substr(service_pos, service_end - service_pos);
        const std::string service_type = Element(service, "serviceType");
        if (service_type.find("AVTransport") != std::string::npos) {
            renderer.av_transport_service_type = service_type;
            renderer.av_transport_control_url = ResolveUrl(
                renderer.location, Element(service, "controlURL"));
            break;
        }
        service_pos = service_end + 10;
    }

    if (renderer.av_transport_service_type.empty() ||
        renderer.av_transport_control_url.empty()) {
        ESP_LOGW(kTag, "No usable AVTransport service in %s", renderer.location.c_str());
        return false;
    }

    ESP_LOGI(kTag, "UPnP renderer: %s / %s; AVTransport=%s",
             renderer.friendly_name.c_str(), renderer.model_name.c_str(),
             renderer.av_transport_control_url.c_str());
    return true;
}
