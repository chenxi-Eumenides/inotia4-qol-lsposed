// save.pre_save v1.0：观察型保存前通知注册表纯逻辑测试。
#include "plugins/core.save/save_pre_save.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace qol::contracts;
using namespace qol::features::core_save::save_pre_save;

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

struct Recorder {
    int calls = 0;
    std::vector<int32_t> slots;
    std::vector<int> order;
};

void record(void* user, int32_t slot) {
    auto* recorder = static_cast<Recorder*>(user);
    ++recorder->calls;
    recorder->slots.push_back(slot);
}

struct Ordered {
    Recorder* recorder;
    int marker;
};

void record_ordered(void* user, int32_t slot) {
    auto* ordered = static_cast<Ordered*>(user);
    ++ordered->recorder->calls;
    ordered->recorder->order.push_back(ordered->marker);
    ordered->recorder->slots.push_back(slot);
}

void record_reentrant(void* user, int32_t slot) {
    auto* recorder = static_cast<Recorder*>(user);
    ++recorder->calls;
    notify(slot + 1);  // 嵌套通知必须被重入保护吞掉。
}

void throw_observer(void*, int32_t) { throw 1; }

void test_register_unregister() {
    reset_for_test();
    Recorder recorder;
    uint64_t id = 0;
    CHECK(succeeded(register_observer(11, &record, &recorder, &id)));
    CHECK(id != 0);
    notify(1);
    CHECK(recorder.calls == 1 && recorder.slots == std::vector<int32_t>({1}));
    CHECK(succeeded(unregister_observer(11, id)));
    notify(2);
    CHECK(recorder.calls == 1);
    // 未知 id 幂等成功。
    CHECK(succeeded(unregister_observer(11, id)));
    CHECK(succeeded(unregister_observer(11, 0)));
}

void test_owner_mismatch() {
    reset_for_test();
    Recorder recorder;
    uint64_t id = 0;
    CHECK(succeeded(register_observer(11, &record, &recorder, &id)));
    CHECK(unregister_observer(12, id).code == kQolErrNotFound);
    notify(0);
    CHECK(recorder.calls == 1);
    CHECK(succeeded(unregister_observer(11, id)));
}

void test_limits_and_duplicates() {
    reset_for_test();
    Recorder recorders[kMaxObservers];
    uint64_t ids[kMaxObservers] = {};
    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        CHECK(succeeded(register_observer(11, &record, &recorders[i], &ids[i])));
    }
    Recorder extra;
    uint64_t extra_id = 0;
    CHECK(register_observer(11, &record, &extra, &extra_id).code == kQolErrBlocked);
    CHECK(extra_id == 0);
    // 同一 owner+fn+user 重复注册拒绝。
    uint64_t duplicate = 0;
    CHECK(register_observer(11, &record, &recorders[0], &duplicate).code == kQolErrBlocked);
    CHECK(duplicate == 0);
    // 非法参数。
    CHECK(register_observer(0, &record, &extra, &extra_id).code == kQolErrInvalidArgument);
    CHECK(register_observer(11, nullptr, &extra, &extra_id).code == kQolErrInvalidArgument);
    CHECK(register_observer(11, &record, &extra, nullptr).code == kQolErrInvalidArgument);
}

void test_notify_order_and_reentrancy() {
    reset_for_test();
    Recorder recorder;
    Ordered first{&recorder, 1};
    Ordered second{&recorder, 2};
    uint64_t first_id = 0;
    uint64_t second_id = 0;
    CHECK(succeeded(register_observer(11, &record_ordered, &first, &first_id)));
    CHECK(succeeded(register_observer(11, &record_ordered, &second, &second_id)));
    notify(5);
    CHECK(recorder.calls == 2);
    CHECK(recorder.order == std::vector<int>({1, 2}));
    CHECK(recorder.slots == std::vector<int32_t>({5, 5}));

    reset_for_test();
    Recorder reentrant;
    uint64_t reentrant_id = 0;
    CHECK(succeeded(register_observer(11, &record_reentrant, &reentrant, &reentrant_id)));
    notify(3);
    CHECK(reentrant.calls == 1);  // 嵌套 notify 不再递归。

    // 观察者异常被吞掉，后续观察者仍执行且注册表恢复可通知。
    reset_for_test();
    Recorder after;
    uint64_t throwing_id = 0;
    uint64_t after_id = 0;
    CHECK(succeeded(register_observer(11, &throw_observer, nullptr, &throwing_id)));
    CHECK(succeeded(register_observer(11, &record, &after, &after_id)));
    notify(7);
    CHECK(after.calls == 1 && after.slots == std::vector<int32_t>({7}));
    notify(8);
    CHECK(after.calls == 2);
}

void test_unregister_owner() {
    reset_for_test();
    Recorder a;
    Recorder b;
    uint64_t a_id = 0;
    uint64_t b_id = 0;
    CHECK(succeeded(register_observer(11, &record, &a, &a_id)));
    CHECK(succeeded(register_observer(22, &record, &b, &b_id)));
    unregister_owner(11);
    notify(0);
    CHECK(a.calls == 0);
    CHECK(b.calls == 1);
    unregister_owner(22);
    notify(1);
    CHECK(b.calls == 1);
}

}  // namespace

int main() {
    test_register_unregister();
    test_owner_mismatch();
    test_limits_and_duplicates();
    test_notify_order_and_reentrancy();
    test_unregister_owner();
    std::printf("save_pre_save_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
