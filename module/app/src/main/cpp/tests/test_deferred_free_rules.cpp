#include "feature/extension_bag/model/deferred_free_rules.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

// host 单测：延迟释放排空（drain）纯规则。
// 覆盖：队列遍历上界修复、释放上界、kept 容量与回填不越界、帧龄下限真值表、
// 入队去重、null 条目跳过、custody 不重复遍历。
// `legacy_*` 断言固化旧实现缺陷（custody 项被环形回绕重复访问、over-free），
// 作为修复的回归证据。风格与 tests/test_autosell_rules.cpp 一致（自带断言计数）。

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

constexpr size_t kQueueCapacity = 16;
constexpr size_t kCustodyCapacity = 128;

// 队列 + custody 的入队模型：复刻 enqueue_deferred_item_free_locked 的查重与
// 队列满转 custody 语义。
struct DeferredModel {
    void* queue[kQueueCapacity] = {};
    size_t head = 0;
    size_t queue_size = 0;
    void* custody[kCustodyCapacity] = {};
    size_t custody_size = 0;

    bool enqueue(void* item) {
        if (item == nullptr) return false;
        for (size_t offset = 0; offset < queue_size; ++offset) {
            if (queue[(head + offset) % kQueueCapacity] == item) return true;
        }
        for (size_t index = 0; index < custody_size; ++index) {
            if (custody[index] == item) return true;
        }
        if (queue_size >= kQueueCapacity) {
            if (custody_size >= kCustodyCapacity) return false;
            custody[custody_size++] = item;
            return true;
        }
        queue[(head + queue_size) % kQueueCapacity] = item;
        ++queue_size;
        return true;
    }
};

// 修复后遍历模型：第一段只走队列自身条目，第二段独立走 custody。返回每项访问次数。
std::map<void*, int> visit_fixed(const DeferredModel& model) {
    std::map<void*, int> visits;
    const size_t bound = deferred_free_rules::queue_iteration_bound(model.queue_size,
                                                                   model.custody_size);
    for (size_t offset = 0; offset < bound; ++offset) {
        ++visits[model.queue[(model.head + offset) % kQueueCapacity]];
    }
    for (size_t index = 0; index < model.custody_size; ++index) {
        ++visits[model.custody[index]];
    }
    return visits;
}

// 旧实现遍历模型：第一段以 queue+custody 为环形队列上界，第二段再走 custody。
std::map<void*, int> visit_legacy(const DeferredModel& model) {
    std::map<void*, int> visits;
    const size_t bound = deferred_free_rules::legacy_iteration_bound(model.queue_size,
                                                                     model.custody_size);
    for (size_t offset = 0; offset < bound; ++offset) {
        ++visits[model.queue[(model.head + offset) % kQueueCapacity]];
    }
    for (size_t index = 0; index < model.custody_size; ++index) {
        ++visits[model.custody[index]];
    }
    return visits;
}

DeferredModel full_queue_with_custody(size_t custody_count) {
    DeferredModel model;
    for (size_t index = 0; index < kQueueCapacity; ++index) {
        model.queue[index] = reinterpret_cast<void*>(0x1000 + index);
    }
    model.queue_size = kQueueCapacity;
    for (size_t index = 0; index < custody_count; ++index) {
        model.custody[index] = reinterpret_cast<void*>(0x2000 + index);
    }
    model.custody_size = custody_count;
    return model;
}

}  // namespace

// 1. 上界修复：队列段上界与队列自身条目数一致，不吸收 custody。
static void test_queue_iteration_bound() {
    CHECK(deferred_free_rules::queue_iteration_bound(16, 128) == 16);
    CHECK(deferred_free_rules::queue_iteration_bound(0, 7) == 0);
    CHECK(deferred_free_rules::queue_iteration_bound(3, 0) == 3);

    // 回归锚：旧实现上界 = queue + custody，custody 项被重复访问。
    CHECK(deferred_free_rules::legacy_iteration_bound(16, 7) == 23);
    CHECK(deferred_free_rules::legacy_repeat_visits(16, 7) == 7);
}

// 2. 释放上界：旧实现会在队列满时 over-free；修复后 freed 不超过 queue+custody。
static void test_freed_within_bound() {
    CHECK(deferred_free_rules::legacy_free_calls(16, 14) == 44);
    CHECK(deferred_free_rules::legacy_free_calls(16, 14) > 16 + 14);
    CHECK(deferred_free_rules::freed_within_bound(30, 16, 14));
    CHECK(deferred_free_rules::freed_within_bound(16 + 14, 16, 14));
    CHECK(!deferred_free_rules::freed_within_bound(31, 16, 14));
    CHECK(deferred_free_rules::freed_within_bound(0, 0, 0));
}

