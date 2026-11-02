#pragma once

#include <cstdint>
#include <mutex>

#include "plugins/extension_bag/model/virtual_bag_state.h"

// 事务协调：pending 记录、阶段回滚、失败隔离与事务 ID。
// 只依赖 State 与模型规则；原生调用/投影恢复仍由调用方（移动用例）负责。
void generate_transaction_id_locked(char* out);
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn);
void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason);
void isolate_pending_locked(const char* reason, const char* detail);
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                     virtual_bag::TxnStage phase, const char* reason,
                                     const char* detail);
uint64_t isolation_now_ms_locked();
