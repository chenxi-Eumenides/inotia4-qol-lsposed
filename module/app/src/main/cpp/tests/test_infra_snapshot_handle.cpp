#include "infra/snapshot_handle.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

struct Record {
    uint32_t value;
    uint32_t complement;
};

void test_uninitialized_and_errors() {
    using namespace qol;
    auto handle = infra::snapshot_handle::create(sizeof(Record));
    assert(handle != nullptr);

    const void* data = reinterpret_cast<const void*>(0x1);
    uint32_t size = 77;
    contracts::QolSnapshotToken token = reinterpret_cast<void*>(0x2);
    assert(!infra::snapshot_handle::acquire(handle, &data, &size, &token));
    assert(data == reinterpret_cast<const void*>(0x1));
    assert(size == 77);
    assert(token == reinterpret_cast<void*>(0x2));

    const Record value{1, ~uint32_t{1}};
    assert(!infra::snapshot_handle::publish(handle, &value, sizeof(value) - 1));
    assert(!infra::snapshot_handle::publish(
        handle, &value, std::numeric_limits<uint32_t>::max()));
    assert(!infra::snapshot_handle::publish(handle, nullptr, sizeof(value)));
    assert(!infra::snapshot_handle::acquire(handle, nullptr, &size, &token));
    assert(infra::snapshot_handle::create(0) == nullptr);
    auto oversized = infra::snapshot_handle::create(std::numeric_limits<uint32_t>::max());
    assert(oversized != nullptr);
    infra::snapshot_handle::destroy(oversized);
    infra::snapshot_handle::destroy(handle);
}

void test_stable_views_and_large_payload() {
    using namespace qol;
    constexpr uint32_t kLargeSize = 1024 * 1024;
    auto handle = infra::snapshot_handle::create(kLargeSize);
    assert(handle != nullptr);
    std::vector<uint8_t> first(kLargeSize, 0x35);
    std::vector<uint8_t> second(kLargeSize, 0xA7);
    assert(infra::snapshot_handle::publish(handle, first.data(), kLargeSize));

    const void* view = nullptr;
    uint32_t size = 0;
    contracts::QolSnapshotToken token = nullptr;
    assert(infra::snapshot_handle::acquire(handle, &view, &size, &token));
    assert(size == kLargeSize);
    assert(static_cast<const uint8_t*>(view)[0] == 0x35);
    assert(static_cast<const uint8_t*>(view)[kLargeSize - 1] == 0x35);
    assert(!infra::snapshot_handle::publish(handle, second.data(), kLargeSize - 1));

    assert(infra::snapshot_handle::publish(handle, second.data(), kLargeSize));
    assert(static_cast<const uint8_t*>(view)[0] == 0x35);
    const void* latest = nullptr;
    uint32_t latest_size = 0;
    contracts::QolSnapshotToken latest_token = nullptr;
    assert(infra::snapshot_handle::acquire(handle, &latest, &latest_size, &latest_token));
    assert(latest_size == kLargeSize);
    assert(static_cast<const uint8_t*>(latest)[0] == 0xA7);

    infra::snapshot_handle::destroy(handle);
    assert(static_cast<const uint8_t*>(view)[kLargeSize - 1] == 0x35);
    assert(static_cast<const uint8_t*>(latest)[kLargeSize - 1] == 0xA7);
    infra::snapshot_handle::release(token);
    infra::snapshot_handle::release(latest_token);
}

void test_concurrent_readers() {
    using namespace qol;
    auto handle = infra::snapshot_handle::create(sizeof(Record));
    assert(handle != nullptr);
    const Record initial{0, ~uint32_t{0}};
    assert(infra::snapshot_handle::publish(handle, &initial, sizeof(initial)));

    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};
    std::array<std::thread, 4> readers;
    for (auto& reader : readers) {
        reader = std::thread([&] {
            while (!done.load(std::memory_order_acquire)) {
                const void* data = nullptr;
                uint32_t size = 0;
                contracts::QolSnapshotToken token = nullptr;
                if (!infra::snapshot_handle::acquire(handle, &data, &size, &token)) continue;
                if (size != sizeof(Record)) {
                    torn.store(true, std::memory_order_relaxed);
                } else {
                    Record record{};
                    std::memcpy(&record, data, sizeof(record));
                    if (record.complement != ~record.value) {
                        torn.store(true, std::memory_order_relaxed);
                    }
                }
                infra::snapshot_handle::release(token);
            }
        });
    }

    for (uint32_t value = 1; value <= 50000; ++value) {
        const Record record{value, ~value};
        assert(infra::snapshot_handle::publish(handle, &record, sizeof(record)));
    }
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    assert(!torn.load(std::memory_order_relaxed));
    infra::snapshot_handle::destroy(handle);
}

}  // namespace

int main() {
    test_uninitialized_and_errors();
    test_stable_views_and_large_payload();
    test_concurrent_readers();
    std::puts("infra_snapshot_handle: pass");
    return 0;
}