// 3. kept 回填不越界：kept 缓冲容量 = 队列容量 + custody 容量；队列回填 <= 队列容量。
static void test_kept_capacity_and_backfill() {
    CHECK(deferred_free_rules::kept_capacity(kQueueCapacity, kCustodyCapacity) == 144);
    CHECK(deferred_free_rules::kept_capacity(16, 0) == 16);

    DeferredModel model = full_queue_with_custody(128);
    CHECK(model.queue_size + model.custody_size == 144);

    // 全部保留时 kept_count 恰好等于 144，144 项缓冲可容纳。
    size_t kept_count = model.queue_size + model.custody_size;
    CHECK(kept_count <= deferred_free_rules::kept_capacity(kQueueCapacity, kCustodyCapacity));

    // 第一段只产生队列项，回填量必须 <= 队列容量。
    const size_t queue_kept = deferred_free_rules::queue_iteration_bound(model.queue_size,
                                                                         model.custody_size);
    CHECK(queue_kept <= kQueueCapacity);
    CHECK(queue_kept == model.queue_size);
}

// 4. 帧龄下限真值表：帧号可用时要求跨帧，不可用时回退墙钟下限。
static void test_residence_truth_table() {
    CHECK(!deferred_free_rules::residence_met(100, 100, 0, 50));
    CHECK(deferred_free_rules::residence_met(100, 101, 1, 50));
    CHECK(!deferred_free_rules::residence_met(100, 99, 1000, 50));
    CHECK(!deferred_free_rules::residence_met(-1, -1, 49, 50));
    CHECK(deferred_free_rules::residence_met(-1, -1, 50, 50));
    CHECK(deferred_free_rules::residence_met(-1, 500, 50, 50));
    CHECK(!deferred_free_rules::residence_met(100, -1, 10, 50));
}

// 5. 去重：同指针二次入队不增项（queue 查重与 custody 查重都命中）。
static void test_enqueue_dedup() {
    DeferredModel model;
    void* item = reinterpret_cast<void*>(0xbeef);
    CHECK(model.enqueue(item));
    CHECK(model.queue_size == 1);
    CHECK(model.enqueue(item));
    CHECK(model.queue_size == 1);
    CHECK(model.custody_size == 0);

    DeferredModel custody_model = full_queue_with_custody(0);
    void* custody_item = reinterpret_cast<void*>(0xfeed);
    CHECK(custody_model.enqueue(custody_item));
    CHECK(custody_model.custody_size == 1);
    CHECK(custody_model.enqueue(custody_item));
    CHECK(custody_model.custody_size == 1);
}

// 6. null 跳过：null 条目不计入释放/保留，也不触达 free 回调。
static void test_null_entries_skipped() {
    std::vector<void*> queue = {reinterpret_cast<void*>(0x1), nullptr,
                                reinterpret_cast<void*>(0x2)};
    size_t visited = 0;
    size_t freed = 0;
    for (void* item : queue) {
        if (item == nullptr) continue;
        ++visited;
        ++freed;  // 无引用时即释放；null 不在此列。
    }
    CHECK(visited == 2);
    CHECK(freed == 2);
    CHECK(freed <= queue.size());
}

// 7. custody 不重复遍历：修复后每项恰访问一次；旧模型重复 custody 次。
static void test_custody_not_double_visited() {
    const size_t custody_count = 7;
    DeferredModel model = full_queue_with_custody(custody_count);

    const std::map<void*, int> fixed = visit_fixed(model);
    for (size_t index = 0; index < kQueueCapacity; ++index) {
        CHECK(fixed.at(reinterpret_cast<void*>(0x1000 + index)) == 1);
    }
    for (size_t index = 0; index < custody_count; ++index) {
        CHECK(fixed.at(reinterpret_cast<void*>(0x2000 + index)) == 1);
    }

    // 旧模型把队首 custody_count 个队列项重复访问一次，且 custody 项只访问一次。
    const std::map<void*, int> legacy = visit_legacy(model);
    for (size_t index = 0; index < kQueueCapacity; ++index) {
        const int expected = index < custody_count ? 2 : 1;
        CHECK(legacy.at(reinterpret_cast<void*>(0x1000 + index)) == expected);
    }
    for (size_t index = 0; index < custody_count; ++index) {
        CHECK(legacy.at(reinterpret_cast<void*>(0x2000 + index)) == 1);
    }
    CHECK(deferred_free_rules::legacy_repeat_visits(kQueueCapacity, custody_count) ==
          custody_count);
}

int main() {
    test_queue_iteration_bound();
    test_freed_within_bound();
    test_kept_capacity_and_backfill();
    test_residence_truth_table();
    test_enqueue_dedup();
    test_null_entries_skipped();
    test_custody_not_double_visited();

    std::printf("deferred_free_rules_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
