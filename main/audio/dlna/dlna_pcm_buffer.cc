#include "dlna_pcm_buffer.h"

#include <cstring>
#include <limits>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "DlnaPcmBuffer";
constexpr size_t kWavHeaderSize = 44;

void Put16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void Put32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}
}  // namespace

DlnaPcmBuffer::DlnaPcmBuffer(size_t max_pcm_bytes)
    : max_pcm_bytes_(max_pcm_bytes) {}

DlnaPcmBuffer::~DlnaPcmBuffer() {
    Reset();
    if (pcm_data_ != nullptr) {
        heap_caps_free(pcm_data_);
        pcm_data_ = nullptr;
    }
}

bool DlnaPcmBuffer::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_data_ != nullptr) return true;
    if (max_pcm_bytes_ == 0 || max_pcm_bytes_ > UINT32_MAX - 44) return false;

    bool allocated_in_psram = true;
    pcm_data_ = static_cast<uint8_t*>(
        heap_caps_malloc(kWavHeaderSize + max_pcm_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pcm_data_ == nullptr) {
        allocated_in_psram = false;
        pcm_data_ = static_cast<uint8_t*>(
            heap_caps_malloc(kWavHeaderSize + max_pcm_bytes_, MALLOC_CAP_8BIT));
    }
    if (pcm_data_ == nullptr) {
        ESP_LOGE(kTag, "Unable to allocate %u-byte PCM buffer",
                 static_cast<unsigned>(max_pcm_bytes_));
        return false;
    }
    ESP_LOGI(kTag, "Allocated %u-byte PCM buffer in %s",
             static_cast<unsigned>(kWavHeaderSize + max_pcm_bytes_),
             allocated_in_psram ? "PSRAM" : "8-bit heap fallback");
    return true;
}

bool DlnaPcmBuffer::Begin(int sample_rate, int channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pcm_data_ == nullptr || sample_rate <= 0 || sample_rate > 192000 ||
        channels < 1 || channels > 2) {
        return false;
    }
    pcm_size_ = 0;
    sample_rate_ = sample_rate;
    channels_ = channels;
    active_ = true;
    return true;
}

bool DlnaPcmBuffer::Append(const int16_t* samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || pcm_data_ == nullptr || channels_ <= 0 ||
        sample_count % static_cast<size_t>(channels_) != 0) {
        return false;
    }
    if (sample_count > (max_pcm_bytes_ - pcm_size_) / sizeof(int16_t)) {
        active_ = false;
        return false;
    }
    const size_t bytes = sample_count * sizeof(int16_t);
    std::memcpy(pcm_data_ + kWavHeaderSize + pcm_size_, samples, bytes);
    pcm_size_ += bytes;
    return true;
}

bool DlnaPcmBuffer::FinalizeWav(uint8_t** wav_data, size_t* wav_size) {
    if (wav_data == nullptr || wav_size == nullptr) return false;
    *wav_data = nullptr;
    *wav_size = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || pcm_data_ == nullptr || pcm_size_ == 0 ||
        sample_rate_ <= 0 || channels_ <= 0 ||
        pcm_size_ > UINT32_MAX - 36) {
        return false;
    }

    // The allocation reserves the WAV header in front of the PCM payload.
    // Finalization therefore only writes 44 bytes and transfers ownership; it
    // never allocates/copies a potentially large utterance while holding mutex_.
    uint8_t* wav = pcm_data_;
    const uint32_t byte_rate =
        static_cast<uint32_t>(sample_rate_) * static_cast<uint32_t>(channels_) * 2;
    const uint16_t block_align = static_cast<uint16_t>(channels_ * 2);
    std::memcpy(wav, "RIFF", 4);
    Put32(wav + 4, static_cast<uint32_t>(36 + pcm_size_));
    std::memcpy(wav + 8, "WAVEfmt ", 8);
    Put32(wav + 16, 16);
    Put16(wav + 20, 1);  // PCM
    Put16(wav + 22, static_cast<uint16_t>(channels_));
    Put32(wav + 24, static_cast<uint32_t>(sample_rate_));
    Put32(wav + 28, byte_rate);
    Put16(wav + 32, block_align);
    Put16(wav + 34, 16);
    std::memcpy(wav + 36, "data", 4);
    Put32(wav + 40, static_cast<uint32_t>(pcm_size_));

    *wav_data = wav;
    *wav_size = kWavHeaderSize + pcm_size_;
    pcm_data_ = nullptr;  // Ownership transfers to the caller (heap_caps_free).
    active_ = false;
    return true;
}

void DlnaPcmBuffer::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pcm_size_ = 0;
    sample_rate_ = 0;
    channels_ = 0;
    active_ = false;
}

size_t DlnaPcmBuffer::GetRemainingPcmBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && pcm_size_ <= max_pcm_bytes_ ? max_pcm_bytes_ - pcm_size_ : 0;
}

size_t DlnaPcmBuffer::GetPcmBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pcm_size_;
}

bool DlnaPcmBuffer::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}
