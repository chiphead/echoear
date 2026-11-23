#include "dlna_resource_registry.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

static uint8_t* MakeWav() {
    auto* data = static_cast<uint8_t*>(std::malloc(44));
    assert(data != nullptr);
    std::memset(data, 0, 44);
    return data;
}

int main() {
    {
        DlnaResourceRegistry registry;
        std::string path;
        assert(registry.Publish(7, 1, MakeWav(), 44, path));
        assert(path == "/dlna/7/1.wav");
        assert(registry.GetLiveCount() == 1);

        DlnaResourceView first{};
        DlnaResourceView second{};
        assert(registry.Acquire(path, first));
        assert(registry.Acquire(path, second));
        assert(first.lease_id != 0 && second.lease_id != 0);
        assert(first.lease_id != second.lease_id);
        assert(first.data == second.data && first.size == 44);

        registry.Retire(path);
        assert(registry.GetLiveCount() == 1);
        DlnaResourceView rejected{};
        assert(!registry.Acquire(path, rejected));

        registry.Release(first.lease_id);
        assert(registry.GetLiveCount() == 1);
        registry.Release(second.lease_id);
        assert(registry.GetLiveCount() == 0);
        registry.Release(second.lease_id);  // Duplicate release is harmless.
        assert(registry.GetLiveCount() == 0);
    }

    {
        DlnaResourceRegistry registry;
        std::string first_path;
        std::string second_path;
        std::string rejected_path;
        assert(registry.Publish(1, 1, MakeWav(), 44, first_path));
        assert(registry.Publish(1, 2, MakeWav(), 44, second_path));
        auto* third = MakeWav();
        assert(!registry.Publish(1, 3, third, 44, rejected_path));
        std::free(third);  // Failed Publish must not take ownership.
        assert(registry.GetLiveCount() == DlnaResourceRegistry::kMaxResources);
        registry.RetireAll();
        assert(registry.GetLiveCount() == 0);
    }

    {
        DlnaResourceRegistry registry;
        std::string path;
        assert(registry.Publish(9, 4, MakeWav(), 44, path));
        auto* duplicate = MakeWav();
        std::string duplicate_path;
        assert(!registry.Publish(9, 4, duplicate, 44, duplicate_path));
        std::free(duplicate);  // Duplicate path rejection preserves caller ownership.

        DlnaResourceView leases[DlnaResourceRegistry::kMaxLeases]{};
        for (size_t i = 0; i < DlnaResourceRegistry::kMaxLeases; ++i) {
            assert(registry.Acquire(path, leases[i]));
        }
        DlnaResourceView overflow{};
        assert(!registry.Acquire(path, overflow));
        registry.RetireAll();
        assert(registry.GetLiveCount() == 1);
        for (const auto& lease : leases) registry.Release(lease.lease_id);
        assert(registry.GetLiveCount() == 0);
    }

    {
        DlnaResourceRegistry registry;
        std::string path;
        auto* invalid = MakeWav();
        assert(!registry.Publish(3, 1, invalid, 43, path));
        std::free(invalid);  // Invalid Publish must preserve caller ownership.
        assert(registry.GetLiveCount() == 0);
    }

    {
        // A retired resource with an outstanding HTTP lease must not block
        // reuse of the other registry slot. Once its final lease is released,
        // that slot must become available again as well.
        DlnaResourceRegistry registry;
        std::string pinned_path;
        std::string reusable_path;
        std::string replacement_path;
        assert(registry.Publish(11, 1, MakeWav(), 44, pinned_path));
        assert(registry.Publish(11, 2, MakeWav(), 44, reusable_path));

        DlnaResourceView pinned{};
        assert(registry.Acquire(pinned_path, pinned));
        registry.Retire(pinned_path);
        assert(registry.GetLiveCount() == 2);

        // The unpinned second entry can be retired and reused immediately.
        registry.Retire(reusable_path);
        assert(registry.GetLiveCount() == 1);
        assert(registry.Publish(11, 3, MakeWav(), 44, replacement_path));
        assert(replacement_path == "/dlna/11/3.wav");
        assert(registry.GetLiveCount() == 2);

        // The pinned entry remains unavailable until its HTTP handler releases.
        auto* blocked = MakeWav();
        std::string blocked_path;
        assert(!registry.Publish(11, 4, blocked, 44, blocked_path));
        std::free(blocked);

        registry.Release(pinned.lease_id);
        assert(registry.GetLiveCount() == 1);
        assert(registry.Publish(11, 5, MakeWav(), 44, blocked_path));
        assert(blocked_path == "/dlna/11/5.wav");
        assert(registry.GetLiveCount() == 2);
        registry.RetireAll();
        assert(registry.GetLiveCount() == 0);
    }

    return 0;
}
