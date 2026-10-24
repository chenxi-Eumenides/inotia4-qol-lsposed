#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/transaction/transaction_coordinator.h"

#include <cstdio>
#include <cstring>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (false)

}  // namespace

namespace qol::features::extension_bag {
// 日志桩：单元测试不接 host sdk。
const qol::contracts::QolHostSdk* extension_bag_host_sdk() { return nullptr; }
}  // namespace qol::features::extension_bag

// 单元测试桩：只记录 dirty/revision，避免链接整个 runtime。
void extension_bag_mark_dirty_locked() {
    runtime_state().persistence.dirty = true;
    runtime_state().persistence.revision.fetch_add(1, std::memory_order_release);
}

int main() {
    auto& state = ::runtime_state();
    state.objects.bag = {};
    state.persistence.revision.store(0);

    char first[virtual_bag::kMaxTransactionIdChars + 1]{};
    char second[virtual_bag::kMaxTransactionIdChars + 1]{};
    generate_transaction_id_locked(first);
    generate_transaction_id_locked(second);
    CHECK(std::strncmp(first, "p4-", 3) == 0);
    CHECK(std::strncmp(second, "p4-", 3) == 0);
    CHECK(std::strcmp(first, second) != 0);

    CHECK(isolation_now_ms_locked() > 0);

    // pending 记录必须同时通过域与载荷校验；空载荷应被拒绝且不写入 State。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferExtensionToOriginal;
    txn.src_bag = 0;
    txn.src_slot = 0;
    txn.dst_bag = 0;
    txn.dst_slot = 0;
    std::strncpy(txn.transaction_id, "p4-test", virtual_bag::kMaxTransactionIdChars);
    CHECK(!txn_record_pending_locked(&txn));
    CHECK(!state.objects.bag.pending.valid);

    // 隔离记录只依赖 pending 字段，不要求载荷有效。
    state.objects.bag.pending = {};
    state.objects.bag.pending.valid = true;
    std::strncpy(state.objects.bag.pending.transaction_id, "p4-isolate",
                 virtual_bag::kMaxTransactionIdChars);
    isolate_pending_locked(virtual_bag::isolation_reason::kInvalidPayload, "unit-test");
    CHECK(state.objects.bag.isolation_count == 1);
    CHECK(state.objects.bag.isolations[0].valid);
    CHECK(std::strcmp(state.objects.bag.isolations[0].detail, "unit-test") == 0);
    CHECK(std::strcmp(state.objects.bag.isolations[0].transaction_id, "p4-isolate") == 0);

    std::printf("transaction-coordinator: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
