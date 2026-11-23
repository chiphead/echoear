#include "dlna_playback_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <new>
#include <utility>

#include <esp_heap_caps.h>
#include "esp_audio_types.h"
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
constexpr uint32_t kMaxAudioFrameDurationMs = 120;

bool IsSonyX88(const DlnaRenderer& renderer) {
    std::string identity = renderer.friendly_name + " " + renderer.manufacturer +
                           " " + renderer.model_name + " " + renderer.server;
    std::transform(identity.begin(), identity.end(), identity.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return identity.find("srs-x88") != std::string::npos;
}
}  // namespace

DlnaPlaybackManager::~DlnaPlaybackManager() {
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
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

bool DlnaPlaybackManager::Start(const std::string& host_ipv4, int source_sample_rate, int channels) {
    if (host_ipv4.empty() || source_sample_rate <= 0 || channels < 1 || channels > 2) return false;
    if (worker_task_ != nullptr) {
        // The resampler and its preallocated output buffer are initialized once
        // for the lifetime of the worker. Do not silently accept a reconnect
        // with a different codec format; that would route frames through a
        // converter configured for the previous stream.
        if (source_sample_rate != resampler_source_rate_ ||
            channels != resampler_channels_) {
            ESP_LOGE(kTag,
                     "Reconnect PCM format changed (%d Hz/%d ch; expected %d Hz/%d ch)",
                     source_sample_rate, channels, resampler_source_rate_,
                     resampler_channels_);
            return false;
        }
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
    output_format_ready_ = InitializeResampler(source_sample_rate, channels);
    if (!output_format_ready_) {
        ESP_LOGE(kTag, "Could not prepare %d Hz to %d Hz PCM conversion; DLNA sink will remain disabled",
                 source_sample_rate, kDlnaSampleRate);
    }
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
    ESP_LOGI(kTag, "DLNA output worker started; host=%s; output=%d Hz PCM16", host_ipv4_.c_str(), kDlnaSampleRate);
    return true;
}

bool DlnaPlaybackManager::InitializeResampler(int source_sample_rate, int channels) {
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
    resampler_source_rate_ = source_sample_rate;
    resampler_channels_ = channels;
    resampled_pcm_.clear();

    if (source_sample_rate == kDlnaSampleRate) return true;

    esp_ae_rate_cvt_cfg_t config = {
        .src_rate = static_cast<uint32_t>(source_sample_rate),
        .dest_rate = static_cast<uint32_t>(kDlnaSampleRate),
        .channel = static_cast<uint8_t>(channels),
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    const auto ret = esp_ae_rate_cvt_open(&config, &resampler_);
    if (ret != ESP_AE_ERR_OK || resampler_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create %d->%d Hz resampler (%d)",
                 source_sample_rate, kDlnaSampleRate, ret);
        return false;
    }

    // AudioService supports up to 120 ms Opus frames. Preallocate here so
    // WritePcm() never allocates while running on AudioOutputTask.
    const uint32_t max_input_frames =
        static_cast<uint32_t>((static_cast<uint64_t>(source_sample_rate) *
                               kMaxAudioFrameDurationMs) / 1000);
    uint32_t max_output_frames = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(resampler_, max_input_frames,
                                               &max_output_frames) != ESP_AE_ERR_OK ||
        max_output_frames == 0) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
        return false;
    }
    resampled_pcm_.resize(static_cast<size_t>(max_output_frames) * channels);
    ESP_LOGI(kTag, "Prepared PCM conversion: %d -> %d Hz, %d channel(s)",
             source_sample_rate, kDlnaSampleRate, channels);
    return true;
}

