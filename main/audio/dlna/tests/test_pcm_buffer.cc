#include "dlna_pcm_buffer.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

static uint16_t Read16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1] << 8);
}

static uint32_t Read32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

int main() {
    {
        DlnaPcmBuffer buffer;
        assert(buffer.Initialize());
        assert(buffer.Begin(32000, 2));

        const int16_t stereo_samples[] = {100, -100, 200, -200};
        assert(buffer.Append(stereo_samples, 4));

        uint8_t* wav = nullptr;
        size_t wav_size = 0;
        assert(buffer.FinalizeWav(&wav, &wav_size));
        assert(wav != nullptr);
        assert(wav_size == 44 + sizeof(stereo_samples));
        assert(std::memcmp(wav, "RIFF", 4) == 0);
        assert(Read32(wav + 4) == 36 + sizeof(stereo_samples));
        assert(std::memcmp(wav + 8, "WAVEfmt ", 8) == 0);
        assert(Read32(wav + 16) == 16);
        assert(Read16(wav + 20) == 1);
        assert(Read16(wav + 22) == 2);
        assert(Read32(wav + 24) == 32000);
        assert(Read32(wav + 28) == 128000);
        assert(Read16(wav + 32) == 4);
        assert(Read16(wav + 34) == 16);
        assert(std::memcmp(wav + 36, "data", 4) == 0);
        assert(Read32(wav + 40) == sizeof(stereo_samples));
        assert(std::memcmp(wav + 44, stereo_samples, sizeof(stereo_samples)) == 0);
        std::free(wav);
    }

    {
        // The production default is 256 KiB of PCM, excluding the WAV header.
        DlnaPcmBuffer buffer;
        assert(buffer.Initialize());
        assert(buffer.Begin(32000, 1));
        std::vector<int16_t> full_segment(256 * 1024 / sizeof(int16_t), 7);
        assert(buffer.Append(full_segment.data(), full_segment.size()));
        assert(buffer.GetRemainingPcmBytes() == 0);

        const int16_t overflow_sample = 1;
        assert(!buffer.Append(&overflow_sample, 1));
        uint8_t* wav = nullptr;
        size_t wav_size = 0;
        assert(!buffer.FinalizeWav(&wav, &wav_size));
        assert(wav == nullptr);
        assert(wav_size == 0);
    }

    return 0;
}
