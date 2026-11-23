#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

struct DlnaResourceView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t lease_id = 0;
};

/**
 * Bounded ownership registry for immutable HTTP media resources.
 *
 * Publish() transfers ownership of wav_data only when it succeeds.
 * Acquire()/Release() bracket every HTTP handler's access to the bytes.
 * Retire() prevents new acquisitions and frees only after outstanding leases
 * have been released. The registry owns and frees published buffers.
 */
class DlnaResourceRegistry {
public:
    static constexpr size_t kMaxResources = 2;
    static constexpr size_t kMaxLeases = 8;

    DlnaResourceRegistry() = default;
    ~DlnaResourceRegistry();

    DlnaResourceRegistry(const DlnaResourceRegistry&) = delete;
    DlnaResourceRegistry& operator=(const DlnaResourceRegistry&) = delete;

    bool Publish(uint32_t generation, uint32_t sequence, uint8_t* wav_data,
                 size_t wav_size, std::string& path);
    bool Acquire(const std::string& path, DlnaResourceView& view);
    void Release(uint32_t lease_id);
    void Retire(const std::string& path);
    void RetireAll();
    size_t GetLiveCount() const;

private:
    struct Entry {
        std::string path;
        uint8_t* data = nullptr;
        size_t size = 0;
        uint16_t references = 0;
        bool occupied = false;
        bool retired = false;
    };

    struct Lease {
        uint32_t id = 0;
        uint8_t entry_index = 0;
        bool active = false;
    };

    static void FreeEntry(Entry& entry);
    mutable std::mutex mutex_;
    std::array<Entry, kMaxResources> entries_{};
    std::array<Lease, kMaxLeases> leases_{};
    uint32_t next_lease_id_ = 1;
};
