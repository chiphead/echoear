#include "dlna_playback_manager.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dlna_control.h"
#include "dlna_device_description.h"

namespace {
constexpr char kTag[] = "DlnaPlayback";
constexpr UBaseType_t kQueueDepth = 3;
constexpr uint32_t kDiscoveryTimeoutMs = 4000;
constexpr uint32_t kControlTimeoutMs = 5000;

bool IsSonyX88(const DlnaRenderer& renderer) {
    std::string identity = renderer.friendly_name + " " + renderer.manufacturer +
                           " " + renderer.model_name + " " + renderer.server;
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return identity.find("srs-x88") != std::string::npos;
}
}  // namespace

DlnaPlaybackManager::~DlnaPlaybackManager() {
    // The manager is owned by Application for the lifetime of the firmware.
    // Avoid deleting a live FreeRTOS worker from an arbitrary destructor context.
    if (resource_queue_ != nullptr) {
        vQueueDelete(resource_queue_);
        resource_queue_ = nullptr;
    }
    if (current_wav_ != nullptr) {
        heap_caps_free(current_wav_);
        current_wav_ = nullptr;
    }
}

bool DlnaPlaybackManager::Start(const std::string& host_ipv4) {
    if (host_ipv4.empty()) return false;
    if (worker_task_ != nullptr) return true;

    host_ipv4_ = host_ipv4;
    for (size_t i = 0; i < kBufferCount; ++i) {
        if (!buffers_[i].Initialize()) {
            ESP_LOGE(kTag, "PCM buffer %u initialization failed", static_cast<unsigned>(i));
            return false;
        }
        buffer_ready_[i] = true;
    }

    resource_queue_ = xQueueCreate(kQueueDepth, sizeof(Resource));
    if (resource_queue_ == nullptr) {
        ESP_LOGE(kTag, "Unable to create resource queue");
        return false;
    }

    if (xTaskCreate(WorkerEntry, "dlna_playback", 9216, this, 4, &worker_task_) != pdPASS) {
        worker_task_ = nullptr;
        vQueueDelete(resource_queue_);
        resource_queue_ = nullptr;
        ESP_LOGE(kTag, "Unable to create DLNA worker");
        return false;
    }
    ESP_LOGI(kTag, "DLNA output worker started; host=%s", host_ipv4_.c_str());
    return true;
}

bool DlnaPlaybackManager::WritePcm(const int16_t* samples, size_t sample_count,
                                   int sample_rate, int channels) {
    if (samples == nullptr || sample_count == 0 || resource_queue_ == nullptr ||
        !sink_ready_.load()) return false;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    const uint8_t index = write_buffer_index_;
    if (!buffer_ready_[index]) return false;

    if (!buffer_active_[index]) {
        if (!buffers_[index].Begin(sample_rate, channels)) return false;
        buffer_active_[index] = true;
    }
    return buffers_[index].Append(samples, sample_count);
}

void DlnaPlaybackManager::EndUtterance() {
    Resource resource{};
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        const uint8_t index = write_buffer_index_;
        if (!buffer_active_[index] || !buffer_ready_[index]) return;

        if (!buffers_[index].FinalizeWav(&resource.wav_data, &resource.wav_size)) {
            buffers_[index].Reset();
            buffer_active_[index] = false;
            ESP_LOGW(kTag, "No complete PCM utterance to publish");
            return;
        }

        resource.buffer_index = index;
        buffer_ready_[index] = false;
        buffer_active_[index] = false;

        const uint8_t other = static_cast<uint8_t>((index + 1) % kBufferCount);
        if (buffer_ready_[other]) {
            write_buffer_index_ = other;
        } else {
            // Keep the completed resource; subsequent frames fall back to the
            // local codec until the worker recycles a buffer.
            ESP_LOGW(kTag, "Both PCM buffers are busy; next frames use local output");
        }
    }

    if (xQueueSend(resource_queue_, &resource, 0) != pdTRUE) {
        ESP_LOGW(kTag, "DLNA resource queue full; dropping completed utterance");
        heap_caps_free(resource.wav_data);
        // The worker is woken so it can recreate this buffer allocation.
        Resource recycle{};
        recycle.buffer_index = resource.buffer_index;
        xQueueSend(resource_queue_, &recycle, 0);
    }
}

