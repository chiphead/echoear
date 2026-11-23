#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

/**
 * Bounded PCM accumulator for one DLNA WAV segment.
 *
 * AudioService callbacks may append frames from its output task. FinalizeWav()
 * must be called from a worker task after playback has drained. The returned
 * WAV buffer is heap_caps_malloc()'d and must be released with heap_caps_free().
 */
class DlnaPcmBuffer {
public:
    // 64 KiB is about 1.0 s of 32 kHz mono PCM16, reducing first-segment
    // latency while retaining enough data for reliable HTTP/DLNA playback.
    explicit DlnaPcmBuffer(size_t max_pcm_bytes = 64 * 1024);
    ~DlnaPcmBuffer();

    DlnaPcmBuffer(const DlnaPcmBuffer&) = delete;
    DlnaPcmBuffer& operator=(const DlnaPcmBuffer&) = delete;

    bool Initialize();
    bool Begin(int sample_rate, int channels);
    bool Append(const int16_t* samples, size_t sample_count);
    bool FinalizeWav(uint8_t** wav_data, size_t* wav_size);
    void Reset();

    size_t GetPcmBytes() const;
    size_t GetRemainingPcmBytes() const;
    bool IsActive() const;

private:
    const size_t max_pcm_bytes_;
    mutable std::mutex mutex_;
    uint8_t* pcm_data_ = nullptr;
    size_t pcm_size_ = 0;
    int sample_rate_ = 0;
    int channels_ = 0;
    bool active_ = false;
};
