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
    if (worker_task_ != nullptr) {
        stopping_.store(true);
        // Let the worker leave its bounded queue wait and release HTTP/WAV
        // resources itself; never delete a task while it may own them.
        if (worker_stopped_ != nullptr) {
            xSemaphoreTake(worker_stopped_, portMAX_DELAY);
        }
        worker_task_ = nullptr;
    }
    if (resource_queue_ != nullptr) {
        Resource pending{};
        while (xQueueReceive(resource_queue_, &pending, 0) == pdTRUE) {
            if (pending.wav_data != nullptr) heap_caps_free(pending.wav_data);
        }
        vQueueDelete(resource_queue_);
        resource_queue_ = nullptr;
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
        worker_stopped_ = nullptr;
    }
}

bool DlnaPlaybackManager::Start(const std::string& host_ipv4) {
    if (host_ipv4.empty()) return false;
    if (worker_task_ != nullptr) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            host_ipv4_ = host_ipv4;
        }
        network_available_.store(true);
        network_lost_.store(true);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        host_ipv4_ = host_ipv4;
    }
    network_available_.store(true);
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
    worker_stopped_ = xSemaphoreCreateBinary();
    if (worker_stopped_ == nullptr) {
        vQueueDelete(resource_queue_);
        resource_queue_ = nullptr;
        ESP_LOGE(kTag, "Unable to create worker completion semaphore");
        return false;
    }
    stopping_.store(false);
    recycle_mask_.store(0);

    if (xTaskCreate(WorkerEntry, "dlna_playback", 9216, this, 4, &worker_task_) != pdPASS) {
        worker_task_ = nullptr;
        vSemaphoreDelete(worker_stopped_);
        worker_stopped_ = nullptr;
        vQueueDelete(resource_queue_);
        resource_queue_ = nullptr;
        ESP_LOGE(kTag, "Unable to create DLNA worker");
        return false;
    }
    ESP_LOGI(kTag, "DLNA output worker started; host=%s", host_ipv4_.c_str());
    return true;
}

void DlnaPlaybackManager::NotifyNetworkDisconnected() {
    network_available_.store(false);
    sink_ready_.store(false);
    network_lost_.store(true);
}

bool DlnaPlaybackManager::WritePcm(const int16_t* samples, size_t sample_count,
                                   int sample_rate, int channels) {
    if (samples == nullptr || sample_count == 0) return false;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (discard_until_drain_) return true;
    if (utterance_decided_ && fallback_utterance_) return false;
    if (!utterance_decided_) {
        utterance_decided_ = true;
        if (resource_queue_ == nullptr || !sink_ready_.load()) {
            fallback_utterance_ = true;
            return false;
        }
    }

    const uint8_t index = write_buffer_index_;
    if (!buffer_ready_[index]) {
        fallback_utterance_ = true;
        return false;
    }

    if (buffer_dropping_[index]) return true;
    if (!buffer_active_[index]) {
        if (!buffers_[index].Begin(sample_rate, channels)) {
            fallback_utterance_ = true;
            return false;
        }
        buffer_active_[index] = true;
    }
    size_t offset = 0;
    while (offset < sample_count) {
        const size_t remaining_samples =
            buffers_[index].GetRemainingPcmBytes() / sizeof(int16_t);
        const size_t frame_samples = static_cast<size_t>(channels);
        const size_t aligned_remaining =
            (remaining_samples / frame_samples) * frame_samples;

        if (aligned_remaining == 0) {
            if (!QueueSegmentLocked(index)) {
                buffer_dropping_[index] = true;
                ESP_LOGE(kTag, "DLNA segment queue/buffer unavailable; dropping utterance tail");
                return true;
            }
            const uint8_t other = static_cast<uint8_t>((index + 1) % kBufferCount);
            if (!buffer_ready_[other]) {
                buffer_dropping_[index] = true;
                ESP_LOGE(kTag, "No free PCM buffer for next DLNA segment; dropping utterance tail");
                return true;
            }
            index = other;
            write_buffer_index_ = index;
            if (!buffers_[index].Begin(sample_rate, channels)) {
                buffer_dropping_[index] = true;
                return true;
            }
            buffer_active_[index] = true;
            continue;
        }

        const size_t chunk_samples = std::min(sample_count - offset, aligned_remaining);
        if (!buffers_[index].Append(samples + offset, chunk_samples)) {
            buffer_dropping_[index] = true;
            ESP_LOGE(kTag, "Invalid PCM frame while splitting DLNA segment");
            return true;
        }
        offset += chunk_samples;

        if (offset < sample_count) {
            if (!QueueSegmentLocked(index)) {
                buffer_dropping_[index] = true;
                ESP_LOGE(kTag, "Could not queue completed DLNA segment");
                return true;
            }
            const uint8_t other = static_cast<uint8_t>((index + 1) % kBufferCount);
            if (!buffer_ready_[other]) {
                buffer_dropping_[index] = true;
                ESP_LOGE(kTag, "No free PCM buffer for next DLNA segment; dropping utterance tail");
                return true;
            }
            index = other;
            write_buffer_index_ = index;
            if (!buffers_[index].Begin(sample_rate, channels)) {
                buffer_dropping_[index] = true;
                return true;
            }
            buffer_active_[index] = true;
        }
    }
    return true;
}