void DlnaPlaybackManager::WorkerEntry(void* arg) {
    static_cast<DlnaPlaybackManager*>(arg)->Worker();
    vTaskDelete(nullptr);
}

bool DlnaPlaybackManager::FindSonyRenderer(DlnaRenderer& renderer) {
    std::vector<DlnaRenderer> candidates;
    if (!DlnaDiscovery::Discover(kDiscoveryTimeoutMs, candidates)) {
        ESP_LOGW(kTag, "SSDP discovery failed");
        return false;
    }

    for (auto& candidate : candidates) {
        if (!DlnaDeviceDescription::FetchAndParse(candidate, kControlTimeoutMs)) {
            continue;
        }
        ESP_LOGI(kTag, "Discovered renderer: %s / %s / %s",
                 candidate.friendly_name.c_str(), candidate.manufacturer.c_str(),
                 candidate.model_name.c_str());
        if (IsSonyX88(candidate)) {
            renderer = std::move(candidate);
            return true;
        }
    }
    ESP_LOGW(kTag, "Sony SRS-X88 not found");
    return false;
}

void DlnaPlaybackManager::ProcessResource(const Resource& resource) {
    if (resource.wav_data == nullptr || resource.wav_size == 0) return;

    if (!renderer_ready_) {
        heap_caps_free(resource.wav_data);
        return;
    }

    // A previous resource may still be fetched by the renderer. Stop it and
    // shut down its HTTP endpoint before releasing or replacing its WAV bytes.
    if (media_server_.IsRunning()) {
        DlnaControl::Stop(renderer_, kControlTimeoutMs);
        media_server_.Stop();
    }
    if (current_wav_ != nullptr) {
        heap_caps_free(current_wav_);
        current_wav_ = nullptr;
    }

    if (!media_server_.Start(host_ipv4_, resource.wav_data, resource.wav_size)) {
        heap_caps_free(resource.wav_data);
        ESP_LOGE(kTag, "Could not publish WAV resource");
        return;
    }

    if (!DlnaControl::SetUriAndPlay(renderer_, media_server_.GetMediaUri(),
                                    "EchoEar audio", kControlTimeoutMs)) {
        ESP_LOGE(kTag, "Renderer rejected SetAVTransportURI/Play");
        media_server_.Stop();
        heap_caps_free(resource.wav_data);
        return;
    }

    // Keep this allocation immutable and alive while the renderer may issue
    // GET/Range requests. It is released only when a later utterance replaces it.
    current_wav_ = resource.wav_data;
    ESP_LOGI(kTag, "DLNA playback command accepted (%u bytes)",
             static_cast<unsigned>(resource.wav_size));
}

void DlnaPlaybackManager::Worker() {
    TickType_t next_discovery = 0;
    while (true) {
        if (!renderer_ready_ &&
            (next_discovery == 0 || xTaskGetTickCount() >= next_discovery)) {
            renderer_ready_ = FindSonyRenderer(renderer_);
            sink_ready_.store(renderer_ready_);
            next_discovery = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }

        Resource resource{};
        if (xQueueReceive(resource_queue_, &resource, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Recreate the just-finalized accumulator in worker context. Its
            // WAV allocation is independent and remains owned by the server.
            const bool recycled = buffers_[resource.buffer_index].Initialize();
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_ready_[resource.buffer_index] = recycled;
                buffer_active_[resource.buffer_index] = false;
                if (!buffer_ready_[write_buffer_index_] && recycled) {
                    write_buffer_index_ = resource.buffer_index;
                }
            }
            if (!recycled) {
                ESP_LOGE(kTag, "Could not recycle PCM buffer %u",
                         static_cast<unsigned>(resource.buffer_index));
            }

            if (resource.wav_data != nullptr) {
                if (renderer_ready_) {
                    ProcessResource(resource);
                } else {
                    heap_caps_free(resource.wav_data);
                    ESP_LOGW(kTag, "Dropping utterance: Sony renderer is not available");
                }
            }
        }
    }
}
