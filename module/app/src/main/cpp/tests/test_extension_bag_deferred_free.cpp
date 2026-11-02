#include "plugins/extension_bag/model/item_handle_custody.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (condition) {                                                                  \
            ++g_pass;                                                                     \
        } else {                                                                          \
            ++g_fail;                                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
        }                                                                                 \
    } while (false)

struct DeferredEntry {
    size_t item = 0;
};

template <size_t Capacity>
struct GuardedQueue {
    size_t before = 0x12345678u;
    std::array<DeferredEntry, Capacity> entries{};
    size_t after = 0x87654321u;
};

void test_full_queue_and_nonempty_custody_are_drained_separately() {
    constexpr size_t kQueueCapacity = 16;
    constexpr size_t kCustodyCapacity = 128;
    using qol::features::extension_bag::model::drain_deferred_free_entries;

    GuardedQueue<kQueueCapacity> queue{};
    constexpr size_t kHead = 5;
    for (size_t offset = 0; offset < kQueueCapacity; ++offset) {
        queue.entries[(kHead + offset) % kQueueCapacity].item = offset + 1;
    }
    const auto queue_result = drain_deferred_free_entries(
        queue.entries, kHead, kQueueCapacity,
        [](const DeferredEntry&) { return false; },
        [](const DeferredEntry&) { return true; });
    CHECK(queue_result.kept == kQueueCapacity);
    CHECK(queue_result.freed == 0);
    CHECK(queue_result.unprocessed == 0);
    CHECK(queue.before == 0x12345678u);
    CHECK(queue.after == 0x87654321u);
    for (size_t offset = 0; offset < kQueueCapacity; ++offset) {
        CHECK(queue.entries[(kHead + offset) % kQueueCapacity].item == offset + 1);
    }

    GuardedQueue<kCustodyCapacity> custody{};
    custody.entries[0].item = 99;
    const auto custody_result = drain_deferred_free_entries(
        custody.entries, 0, 1,
        [](const DeferredEntry&) { return false; },
        [](const DeferredEntry&) { return true; });
    CHECK(custody_result.kept == 1);
    CHECK(custody_result.freed == 0);
    CHECK(custody.entries[0].item == 99);

    // A native release failure remains queued and is not reported as freed.
    const auto failed_release = drain_deferred_free_entries(
        custody.entries, 0, 1,
        [](const DeferredEntry&) { return true; },
        [](const DeferredEntry&) { return false; });
    CHECK(failed_release.kept == 1);
    CHECK(failed_release.freed == 0);
    CHECK(custody.entries[0].item == 99);
}

void test_count_is_bounded_by_backing_array_capacity() {
    constexpr size_t kCapacity = 16;
    using qol::features::extension_bag::model::drain_deferred_free_entries;

    GuardedQueue<kCapacity> queue{};
    for (size_t index = 0; index < kCapacity; ++index) {
        queue.entries[index].item = index + 1;
    }
    const auto result = drain_deferred_free_entries(
        queue.entries, 0, kCapacity + 7,
        [](const DeferredEntry&) { return false; },
        [](const DeferredEntry&) { return true; });
    CHECK(result.kept == kCapacity);
    CHECK(result.unprocessed == 7);
    CHECK(queue.before == 0x12345678u);
    CHECK(queue.after == 0x87654321u);
    for (size_t index = 0; index < kCapacity; ++index) {
        CHECK(queue.entries[index].item == index + 1);
    }
}

void test_residence_frame_and_elapsed_fallback() {
    using qol::features::extension_bag::model::residence_met;
    using qol::features::extension_bag::model::deferred_free_elapsed_ms;
    using qol::features::extension_bag::model::kDeferredFreeMinResidenceMs;

    CHECK(residence_met(100, 101, 0, kDeferredFreeMinResidenceMs));
    CHECK(!residence_met(100, 100, 100, kDeferredFreeMinResidenceMs));
    CHECK(residence_met(-1, -1, kDeferredFreeMinResidenceMs,
                        kDeferredFreeMinResidenceMs));
    CHECK(!residence_met(-1, 200, kDeferredFreeMinResidenceMs - 1,
                         kDeferredFreeMinResidenceMs));

    const uint64_t wrapped_elapsed = deferred_free_elapsed_ms(
        std::numeric_limits<uint64_t>::max() - 4, 3);
    CHECK(wrapped_elapsed == 0);
    CHECK(!residence_met(-1, -1, wrapped_elapsed, kDeferredFreeMinResidenceMs));
}

}  // namespace

int main() {
    test_full_queue_and_nonempty_custody_are_drained_separately();
    test_count_is_bounded_by_backing_array_capacity();
    test_residence_frame_and_elapsed_fallback();
    std::printf("extension_bag_deferred_free: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