bool DlnaPlaybackManager::QueueSegmentLocked(uint8_t index) {
    if (index >= kBufferCount || !buffer_ready_[index] || !buffer_active_[index]) return false;
    Resource resource{};
    if (!buffers_[index].FinalizeWav(&resource.wav_data, &resource.wav_size)) return false;
    resource.buffer_index = index;
    buffer_ready_[index] = false;
    buffer_active_[index] = false;
    if (xQueueSend(resource_queue_, &resource, 0) != pdTRUE) {
        heap_caps_free(resource.wav_data);
        recycle_mask_.fetch_or(1u << index);
        ESP_LOGW(kTag, "DLNA segment queue full; dropping this segment");
        return false;
    }
    return true;
}

void DlnaPlaybackManager::EndUtterance() {
    Resource resource{};
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        const uint8_t index = write_buffer_index_;
        if (discard_until_drain_) {
            buffers_[index].Reset();
            buffer_active_[index] = false;
            buffer_dropping_[index] = false;
            fallback_utterance_ = false;
            utterance_decided_ = false;
            discard_until_drain_ = false;
            ESP_LOGI(kTag, "Discarded interrupted DLNA utterance");
            return;
        }
        if (fallback_utterance_) {
            fallback_utterance_ = false;
            utterance_decided_ = false;
            return;
        }
        if (buffer_dropping_[index]) {
            if (buffer_ready_[index]) buffers_[index].Reset();
            buffer_active_[index] = false;
            buffer_dropping_[index] = false;
            utterance_decided_ = false;
            fallback_utterance_ = false;
            ESP_LOGW(kTag, "Dropped the remainder of an utterance after DLNA segment capacity was exhausted");
            return;
        }
        if (buffer_active_[index] && buffer_ready_[index]) {
            QueueSegmentLocked(index);
        }
        utterance_decided_ = false;
        fallback_utterance_ = false;
        const uint8_t other = static_cast<uint8_t>((index + 1) % kBufferCount);
        if (buffer_ready_[other]) write_buffer_index_ = other;
    }

    if (xQueueSend(resource_queue_, &resource, 0) != pdTRUE) {
        ESP_LOGW(kTag, "DLNA resource queue full; dropping completed utterance");
        heap_caps_free(resource.wav_data);
        // Queue saturation must not permanently strand this buffer. The
        // worker polls this bitmask independently of the resource queue.
        recycle_mask_.fetch_or(1u << resource.buffer_index);

    }
}

