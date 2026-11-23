#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "dlna_pcm_buffer.h"
#include "dlna_discovery.h"
#include "dlna_media_server.h"

/**
 * Routes all AudioService PCM output (TTS, prompt sounds and notifications)
 * to a Sony SRS-X88. PCM callbacks only copy into bounded buffers; SSDP,
 * HTTP and SOAP operations run exclusively in the worker task.
 */
class DlnaPlaybackManager {
public:
    DlnaPlaybackManager() = default;
    ~DlnaPlaybackManager();

    DlnaPlaybackManager(const DlnaPlaybackManager&) = delete;
    DlnaPlaybackManager& operator=(const DlnaPlaybackManager&) = delete;

    bool Start(const std::string& host_ipv4);
    bool WritePcm(const int16_t* samples, size_t sample_count,
                  int sample_rate, int channels);
    void EndUtterance();

private:
    struct Resource {
        uint8_t* wav_data = nullptr;
        size_t wav_size = 0;
        uint8_t buffer_index = 0;
    };

    static void WorkerEntry(void* arg);
    void Worker();
    bool FindSonyRenderer(DlnaRenderer& renderer);
    void ProcessResource(const Resource& resource);

    static constexpr size_t kBufferCount = 2;
    DlnaPcmBuffer buffers_[kBufferCount];
    bool buffer_ready_[kBufferCount] = {false, false};
    bool buffer_active_[kBufferCount] = {false, false};
    bool buffer_dropping_[kBufferCount] = {false, false};
    uint8_t write_buffer_index_ = 0;
    std::mutex buffer_mutex_;

    QueueHandle_t resource_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    std::string host_ipv4_;
    DlnaRenderer renderer_;
    bool renderer_ready_ = false;
    std::atomic<bool> sink_ready_{false};

    DlnaMediaServer media_server_;
    uint8_t* current_wav_ = nullptr;
};
