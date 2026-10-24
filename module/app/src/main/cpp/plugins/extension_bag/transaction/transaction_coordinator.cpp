#include "plugins/extension_bag/transaction/transaction_coordinator.h"

#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"

#include <chrono>
#include <cstdio>
#include <cstring>

// ---- P4.4 五态事务协调：三条移动路径共享的唯一推进/回滚实现 ----

uint64_t g_transaction_sequence = 0;

void generate_transaction_id_locked(char* out) {
    const uint64_t seq = ++g_transaction_sequence;
    std::snprintf(out, virtual_bag::kMaxTransactionIdChars, "p4-%llu",
                  static_cast<unsigned long long>(seq));
}

// pending-recorded 阶段：事务 pending 写入 State。durable=false：仅本进程记录。
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn) {
    virtual_bag::txn_build_pending(txn);
    if (!virtual_bag::valid_pending_transfer_domain(txn->pending) ||
        !virtual_bag::valid_pending_transfer_payload(txn->pending)) {
        VIRTBAG_LOG("txn pending reject id=%s reason=invalid_domain", txn->transaction_id);
        return false;
    }
    runtime_state().objects.bag.pending = txn->pending;
    extension_bag_mark_dirty_locked();
    VIRTBAG_LOG("txn stage=pending-recorded durable=false id=%s direction=%u src=%u/%u dst=%u/%u",
                txn->transaction_id, static_cast<unsigned int>(txn->direction),
                static_cast<unsigned int>(txn->src_bag),
                static_cast<unsigned int>(txn->src_slot),
                static_cast<unsigned int>(txn->dst_bag),
                static_cast<unsigned int>(txn->dst_slot));
    return true;
}

// 按失败阶段回滚：先还原逻辑槽与 pending，再由调用方释放仍归模块所有的临时对象；
// 已移交原版库存的对象不参与（所有权账本终态）。
uint64_t isolation_now_ms_locked() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// P4.5：pending 恢复失败隔离——原始袋槽值原样保留（可能越界，不正则化），
// payload 字节留作只读诊断。
void isolate_pending_locked(const char* reason, const char* detail) {
    const virtual_bag::PendingTransfer& pending = runtime_state().objects.bag.pending;
    virtual_bag::IsolationRecord record{};
    record.valid = true;
    record.reason = reason;
    record.observed_at_ms = isolation_now_ms_locked();
    std::strncpy(record.transaction_id, pending.transaction_id,
                 virtual_bag::kMaxTransactionIdChars);
    record.direction = pending.direction;
    record.src_bag = pending.src_bag;
    record.src_slot = pending.src_slot;
    record.dst_bag = pending.dst_bag;
    record.dst_slot = pending.dst_slot;
    record.payload_size = pending.payload_size;
    record.payload = pending.payload;
    record.source_payload_size = pending.source_payload_size;
    record.source_payload = pending.source_payload;
    record.detail = detail;
    push_isolation(&runtime_state().objects.bag, record);
}

// P4.5：ext→orig 失败隔离——扩展源逻辑物品保留（payload 字节未丢），记录
// 事务上下文与失败阶段供审计。
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                     virtual_bag::TxnStage phase, const char* reason,
                                     const char* detail) {
    virtual_bag::IsolationRecord record{};
    record.valid = true;
    record.reason = reason;
    record.observed_at_ms = isolation_now_ms_locked();
    std::strncpy(record.transaction_id, txn.transaction_id,
                 virtual_bag::kMaxTransactionIdChars);
    record.direction = txn.direction;
    record.phase = static_cast<int>(phase);
    record.src_bag = txn.src_bag;
    record.src_slot = txn.src_slot;
    record.dst_bag = txn.dst_bag;
    record.dst_slot = txn.dst_slot;
    record.payload_size = txn.source.payload_size;
    record.payload = txn.source.payload;
    record.detail = detail;
    push_isolation(&runtime_state().objects.bag, record);
}

void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason) {
    virtual_bag::txn_rollback_logical(&runtime_state().objects.bag, txn, failed_at);
    extension_bag_mark_dirty_locked();
    VIRTBAG_LOG("txn abort stage=%d id=%s reason=%s direction=%u src=%u/%u dst=%u/%u",
                static_cast<int>(failed_at), txn.transaction_id, reason,
                static_cast<unsigned int>(txn.direction),
                static_cast<unsigned int>(txn.src_bag),
                static_cast<unsigned int>(txn.src_slot),
                static_cast<unsigned int>(txn.dst_bag),
                static_cast<unsigned int>(txn.dst_slot));
}