void DlnaPlaybackManager::AbortUtterance() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // A frame may already be in AudioOutputTask but not yet have reached the
    // PCM callback. Consume/discard frames until AudioService reports drain.
    discard_until_drain_ = true;
    const uint8_t index = write_buffer_index_;
    buffers_[index].Reset();
    buffer_active_[index] = false;
    buffer_dropping_[index] = false;
    fallback_utterance_ = false;
    utterance_decided_ = false;
}

void DlnaPlaybackManager::WorkerEntry(void* arg) {
    auto* manager = static_cast<DlnaPlaybackManager*>(arg);
    manager->Worker();
    if (manager->worker_stopped_ != nullptr) {
        xSemaphoreGive(manager->worker_stopped_);
    }
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


void DlnaPlaybackManager::WaitForPlaybackCompletion(size_t wav_size) {
    // Canonical PCM WAV fields: byte rate at offset 28, data size at offset 40.
    // The buffer is still owned by current_wav_ while this method runs.
    uint32_t byte_rate = 0;
    uint32_t data_size = 0;
    if (wav_size >= 44) {
        const uint8_t* wav = current_wav_;
        byte_rate = static_cast<uint32_t>(wav[28]) |
                    (static_cast<uint32_t>(wav[29]) << 8) |
                    (static_cast<uint32_t>(wav[30]) << 16) |
                    (static_cast<uint32_t>(wav[31]) << 24);
        data_size = static_cast<uint32_t>(wav[40]) |
                    (static_cast<uint32_t>(wav[41]) << 8) |
                    (static_cast<uint32_t>(wav[42]) << 16) |
                    (static_cast<uint32_t>(wav[43]) << 24);
    }
    const uint32_t duration_ms = byte_rate == 0 ? 0 :
        static_cast<uint32_t>((static_cast<uint64_t>(data_size) * 1000) / byte_rate);
    const uint32_t fallback_wait_ms = std::min<uint32_t>(duration_ms + 5000, 120000);
    const TickType_t started = xTaskGetTickCount();
    bool observed_playing = false;
    bool transport_query_succeeded = false;

    ESP_LOGI(kTag, "Tracking renderer playback (estimated duration=%u ms)", duration_ms);
    while (!stopping_.load() && network_available_.load() &&
           !network_lost_.load()) {
        vTaskDelay(pdMS_TO_TICKS(750));
        if (stopping_.load() || !network_available_.load() || network_lost_.load()) break;

        DlnaTransportState state = DlnaTransportState::kUnknown;
        if (DlnaControl::GetTransportState(renderer_, state, 2500)) {
            transport_query_succeeded = true;
            if (state == DlnaTransportState::kPlaying) {
                observed_playing = true;
            } else if (observed_playing && state == DlnaTransportState::kStopped) {
                ESP_LOGI(kTag, "Renderer reports playback completed");
                break;
            }
        }

        const uint32_t elapsed_ms = static_cast<uint32_t>(
            (xTaskGetTickCount() - started) * portTICK_PERIOD_MS);
        if (!observed_playing && elapsed_ms >= fallback_wait_ms) {
            ESP_LOGW(kTag,
                     "No PLAYING state observed; releasing resource after duration-based safety wait (transport queries=%d)",
                     transport_query_succeeded);
            break;
        }
        if (observed_playing && elapsed_ms >= 120000) {
            ESP_LOGW(kTag, "Playback state did not return to STOPPED; applying 120 s safety limit");
            break;
        }
    }
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

    std::string host;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        host = host_ipv4_;
    }
    if (!media_server_.Start(host, resource.wav_data, resource.wav_size)) {
        heap_caps_free(resource.wav_data);
        ESP_LOGE(kTag, "Could not publish WAV resource");
        renderer_ready_ = false;
        sink_ready_.store(false);
        return;
    }

    if (!DlnaControl::SetUriAndPlay(renderer_, media_server_.GetMediaUri(),
                                    "EchoEar audio", kControlTimeoutMs)) {
        ESP_LOGE(kTag, "Renderer rejected SetAVTransportURI/Play");
        media_server_.Stop();
        heap_caps_free(resource.wav_data);
        renderer_ready_ = false;
        sink_ready_.store(false);
        return;
    }

    // Keep this allocation immutable and alive while the renderer may issue
    // GET/Range requests. It is released only when a later utterance replaces it.
    current_wav_ = resource.wav_data;
    ESP_LOGI(kTag, "DLNA playback command accepted (%u bytes)",
             static_cast<unsigned>(resource.wav_size));

    // Keep the HTTP resource alive until AVTransport reports STOPPED, or a
    // duration-based safety deadline expires when the renderer lacks state support.
    WaitForPlaybackCompletion(resource.wav_size);

    if (media_server_.IsRunning()) media_server_.Stop();
    if (current_wav_ != nullptr) {
        heap_caps_free(current_wav_);
        current_wav_ = nullptr;
    }
}

