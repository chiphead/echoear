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

    return 0;
}
