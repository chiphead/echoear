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

std::string ResolveUrl(const std::string& base, const std::string& reference) {
    if (reference.empty()) return {};
    if (reference.compare(0, 7, "http://") == 0 ||
        reference.compare(0, 8, "https://") == 0) {
        return reference;
    }

    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return {};
    const size_t authority_start = scheme + 3;
    const size_t path_start = base.find('/', authority_start);
    const std::string origin = path_start == std::string::npos
        ? base : base.substr(0, path_start);

    if (reference[0] == '/') return origin + reference;
    if (reference[0] == '?') {
        const size_t query = base.find_first_of("?#");
        return (query == std::string::npos ? base : base.substr(0, query)) + reference;
    }

    const size_t suffix = base.find_first_of("?#");
    const std::string clean_base = suffix == std::string::npos
        ? base : base.substr(0, suffix);
    std::string directory;
    if (clean_base.size() <= origin.size()) {
        directory = origin + "/";
    } else if (clean_base.back() == '/') {
        directory = clean_base;
    } else {
        const size_t slash = clean_base.rfind('/');
        directory = slash == std::string::npos
            ? origin + "/" : clean_base.substr(0, slash + 1);
    }
    return directory + reference;
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
        // Do not mistake the enclosing <serviceList> element for a <service> entry.
        const size_t name_end = service_pos + std::strlen("<service");
        if (name_end < body.data.size() &&
            (std::isalnum(static_cast<unsigned char>(body.data[name_end])) ||
             body.data[name_end] == '_' || body.data[name_end] == ':' ||
             body.data[name_end] == '-')) {
            service_pos = name_end;
            continue;
        }

        const size_t service_end = body.data.find("</service>", name_end);
        if (service_end == std::string::npos) break;
        const std::string service = body.data.substr(service_pos, service_end - service_pos);
        const std::string service_type = Element(service, "serviceType");
        if (service_type.find("AVTransport") != std::string::npos) {
            renderer.av_transport_service_type = service_type;
            const std::string url_base = Element(body.data, "URLBase");
            const std::string resolved_base = url_base.empty()
                ? renderer.location : ResolveUrl(renderer.location, url_base);
            renderer.av_transport_control_url = ResolveUrl(
                resolved_base, Element(service, "controlURL"));
            break;
        }
        service_pos = service_end + std::strlen("</service>");
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