void DlnaPlaybackManager::Worker() {
    TickType_t next_discovery = 0;
    while (!stopping_.load()) {
        // Recycle requests are independent of the bounded resource queue, so
        // a full queue cannot permanently consume one of the two PCM buffers.
        uint32_t recycle = recycle_mask_.exchange(0);
        for (uint8_t i = 0; i < kBufferCount; ++i) {
            if ((recycle & (1u << i)) == 0) continue;
            const bool recycled = buffers_[i].Initialize();
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_ready_[i] = recycled;
                buffer_active_[i] = false;
                buffer_dropping_[i] = false;
                if (!buffer_ready_[write_buffer_index_] && recycled) {
                    write_buffer_index_ = i;
                }
            }
            if (!recycled) {
                ESP_LOGE(kTag, "Could not recycle PCM buffer %u",
                         static_cast<unsigned>(i));
            }
        }

        if (network_lost_.exchange(false)) {
            sink_ready_.store(false);
            if (media_server_.IsRunning()) {
                if (renderer_ready_) DlnaControl::Stop(renderer_, kControlTimeoutMs);
                media_server_.Stop();
            }
            if (current_wav_ != nullptr) {
                heap_caps_free(current_wav_);
                current_wav_ = nullptr;
            }
            renderer_ready_ = false;
            next_discovery = 0;
            ESP_LOGI(kTag, "Network loss/reconnect: released active DLNA resource");
        }

        if (network_available_.load() && !renderer_ready_ &&
            (next_discovery == 0 || xTaskGetTickCount() >= next_discovery)) {
            renderer_ready_ = FindSonyRenderer(renderer_);
            sink_ready_.store(renderer_ready_);
            next_discovery = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }

        Resource resource{};
        if (xQueueReceive(resource_queue_, &resource, pdMS_TO_TICKS(100)) == pdTRUE) {
            // The normal path recycles the buffer before network work. The
            // allocation belongs to the worker now; the WAV resource remains
            // independent and immutable.
            const bool recycled = buffers_[resource.buffer_index].Initialize();
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_ready_[resource.buffer_index] = recycled;
                buffer_active_[resource.buffer_index] = false;
                buffer_dropping_[resource.buffer_index] = false;
                if (!buffer_ready_[write_buffer_index_] && recycled) {
                    write_buffer_index_ = resource.buffer_index;
                }
            }
            if (!recycled) {
                ESP_LOGE(kTag, "Could not recycle PCM buffer %u",
                         static_cast<unsigned>(resource.buffer_index));
            }

            if (resource.wav_data != nullptr) {
                if (network_available_.load() && renderer_ready_) {
                    ProcessResource(resource);
                } else {
                    heap_caps_free(resource.wav_data);
                    ESP_LOGW(kTag, "Dropping utterance: Sony renderer is not available");
                }
            }
        }
    }

    sink_ready_.store(false);
    if (media_server_.IsRunning()) {
        if (renderer_ready_) DlnaControl::Stop(renderer_, kControlTimeoutMs);
        media_server_.Stop();
    }
    if (current_wav_ != nullptr) {
        heap_caps_free(current_wav_);
        current_wav_ = nullptr;
    }
    renderer_ready_ = false;
}
