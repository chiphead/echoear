#include "dlna_smoke_test.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "dlna_control.h"
#include "dlna_device_description.h"
#include "dlna_discovery.h"
#include "dlna_media_server.h"

namespace {
constexpr char kTag[] = "DlnaSmokeTest";
constexpr uint32_t kRate = 44100;
constexpr uint32_t kSeconds = 2;
constexpr uint16_t kChannels = 2;
constexpr size_t kDataBytes = static_cast<size_t>(kRate) * kSeconds * kChannels * 2;
std::atomic<bool> s_running{false};

void Put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
}
void Put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}
uint8_t* MakeWav() {
    auto* wav = static_cast<uint8_t*>(heap_caps_malloc(44 + kDataBytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!wav) wav = static_cast<uint8_t*>(heap_caps_malloc(44 + kDataBytes, MALLOC_CAP_8BIT));
    if (!wav) return nullptr;
    std::memset(wav, 0, 44);
    std::memcpy(wav, "RIFF", 4); Put32(wav + 4, 36 + kDataBytes);
    std::memcpy(wav + 8, "WAVEfmt ", 8); Put32(wav + 16, 16);
    Put16(wav + 20, 1); Put16(wav + 22, kChannels); Put32(wav + 24, kRate);
    Put32(wav + 28, kRate * kChannels * 2); Put16(wav + 32, kChannels * 2);
    Put16(wav + 34, 16); std::memcpy(wav + 36, "data", 4); Put32(wav + 40, kDataBytes);
    auto* pcm = reinterpret_cast<int16_t*>(wav + 44);
    for (size_t i = 0; i < static_cast<size_t>(kRate) * kSeconds; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * 440.0 * i / kRate;
        const auto sample = static_cast<int16_t>(std::sin(phase) * 8000.0);
        pcm[2 * i] = sample; pcm[2 * i + 1] = sample;
    }
    return wav;
}
bool IsSonyX88(const DlnaRenderer& r) {
    std::string id = r.friendly_name + " " + r.manufacturer + " " + r.model_name + " " + r.server;
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return id.find("srs-x88") != std::string::npos;
}
void Run(std::string host) {
    ESP_LOGI(kTag, "Starting Sony SRS-X88 DLNA test; ESP32 address=%s", host.c_str());
    std::vector<DlnaRenderer> devices;
    if (!DlnaDiscovery::Discover(5000, devices)) {
        ESP_LOGE(kTag, "SSDP discovery failed"); return;
    }
    ESP_LOGI(kTag, "SSDP returned %u renderer(s)", static_cast<unsigned>(devices.size()));
    DlnaRenderer* sony = nullptr;
    for (auto& d : devices) {
        if (!DlnaDeviceDescription::FetchAndParse(d, 5000)) {
            ESP_LOGW(kTag, "Device description failed: %s", d.location.c_str()); continue;
        }
        ESP_LOGI(kTag, "Renderer: %s / %s / %s", d.friendly_name.c_str(),
                 d.manufacturer.c_str(), d.model_name.c_str());
        if (IsSonyX88(d)) { sony = &d; break; }
    }
    if (!sony) {
        ESP_LOGE(kTag, "Sony SRS-X88 not found; refusing to control another renderer"); return;
    }
    uint8_t* wav = MakeWav();
    if (!wav) { ESP_LOGE(kTag, "WAV allocation failed"); return; }
    DlnaMediaServer server;
    if (!server.Start(host, wav, 44 + kDataBytes)) {
        ESP_LOGE(kTag, "HTTP media server failed"); heap_caps_free(wav); return;
    }
    const bool accepted = DlnaControl::SetUriAndPlay(
        *sony, server.GetMediaUri(), "EchoEar Sony X88 DLNA test", 8000);
    if (accepted) {
        ESP_LOGI(kTag, "SOAP Play accepted; media endpoint stays alive for 15 seconds");
        vTaskDelay(pdMS_TO_TICKS(15000));
        DlnaControl::Stop(*sony, 5000);
    } else {
        ESP_LOGE(kTag, "SetURI/Play failed or was rejected");
    }
    server.Stop();
    heap_caps_free(wav);
    ESP_LOGI(kTag, "Test finished; SOAP success alone does not prove audible playback");
}
void Worker(void* arg) {
    auto* host = static_cast<std::string*>(arg);
    Run(std::move(*host));
    delete host;
    s_running.store(false);
    vTaskDelete(nullptr);
}
} // namespace

bool DlnaSmokeTest::Start(const std::string& host_ipv4) {
    if (host_ipv4.empty()) return false;
    bool expected = false;
    if (!s_running.compare_exchange_strong(expected, true)) return false;
    auto* host = new (std::nothrow) std::string(host_ipv4);
    if (!host) { s_running.store(false); return false; }
    if (xTaskCreate(Worker, "dlna_smoke", 8192, host, 4, nullptr) != pdPASS) {
        delete host; s_running.store(false); return false;
    }
    return true;
}