void DlnaPlaybackManager::NotifyNetworkDisconnected() {
    // Disable new DLNA writes before taking the PCM lock. Otherwise a callback
    // racing with this notification could start a fresh utterance after the
    // generation has been invalidated but before the sink is marked offline.
    network_available_.store(false);
    sink_ready_.store(false);
    network_lost_.store(true);
    utterance_generation_.fetch_add(1);

    // Only keep the discard-until-drain guard when a DLNA-routed utterance was
    // actually in progress. A network event does not guarantee that
    // AudioService will issue a later playback-drained callback when idle.
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        const bool dlna_utterance_in_progress =
            utterance_decided_ && !fallback_utterance_;
        discard_until_drain_ = discard_until_drain_ || dlna_utterance_in_progress;
        end_pending_ = false;
        const uint8_t index = write_buffer_index_;
        buffers_[index].Reset();
        buffer_active_[index] = false;
        buffer_dropping_[index] = false;
        utterance_dropping_ = false;
        fallback_utterance_ = false;
        utterance_decided_ = false;
        if (resampler_ != nullptr) esp_ae_rate_cvt_reset(resampler_);
    }
}

bool DlnaPlaybackManager::WritePcm(const int16_t* samples, size_t sample_count,
                                   int sample_rate, int channels) {
    if (samples == nullptr || sample_count == 0) return false;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (discard_until_drain_ || utterance_dropping_) return true;
    if (end_pending_) {
        const TickType_t silence_ticks = pdMS_TO_TICKS(kEndSilenceGraceMs);
        if ((xTaskGetTickCount() - end_pending_tick_) >= silence_ticks) {
            // A real quiet interval separates utterances. Finalize the previous
            // tail before accepting this frame as a new utterance.
            FinalizePendingUtteranceLocked();
        } else {
            // Queue drain was only a short inter-packet gap. Keep the same WAV
            // segment and, importantly, preserve the resampler's filter state.
            end_pending_ = false;
        }
    }
    if (channels < 1 || channels > 2 ||
        sample_count % static_cast<size_t>(channels) != 0) {
        if (utterance_decided_ && !fallback_utterance_) {
            buffer_dropping_[write_buffer_index_] = true;
            utterance_dropping_ = true;
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    if (utterance_decided_ && fallback_utterance_) return false;
    bool new_utterance = false;
    if (!utterance_decided_) {
        new_utterance = true;
        utterance_decided_ = true;
        current_utterance_generation_ = utterance_generation_.load();
        if (resource_queue_ == nullptr || !sink_ready_.load()) {
            fallback_utterance_ = true;
            return false;
        }
    }
    const auto reject_frame = [this, new_utterance]() {
        if (new_utterance) {
            fallback_utterance_ = true;
            return false;
        }
        // Once an utterance has started on DLNA, never send a later frame to
        // the local codec: discard its tail instead of splitting the output.
        buffer_dropping_[write_buffer_index_] = true;
        utterance_dropping_ = true;
        return true;
    };

    if (channels != resampler_channels_) {
        callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
        return reject_frame();
    }

    // The SRS-X88 published network WAV/Linear PCM formats start at 32 kHz.
    // Convert decoded PCM before buffering; preserve channels to avoid
    // unnecessary bandwidth expansion. Conversion uses a preallocated buffer.
    const int16_t* output_samples = samples;
    size_t output_sample_count = sample_count;
    if (sample_rate != kDlnaSampleRate) {
        if (!output_format_ready_ || resampler_ == nullptr ||
            sample_rate != resampler_source_rate_ || channels != resampler_channels_) {
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
            return reject_frame();
        }
        uint32_t max_output_frames = 0;
        const uint32_t input_frames = static_cast<uint32_t>(sample_count / channels);
        if (esp_ae_rate_cvt_get_max_out_sample_num(resampler_, input_frames,
                                                   &max_output_frames) != ESP_AE_ERR_OK ||
            static_cast<size_t>(max_output_frames) * channels > resampled_pcm_.size()) {
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
            return reject_frame();
        }
        uint32_t actual_output_frames = max_output_frames;
        const auto convert_ret = esp_ae_rate_cvt_process(
            resampler_, (esp_ae_sample_t*)samples, input_frames,
            (esp_ae_sample_t*)resampled_pcm_.data(), &actual_output_frames);
        if (convert_ret != ESP_AE_ERR_OK || actual_output_frames == 0) {
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
            return reject_frame();
        }
        output_samples = resampled_pcm_.data();
        output_sample_count = static_cast<size_t>(actual_output_frames) * channels;
        sample_rate = kDlnaSampleRate;
    } else if (!output_format_ready_) {
        return reject_frame();
    }

    uint8_t index = write_buffer_index_;
    if (!buffer_ready_[index]) {
        const int available = FindReadyBufferLocked(index);
        if (available >= 0) {
            index = static_cast<uint8_t>(available);
            write_buffer_index_ = index;
        } else {
            if (!new_utterance) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            // The first frame has not been accepted by DLNA yet. Preserve the
            // utterance-level routing invariant by falling back locally now.
            fallback_utterance_ = true;
            return false;
        }
    }

    if (buffer_dropping_[index]) return true;
    if (!buffer_active_[index]) {
        if (!buffers_[index].Begin(sample_rate, channels)) {
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
            return reject_frame();
        }
        buffer_active_[index] = true;
    }
    size_t offset = 0;
    while (offset < output_sample_count) {
        const size_t remaining_samples =
            buffers_[index].GetRemainingPcmBytes() / sizeof(int16_t);
        const size_t frame_samples = static_cast<size_t>(channels);
        const size_t aligned_remaining =
            (remaining_samples / frame_samples) * frame_samples;

        if (aligned_remaining == 0) {
            if (!QueueSegmentLocked(index)) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            const int available = FindReadyBufferLocked(index);
            if (available < 0) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            index = static_cast<uint8_t>(available);
            write_buffer_index_ = index;
            if (!buffers_[index].Begin(sample_rate, channels)) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            buffer_active_[index] = true;
            continue;
        }

        const size_t chunk_samples = std::min(output_sample_count - offset, aligned_remaining);
        if (!buffers_[index].Append(output_samples + offset, chunk_samples)) {
            buffer_dropping_[index] = true;
            utterance_dropping_ = true;
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        offset += chunk_samples;

        if (offset < output_sample_count) {
            if (!QueueSegmentLocked(index)) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            const int available = FindReadyBufferLocked(index);
            if (available < 0) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            index = static_cast<uint8_t>(available);
            write_buffer_index_ = index;
            if (!buffers_[index].Begin(sample_rate, channels)) {
                buffer_dropping_[index] = true;
                utterance_dropping_ = true;
                callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            buffer_active_[index] = true;
        }
    }
    return true;
}


int DlnaPlaybackManager::FindReadyBufferLocked(uint8_t after_index) const {
    for (size_t offset = 1; offset <= kBufferCount; ++offset) {
        const uint8_t candidate = static_cast<uint8_t>(
            (static_cast<size_t>(after_index) + offset) % kBufferCount);
        if (buffer_ready_[candidate]) return candidate;
    }
    return -1;
}

bool DlnaPlaybackManager::QueueSegmentLocked(uint8_t index) {
    if (index >= kBufferCount || !buffer_ready_[index] || !buffer_active_[index]) return false;
    Resource resource{};
    if (!buffers_[index].FinalizeWav(&resource.wav_data, &resource.wav_size)) return false;
    resource.buffer_index = index;
    resource.utterance_generation = current_utterance_generation_;
    buffer_ready_[index] = false;
    buffer_active_[index] = false;
    const uint32_t byte_rate = static_cast<uint32_t>(resource.wav_data[28]) |
        (static_cast<uint32_t>(resource.wav_data[29]) << 8) |
        (static_cast<uint32_t>(resource.wav_data[30]) << 16) |
        (static_cast<uint32_t>(resource.wav_data[31]) << 24);
    const uint32_t pcm_bytes = static_cast<uint32_t>(resource.wav_data[40]) |
        (static_cast<uint32_t>(resource.wav_data[41]) << 8) |
        (static_cast<uint32_t>(resource.wav_data[42]) << 16) |
        (static_cast<uint32_t>(resource.wav_data[43]) << 24);
    resource.duration_ms = byte_rate == 0 ? 0 : static_cast<uint32_t>(
        (static_cast<uint64_t>(pcm_bytes) * 1000) / byte_rate);
    if (xQueueSend(resource_queue_, &resource, 0) != pdTRUE) {
        heap_caps_free(resource.wav_data);
        recycle_mask_.fetch_or(1u << index);
        queue_drop_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void DlnaPlaybackManager::EndUtterance() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (discard_until_drain_) {
        utterance_dropping_ = false;
        const uint8_t index = write_buffer_index_;
        buffers_[index].Reset();
        buffer_active_[index] = false;
        buffer_dropping_[index] = false;
        fallback_utterance_ = false;
        utterance_decided_ = false;
        end_pending_ = false;
        discard_until_drain_ = false;
        if (resampler_ != nullptr) esp_ae_rate_cvt_reset(resampler_);
        return;
    }
    if (fallback_utterance_) {
        utterance_dropping_ = false;
        fallback_utterance_ = false;
        utterance_decided_ = false;
        end_pending_ = false;
        if (resampler_ != nullptr) esp_ae_rate_cvt_reset(resampler_);
        return;
    }
    if (!utterance_decided_) return;

    // AudioService queue-drained is not a protocol end-of-stream marker. A
    // short inter-packet gap must not turn every Opus frame into a tiny WAV.
    end_pending_ = true;
    end_pending_tick_ = xTaskGetTickCount();
}

void DlnaPlaybackManager::FinalizePendingUtteranceLocked() {
    if (!end_pending_) return;
    end_pending_ = false;
    if (resampler_ != nullptr) esp_ae_rate_cvt_reset(resampler_);

    const uint8_t index = write_buffer_index_;
    if (utterance_dropping_ || buffer_dropping_[index]) {
        if (buffer_ready_[index]) buffers_[index].Reset();
        if (buffer_ready_[index]) buffers_[index].Reset();
        buffer_active_[index] = false;
        buffer_dropping_[index] = false;
        callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (buffer_active_[index] && buffer_ready_[index]) {
        if (!QueueSegmentLocked(index)) {
            callback_issue_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    utterance_decided_ = false;
    fallback_utterance_ = false;
    utterance_dropping_ = false;
    const int available = FindReadyBufferLocked(index);
    if (available >= 0) write_buffer_index_ = static_cast<uint8_t>(available);
}

void DlnaPlaybackManager::AbortUtterance() {
    utterance_generation_.fetch_add(1);
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (resampler_ != nullptr) esp_ae_rate_cvt_reset(resampler_);
    // A frame may already be in AudioOutputTask but not yet have reached the
    // PCM callback. Consume/discard frames until AudioService reports drain.
    discard_until_drain_ = true;
    utterance_dropping_ = false;
    end_pending_ = false;
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


void DlnaPlaybackManager::ProcessResource(const Resource& first_resource) {
    struct Item {
        Resource resource{};
        std::string path;
        std::string uri;
        TickType_t started = 0;
        uint32_t duration_ms = 0;
        bool observed_playing = false;
        bool published = false;
        bool next_action_attempted = false;
        bool next_action_sent = false;
        TickType_t next_action_sent_at = 0;
        bool next_uri_observed = false;
    };

    auto recycle_buffer = [this](uint8_t index) {
        if (index >= kBufferCount) return;
        const bool recycled = buffers_[index].Initialize();
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_ready_[index] = recycled;
        buffer_active_[index] = false;
        buffer_dropping_[index] = false;
        if (!buffer_ready_[write_buffer_index_] && recycled) write_buffer_index_ = index;
        if (!recycled) ESP_LOGE(kTag, "Could not recycle PCM buffer %u",
                                static_cast<unsigned>(index));
    };

    std::string host;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        host = host_ipv4_;
    }
    if (first_resource.wav_data == nullptr || first_resource.wav_size < 44) {
        if (first_resource.wav_data) heap_caps_free(first_resource.wav_data);
        return;
    }
    if (first_resource.utterance_generation != utterance_generation_.load() ||
        !renderer_ready_) {
        heap_caps_free(first_resource.wav_data);
        return;
    }
    if (!media_server_.IsRunning() &&
        !media_server_.Start(host, resource_registry_)) {
        heap_caps_free(first_resource.wav_data);
        ESP_LOGE(kTag, "Could not start registry-backed media server");
        renderer_ready_ = false;
        sink_ready_.store(false);
        return;
    }

    auto publish_item = [this](Item& item) -> bool {
        if (item.resource.wav_data == nullptr || item.resource.wav_size < 44) return false;
        const uint32_t sequence = ++resource_sequence_;
        if (!resource_registry_.Publish(item.resource.utterance_generation, sequence,
                                        item.resource.wav_data, item.resource.wav_size,
                                        item.path)) return false;
        item.uri = media_server_.GetMediaUri() + item.path.substr(std::strlen("/dlna/"));
        const uint8_t* wav = item.resource.wav_data;
        const uint32_t byte_rate = static_cast<uint32_t>(wav[28]) |
            (static_cast<uint32_t>(wav[29]) << 8) |
            (static_cast<uint32_t>(wav[30]) << 16) |
            (static_cast<uint32_t>(wav[31]) << 24);
        const uint32_t data_size = static_cast<uint32_t>(wav[40]) |
            (static_cast<uint32_t>(wav[41]) << 8) |
            (static_cast<uint32_t>(wav[42]) << 16) |
            (static_cast<uint32_t>(wav[43]) << 24);
        item.duration_ms = byte_rate == 0 ? 0 : static_cast<uint32_t>(
            (static_cast<uint64_t>(data_size) * 1000) / byte_rate);
        item.resource.wav_data = nullptr;  // Registry owns it from here.
        item.published = true;
        return true;
    };

    Item current{};
    current.resource = first_resource;
    bool current_published = false;
    for (int attempt = 0; attempt < 20 && !stopping_.load() &&
         network_available_.load() && !network_lost_.load() &&
         current.resource.utterance_generation == utterance_generation_.load(); ++attempt) {
        current_published = publish_item(current);
        if (current_published) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!current_published) {
        heap_caps_free(current.resource.wav_data);
        ESP_LOGE(kTag, "Could not publish initial WAV resource after bounded retries");
        return;
    }
    if (!DlnaControl::SetUriAndPlay(renderer_, current.uri,
                                    "EchoEar audio", kControlTimeoutMs)) {
        resource_registry_.Retire(current.path);
        renderer_ready_ = false;
        sink_ready_.store(false);
        return;
    }
    current.started = xTaskGetTickCount();
    ESP_LOGI(kTag, "Started DLNA item %s (%u ms)",
             current.uri.c_str(), current.duration_ms);

    Item next{};
    bool have_next = false;
    TickType_t next_poll = xTaskGetTickCount();
    TickType_t stopped_since = 0;

    while (!stopping_.load() && network_available_.load() &&
           !network_lost_.load() &&
           current.resource.utterance_generation == utterance_generation_.load()) {
        if (!have_next) {
            Resource queued{};
            if (xQueueReceive(resource_queue_, &queued, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(kTag, "Dequeued look-ahead segment: %u ms, queue_depth=%u/%u",
                         static_cast<unsigned>(queued.duration_ms),
                         static_cast<unsigned>(uxQueueMessagesWaiting(resource_queue_)),
                         static_cast<unsigned>(kQueueDepth));
                recycle_buffer(queued.buffer_index);
                if (queued.wav_data != nullptr) {
                    if (queued.utterance_generation != utterance_generation_.load()) {
                        heap_caps_free(queued.wav_data);
                    } else if (queued.wav_size >= 44) {
                        next.resource = queued;
                        have_next = true;
                        if (publish_item(next)) {
                            ESP_LOGI(kTag, "Prepared next DLNA item %s", next.uri.c_str());
                        } else {
                            ESP_LOGW(kTag, "Registry is busy; retaining look-ahead WAV until a slot is free");
                        }
                    } else {
                        heap_caps_free(queued.wav_data);
                        ESP_LOGW(kTag, "Discarded malformed queued WAV resource");
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (stopping_.load() || !network_available_.load() || network_lost_.load() ||
            current.resource.utterance_generation != utterance_generation_.load()) break;

        if (have_next && !next.published) {
            if (publish_item(next)) {
                ESP_LOGI(kTag, "Published retained look-ahead item %s", next.uri.c_str());
            }
        }

        if (xTaskGetTickCount() < next_poll) continue;
        next_poll = xTaskGetTickCount() + pdMS_TO_TICKS(650);

        DlnaTransportState transport = DlnaTransportState::kUnknown;
        const bool state_ok = DlnaControl::GetTransportState(renderer_, transport, 1500);
        if (state_ok && transport == DlnaTransportState::kPlaying) {
            current.observed_playing = true;
            stopped_since = 0;
        }

        bool next_is_current = false;
        if (have_next && next.published && next.next_action_sent) {
            std::string renderer_uri;
            if (DlnaControl::GetCurrentUri(renderer_, renderer_uri, 1500)) {
                const bool uri_matches_next = renderer_uri == next.uri;
                if (uri_matches_next && !next.next_uri_observed) {
                    next.next_uri_observed = true;
                    ESP_LOGI(kTag, "Renderer CurrentURI now references preloaded item; waiting for playback state");
                }
                next_is_current = uri_matches_next && state_ok &&
                                  transport == DlnaTransportState::kPlaying;
            }
        }
        if (next_is_current) {
            ESP_LOGI(kTag, "Renderer transitioned to preloaded item");
            resource_registry_.Retire(current.path);
            current = std::move(next);
            next = Item{};
            have_next = false;
            current.started = xTaskGetTickCount();
            current.observed_playing = state_ok && transport == DlnaTransportState::kPlaying;
            stopped_since = 0;
            continue;
        }

        if (have_next && next.published && current.observed_playing && !next.next_action_attempted &&
            renderer_.supports_set_next_av_transport_uri) {
            next.next_action_attempted = true;
            next.next_action_sent = DlnaControl::SetNextUri(
                renderer_, next.uri, "EchoEar audio", kControlTimeoutMs);
            if (next.next_action_sent) {
                next.next_action_sent_at = xTaskGetTickCount();
            }
            if (!next.next_action_sent) {
                ESP_LOGW(kTag, "SetNext rejected; sequential fallback retained");
            }
        }

        const uint32_t elapsed_ms = static_cast<uint32_t>(
            (xTaskGetTickCount() - current.started) * portTICK_PERIOD_MS);
        // Never let a lost/stale AVTransport status query pin the worker
        // forever. Allow a generous tail after the encoded WAV duration, but
        // apply this watchdog whether or not PLAYING was ever observed.
        const uint32_t playback_watchdog_ms = std::min<uint32_t>(
            current.duration_ms + 15000, 180000);
        const bool duration_expired = elapsed_ms >= playback_watchdog_ms;
        const bool renderer_stopped = state_ok &&
            transport == DlnaTransportState::kStopped;
        const bool stopped_after_play = renderer_stopped && current.observed_playing;
        const bool next_transition_pending = have_next && next.published &&
            next.next_action_sent &&
            (next.next_uri_observed ||
             (state_ok && transport == DlnaTransportState::kTransitioning)) &&
            (xTaskGetTickCount() - next.next_action_sent_at) < pdMS_TO_TICKS(10000);
        if (duration_expired && !renderer_stopped && !next_transition_pending) {
            ESP_LOGW(kTag, "Playback watchdog expired without a confirmed STOPPED state; stopping renderer before fallback");
            if (!DlnaControl::Stop(renderer_, kControlTimeoutMs)) {
                renderer_ready_ = false;
                sink_ready_.store(false);
                media_server_.Stop();
                if (!media_server_.IsRunning()) {
                    resource_registry_.RetireAll();
                } else {
                    // Keep the worker and registry alive while HTTPD may still
                    // hold request user_ctx/leases. Worker cleanup retries Stop.
                    network_available_.store(false);
                    network_lost_.store(true);
                }
                ESP_LOGE(kTag, "Renderer Stop failed; media server shutdown requested before WAV retirement");
                return;
            }
        }

        if (stopped_after_play || duration_expired) {
            if (have_next && !next.published) {
                // Do not wait forever with the completed current item occupying
                // a registry slot. Once playback is stopped, retire it and give
                // outstanding HTTP leases a bounded window to drain.
                ESP_LOGW(kTag, "Next WAV is not published; retiring completed item before bounded retry");
                resource_registry_.Retire(current.path);
                bool next_published = false;
                for (int attempt = 0; attempt < 30 && !stopping_.load() &&
                     network_available_.load() && !network_lost_.load() &&
                     current.resource.utterance_generation == utterance_generation_.load();
                     ++attempt) {
                    if (publish_item(next)) {
                        next_published = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (!next_published) {
                    ESP_LOGE(kTag, "Next WAV could not be published after bounded lease-drain wait");
                    break;
                }
                ESP_LOGI(kTag, "Published retained next item after current resource retirement");
            }
            if (have_next && next.published) {
                // A successful SetNext may have transitioned while the
                // transport-state query briefly reports STOPPED. Keep both
                // resources alive and retry CurrentURI observation before
                // forcing sequential playback, avoiding an accidental replay.
                if (next.next_action_sent) {
                    const TickType_t now = xTaskGetTickCount();
                    const bool renderer_reports_transition =
                        state_ok && transport == DlnaTransportState::kTransitioning;
                    const bool next_transition_in_progress =
                        renderer_reports_transition || next.next_uri_observed;
                    if (next_transition_in_progress &&
                        (now - next.next_action_sent_at) < pdMS_TO_TICKS(10000)) {
                        // Some renderers update CurrentURI before reporting
                        // PLAYING, or expose a TRANSITIONING/STOPPED interval.
                        // Do not overwrite that accepted next item with a
                        // second SetAVTransportURI while it is starting.
                        continue;
                    }
                    if (stopped_since == 0) stopped_since = now;
                    if ((now - stopped_since) < pdMS_TO_TICKS(2000)) {
                        continue;
                    }
                }
                if (DlnaControl::SetUriAndPlay(renderer_, next.uri,
                                               "EchoEar audio", kControlTimeoutMs)) {
                    resource_registry_.Retire(current.path);
                    current = std::move(next);
                    next = Item{};
                    have_next = false;
                    current.started = xTaskGetTickCount();
                    current.observed_playing = false;
                    stopped_since = 0;
                    ESP_LOGI(kTag, "Started next item using sequential fallback");
                    continue;
                }
                ESP_LOGE(kTag, "Could not start next DLNA item");
                break;
            }
            if (stopped_since == 0) {
                stopped_since = xTaskGetTickCount();
                continue;
            }
            if ((xTaskGetTickCount() - stopped_since) >= pdMS_TO_TICKS(700)) {
                ESP_LOGI(kTag, "Current DLNA item completed");
                break;
            }
        }
    }

    if (stopping_.load() || !network_available_.load() || network_lost_.load() ||
        current.resource.utterance_generation != utterance_generation_.load()) {
        DlnaControl::Stop(renderer_, kControlTimeoutMs);
    }
    resource_registry_.Retire(current.path);
    if (have_next) {
        if (next.published) resource_registry_.Retire(next.path);
        else if (next.resource.wav_data != nullptr) heap_caps_free(next.resource.wav_data);
    }
}

void DlnaPlaybackManager::Worker() {
    TickType_t next_discovery = 0;
    TickType_t next_memory_report = xTaskGetTickCount();
    while (!stopping_.load()) {
        // Report both free and largest contiguous blocks. Free bytes alone can
        // hide fragmentation that prevents a 256 KiB segment from being recycled.
        const TickType_t memory_report_now = xTaskGetTickCount();
        if (static_cast<int32_t>(memory_report_now - next_memory_report) >= 0) {
            next_memory_report = memory_report_now + pdMS_TO_TICKS(10000);
            size_t ready_buffers = 0;
            size_t active_buffers = 0;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                for (size_t i = 0; i < kBufferCount; ++i) {
                    if (buffer_ready_[i]) ++ready_buffers;
                    if (buffer_active_[i]) ++active_buffers;
                }
            }
            const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
            const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            ESP_LOGI(kTag,
                     "Memory: PSRAM free=%u largest=%u min_free=%u; "
                     "internal free=%u largest=%u min_free=%u; "
                     "PCM buffers ready=%u active=%u; resources queued=%u live=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(psram_caps)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(psram_caps)),
                     static_cast<unsigned>(heap_caps_get_minimum_free_size(psram_caps)),
                     static_cast<unsigned>(heap_caps_get_free_size(internal_caps)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(internal_caps)),
                     static_cast<unsigned>(heap_caps_get_minimum_free_size(internal_caps)),
                     static_cast<unsigned>(ready_buffers),
                     static_cast<unsigned>(active_buffers),
                     static_cast<unsigned>(resource_queue_ != nullptr
                                               ? uxQueueMessagesWaiting(resource_queue_) : 0),
                     static_cast<unsigned>(resource_registry_.GetLiveCount()));
        }
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (end_pending_ &&
                (xTaskGetTickCount() - end_pending_tick_) >=
                    pdMS_TO_TICKS(kEndSilenceGraceMs)) {
                FinalizePendingUtteranceLocked();
            }
        }

        const uint32_t callback_issues =
            callback_issue_count_.exchange(0, std::memory_order_relaxed);
        if (callback_issues > 0) {
            ESP_LOGW(kTag, "Audio callback reported %u DLNA PCM issue(s)",
                     static_cast<unsigned>(callback_issues));
        }
        const uint32_t queue_drops =
            queue_drop_count_.exchange(0, std::memory_order_relaxed);
        if (queue_drops > 0) {
            ESP_LOGW(kTag, "Resource queue full; dropped %u DLNA segment(s)",
                     static_cast<unsigned>(queue_drops));
        }

        // Recycle requests are independent of the bounded resource queue, so
        // a full queue cannot permanently consume one of the three PCM buffers.
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
            if (media_server_.IsRunning()) {
                // A failed httpd_stop() means handlers may still reference
                // this manager's registry. Retry without retiring/destroying it.
                network_lost_.store(true);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            resource_registry_.RetireAll();
            renderer_ready_ = false;
            next_discovery = 0;
            ESP_LOGI(kTag, "Network loss/reconnect: released active DLNA resource");
        }

        if (network_available_.load() && !renderer_ready_ &&
            (next_discovery == 0 || xTaskGetTickCount() >= next_discovery)) {
            renderer_ready_ = FindSonyRenderer(renderer_);
            sink_ready_.store(renderer_ready_ && output_format_ready_);
            next_discovery = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }

        Resource resource{};
        if (xQueueReceive(resource_queue_, &resource, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(kTag, "Dequeued DLNA segment: %u ms, queue_depth=%u/%u",
                     static_cast<unsigned>(resource.duration_ms),
                     static_cast<unsigned>(uxQueueMessagesWaiting(resource_queue_)),
                     static_cast<unsigned>(kQueueDepth));
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
    // The destructor waits for this worker. Do not signal completion while
    // HTTPD can still call handlers whose user_ctx points into this object.
    while (media_server_.IsRunning()) {
        ESP_LOGE(kTag, "HTTP server is still active; retrying shutdown before registry destruction");
        vTaskDelay(pdMS_TO_TICKS(500));
        media_server_.Stop();
    }
    resource_registry_.RetireAll();
    renderer_ready_ = false;
}
