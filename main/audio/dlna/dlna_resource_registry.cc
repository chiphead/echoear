#include "dlna_resource_registry.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "DlnaResourceRegistry";
}

DlnaResourceRegistry::~DlnaResourceRegistry() {
    RetireAll();
}

bool DlnaResourceRegistry::Publish(uint32_t generation, uint32_t sequence,
                                   uint8_t* wav_data, size_t wav_size,
                                   std::string& path) {
    if (wav_data == nullptr || wav_size < 44) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.occupied && entry.path ==
            "/dlna/" + std::to_string(generation) + "/" +
            std::to_string(sequence) + ".wav") {
            ESP_LOGW(kTag, "Refusing duplicate resource path");
            return false;
        }
    }

    for (auto& entry : entries_) {
        if (entry.occupied) continue;
        entry.path = "/dlna/" + std::to_string(generation) + "/" +
                     std::to_string(sequence) + ".wav";
        entry.data = wav_data;
        entry.size = wav_size;
        entry.lease_id = next_lease_id_++;
        if (next_lease_id_ == 0) next_lease_id_ = 1;
        entry.references = 0;
        entry.occupied = true;
        entry.retired = false;
        path = entry.path;
        return true;
    }

    ESP_LOGW(kTag, "Resource registry full (capacity=%u)",
             static_cast<unsigned>(kMaxResources));
    return false;
}

bool DlnaResourceRegistry::Acquire(const std::string& path,
                                   DlnaResourceView& view) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (!entry.occupied || entry.retired || entry.path != path) continue;
        if (entry.references == UINT16_MAX) return false;
        ++entry.references;
        view.data = entry.data;
        view.size = entry.size;
        view.lease_id = entry.lease_id;
        return true;
    }
    return false;
}

void DlnaResourceRegistry::Release(uint32_t lease_id) {
    if (lease_id == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (!entry.occupied || entry.lease_id != lease_id) continue;
        if (entry.references > 0) --entry.references;
        if (entry.retired && entry.references == 0) FreeEntry(entry);
        return;
    }
}

void DlnaResourceRegistry::Retire(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (!entry.occupied || entry.path != path) continue;
        entry.retired = true;
        if (entry.references == 0) FreeEntry(entry);
        return;
    }
}

void DlnaResourceRegistry::RetireAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (!entry.occupied) continue;
        entry.retired = true;
        if (entry.references == 0) FreeEntry(entry);
    }
}

size_t DlnaResourceRegistry::GetLiveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& entry : entries_) {
        if (entry.occupied) ++count;
    }
    return count;
}

void DlnaResourceRegistry::FreeEntry(Entry& entry) {
    if (entry.data != nullptr) heap_caps_free(entry.data);
    entry = Entry{};
}
