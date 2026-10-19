#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"
#include "plugins/extension_bag/extension_bag_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

#define isolation_now_ms_locked isolation_now_ms_locked_internal
#define bag_size_word_locked bag_size_word_locked_internal
#define clear_module_cache_locked clear_module_cache_locked_internal
#define clear_original_desc_locked clear_original_desc_locked_internal
#define ensure_module_view_root_current_locked ensure_module_view_root_current_locked_internal
#define ensure_state_loaded_locked ensure_state_loaded_locked_internal
#define free_module_object_locked free_module_object_locked_internal
#define generate_transaction_id_locked generate_transaction_id_locked_internal
#define install_module_view_locked install_module_view_locked_internal
#define inventory_slot_locked inventory_slot_locked_internal
#define isolate_ext2orig_failure_locked isolate_ext2orig_failure_locked_internal
#define isolate_pending_locked isolate_pending_locked_internal
#define item_pointer_known_locked item_pointer_known_locked_internal
#define load_item_payload_locked load_item_payload_locked_internal
#define log_exit_trace_locked log_exit_trace_locked_internal
#define materialize_module_item_locked materialize_module_item_locked_internal
#define module_cache_clear_preflight_locked module_cache_clear_preflight_locked_internal
#define module_item_locked module_item_locked_internal
#define original_bag_button_index original_bag_button_index_internal
#define original_bag_locked original_bag_locked_internal
#define original_inventory_contains_payload_locked original_inventory_contains_payload_locked_internal
#define original_inventory_handle_slot_locked original_inventory_handle_slot_locked_internal
#define original_inventory_payload_slot_locked original_inventory_payload_slot_locked_internal
#define persist_state_locked persist_state_locked_internal
#define prepare_main_menu_locked prepare_main_menu_locked_internal
#define recover_pending_transaction_locked recover_pending_transaction_locked_internal
#define refresh_module_item_area_locked refresh_module_item_area_locked_internal
#define refresh_projected_module_view_after_move_locked refresh_projected_module_view_after_move_locked_internal
#define refresh_projection_if_overwritten_locked refresh_projection_if_overwritten_locked_internal
#define release_temporary_item_locked release_temporary_item_locked_internal
#define remove_item_direct_unlocked remove_item_direct_unlocked_internal
#define reset_extension_desc_item_hooks_locked reset_extension_desc_item_hooks_locked_internal
#define restore_extension_desc_popup_callbacks_locked restore_extension_desc_popup_callbacks_locked_internal
#define restore_module_view_locked restore_module_view_locked_internal
#define serialize_item_payload_locked serialize_item_payload_locked_internal
#define set_bag_size_word_locked set_bag_size_word_locked_internal
#define set_original_bag_locked set_original_bag_locked_internal
#define sync_projected_slot_locked sync_projected_slot_locked_internal
#define teardown_extension_bag_hosts_locked teardown_extension_bag_hosts_locked_internal
#define txn_abort_locked txn_abort_locked_internal
#define txn_record_pending_locked txn_record_pending_locked_internal

namespace {
bool inventory_slot_locked(int bag, int slot, void** out);

bool remove_item_direct_unlocked(std::unique_lock<std::mutex>& lock, int bag, int slot,
                                 void* expected_item, const char* context) {
    if (!lock.owns_lock() || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.remove_item_direct == nullptr ||
        expected_item == nullptr || runtime_state().host.item_facts == nullptr ||
        runtime_state().host.item_facts->item_ref_at == nullptr) {
        return false;
    }

    void* source_item = nullptr;
    qol::contracts::QolInventoryItemRef source_ref{};
    qol::contracts::QolItemHandle source_handle{};
    if (!inventory_slot_locked(bag, slot, &source_item) || source_item != expected_item ||
        !qol::contracts::succeeded(runtime_state().host.item_facts->item_ref_at(bag, slot, &source_ref)) ||
        source_ref.bag != bag || source_ref.slot != slot ||
        source_ref.item.slot == 0 || source_ref.item.generation == 0 ||
        !extension_bag_item_handle_for_pointer(expected_item, &source_handle) ||
        source_handle.slot != source_ref.item.slot ||
        source_handle.generation != source_ref.item.generation) {
        VIRTBAG_LOG_ERROR(
            "ERROR native remove source identity unavailable context=%s bag=%d slot=%d expected=%p actual=%p ref_bag=%d ref_slot=%d handle=%u/%u",
            context != nullptr ? context : "unknown", bag, slot, expected_item, source_item,
            source_ref.bag, source_ref.slot, source_ref.item.slot,
            source_ref.item.generation);
        return false;
    }

    bool remove_call_ok = false;
    int result = 0;
    lock.unlock();
    {
        NativeCallScope native_call;
        remove_call_ok = extension_remove_item_direct(bag, slot, &result);
    }
    lock.lock();

    void* actual_item = nullptr;
    const bool readable = inventory_slot_locked(bag, slot, &actual_item);
    qol::contracts::QolInventoryItemRef actual_ref{};
    const bool identity_readable = qol::contracts::succeeded(
        runtime_state().host.item_facts->item_ref_at(bag, slot, &actual_ref));
    const bool identity_at_source = identity_readable &&
        actual_ref.bag == bag && actual_ref.slot == slot;
    const bool source_handle_cleared = identity_at_source &&
        actual_ref.item.slot == 0 && actual_ref.item.generation == 0;
    // A successfully removed source slot is no longer readable by
    // physical_item_at, so the empty slot itself is the postcondition.
    const bool removed_empty_slot = !readable && remove_call_ok && actual_item == nullptr;
    const bool removed = ((readable && identity_at_source && source_handle_cleared) ||
                          removed_empty_slot) &&
                         virtual_bag::original_to_extension_source_slot_postcondition(
                             expected_item, actual_item);
    if (!remove_call_ok) {
        VIRTBAG_LOG_ERROR(
            "ERROR native remove call failed context=%s bag=%d slot=%d result=%d; checking source identity postcondition",
            context != nullptr ? context : "unknown", bag, slot, result);
    }
    if (!removed) {
        VIRTBAG_LOG_ERROR(
                    "ERROR native remove postcondition failed context=%s bag=%d slot=%d expected=%p actual=%p readable=%d source_handle=%u/%u identity_readable=%d ref_bag=%d ref_slot=%d actual_handle=%u/%u",
                    context != nullptr ? context : "unknown", bag, slot, expected_item,
                    actual_item, readable ? 1 : 0, source_handle.slot,
                    source_handle.generation, identity_readable ? 1 : 0,
                    actual_ref.bag, actual_ref.slot, actual_ref.item.slot,
                    actual_ref.item.generation);
    }
    return removed;
}
void restore_module_view_locked();

void teardown_extension_bag_hosts_locked() {
    // Disable all three UI hosts in the same lifecycle transaction.
    store_teardown_locked();
    mix_teardown_locked();
    restore_module_view_locked();
    disable_extension_tab_buttons_locked();
}
void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param, uint64_t param2,
                           int64_t x = -1, int64_t y = -1) {
    // Do not format/write a line for every touch event. The previous tracing was
    // synchronous on the UI thread and also inflated 0x19 move latency.
    (void)phase; (void)event; (void)param; (void)param2; (void)x; (void)y;
    return;
    const uint64_t sequence = runtime_state().exit_trace_sequence.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    VIRTBAG_LOG("exit seq=%llu ms=%lld phase=%s event=0x%llx param=0x%llx param2=0x%llx x=%lld y=%lld mode=%d selected=%d inspected=%d overlay=%d overlay_index=%d direct=%d got=%d desc=%d saved_direct=%u saved_got=%u",
                static_cast<unsigned long long>(sequence), static_cast<long long>(milliseconds), phase,
                static_cast<unsigned long long>(event), static_cast<unsigned long long>(param),
                static_cast<unsigned long long>(param2), static_cast<long long>(x), static_cast<long long>(y),
                static_cast<int>(runtime_state().objects.bag.mode), runtime_state().objects.bag.selected,
                runtime_state().objects.bag.inspected, runtime_state().objects.module_view_installed ? 1 : 0, runtime_state().objects.module_view_index,
                raw_direct_bag_locked(), raw_got_bag_locked(), raw_desc_type_locked(),
                static_cast<unsigned int>(runtime_state().objects.original_current_direct),
                static_cast<unsigned int>(runtime_state().objects.original_current_got));
}
void ensure_state_loaded_locked();
bool persist_state_locked(bool force = false);
int original_bag_locked();
void set_original_bag_locked(int bag);
bool install_module_view_locked(int bag);
bool free_module_object_locked(int bag, int slot, void* use_token = nullptr);
void reset_extension_desc_item_hooks_locked();
void restore_extension_desc_popup_callbacks_locked();

bool module_cache_clear_preflight_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const ModuleUseState& use = runtime_state().objects.module_object_use[bag][slot];
            if (use.active) {
                VIRTBAG_LOG("module cache clear preflight rejected active object bag=%d slot=%d item=%p generation=%llu",
                            bag, slot, use.item,
                            static_cast<unsigned long long>(use.generation));
                return false;
            }
        }
    }
    return true;
}

bool clear_module_cache_locked() {
    if (!module_cache_clear_preflight_locked()) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            // Scheme C objects never enter g_inven, so the module owns them
            // until an explicit transfer is implemented. Drop them on slot
            // exit instead of retaining stale pointers across saves.
            if (!free_module_object_locked(bag, slot)) {
                VIRTBAG_LOG("module cache clear aborted; retained object bag=%d slot=%d item=%p",
                            bag, slot, runtime_state().objects.module_objects[bag][slot]);
                return false;
            }
        }
    }
    cancel_projected_drag_session_locked("module-cache-clear");
    reset_extension_desc_item_hooks_locked();
    runtime_state().ui.extension_desc_item = nullptr;
    runtime_state().ui.extension_desc_item_bag = -1;
    runtime_state().ui.extension_desc_item_slot = -1;
    runtime_state().objects.original_inventory.fill(nullptr);
    // 标签袋物品对象是 ItemPool 里的裸指针，跨存档槽位加载必须失效：游戏
    // SAVE_LoadItem 重置/回收池内存，旧指针会被新读入的存档物品复用（真机
    // 实证：返回主菜单不保存重进存档后，扩展标签/信息页显示成药水、金币）。
    // 此处只清引用不释放——对象可能已被池回收，再 ItemPool_Free 是 UAF；
    // 下次 install_extension_tab_buttons 按 types 重建新对象。
    for (void*& tab_item : runtime_state().ui.tab_bag_items) tab_item = nullptr;
    runtime_state().objects.original_bag_size_bag = -1;
    runtime_state().objects.original_bag_size = 0;
    runtime_state().objects.module_window_original_bag = -1;
    runtime_state().objects.original_current_direct = 0;
    runtime_state().objects.original_current_got = 0;
    runtime_state().objects.module_view_installed = false;
    runtime_state().objects.module_view_index = -1;
    runtime_state().persistence.dirty = false;
    runtime_state().ui.extension_touch_capture = false;
    runtime_state().ui.extension_drag = {};
    return true;
}

bool serialize_item_payload_locked(void* item,
                                   std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out,
                                   int* out_size) {
    return virtual_bag::save_item_payload(extension_serialize_item_payload, item, out, out_size) ==
           virtual_bag::PayloadValidation::kOk;
}

void release_temporary_item_locked(void* item) {
    if (item == nullptr) return;
    extension_bag_invalidate_item_handle(item);
    (void)extension_free_item(item);
}

void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context) {
    const virtual_bag::ManagedLoadResult result = virtual_bag::load_item_payload_exact(
        payload, payload_size, extension_serialize_item_payload, extension_load_item_payload,
        extension_free_item_payload);
    if (result.item != nullptr) return result.item;

    VIRTBAG_LOG("payload bridge reject context=%s reason=%s validation=%s size=%d",
                context,
                virtual_bag::managed_load_failure_reason(result.failure),
                virtual_bag::payload_validation_reason(result.validation), payload_size);
    return nullptr;
}

// 容量外槽位剔除的模型层实现见 virtual_bag::discard_over_capacity（Host 可测）；
// 唯一调用点是模块级保存 prepare 中扩展背包自己的校验
// virtual_bag_module_save_validate_locked，其他路径不做任何丢弃。

// P3c 准入化：加载由 save.slot_data provider 的 SessionBound load 承担（每次 session
// 激活回调 EB 内部 codec），本函数不再执行惰性 IO（读档/清缓存/换槽重读全部移除）。
// 未加载（epoch 未登记）/降级阻断时保持空状态——扫描与物品操作由调用点经
// extension_bag_session_admitted_locked() 明确拒绝；slot 漂移由 provider 的转换
// 回调（prepare_detach + load）处理，这里不得再读 current_save_slot 兜底。
void ensure_state_loaded_locked() {
    // 准入状态由 provider 回调维护（extension_bag_session_admitted_locked）；
    // 保留函数作为全部既有调用点的稳定锚，无 IO、无游戏状态读取。
}
bool inventory_slot_locked(int bag, int slot, void** out) {
    if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->physical_item_at == nullptr ||
        out == nullptr || bag < 0 || bag >= 6 || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    *out = nullptr;
    qol::contracts::QolItemHandle handle{};
    if (!qol::contracts::succeeded(runtime_state().host.item_facts->physical_item_at(bag, slot, &handle))) {
        return false;
    }
    // physical_item_at returns a handle that may be game-owned (original inventory)
    // or provider-owned (EB). Private custody only knows the latter, so resolve
    // through the Game Host handle bridge first.
    const auto* game = runtime_state().host.game;
    if (game != nullptr && game->item_abi.native_item_for_handle != nullptr &&
        qol::contracts::succeeded(game->item_abi.native_item_for_handle(handle, out)) &&
        *out != nullptr) {
        return true;
    }
    return extension_bag_private_item_for_handle(handle, out);
}

bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag) {
    if (runtime_state().host.game == nullptr || payload == nullptr || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.serialize_item == nullptr ||
        payload_size <= 0 || !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return false;
    }
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = nullptr;
        if (!inventory_slot_locked(target_bag, slot, &item)) continue;
        if (item == nullptr) continue;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> serialized{};
        int size = 0;
        if (!serialize_item_payload_locked(item, &serialized, &size) || size != payload_size) continue;
        if (std::memcmp(serialized.data(), payload, static_cast<size_t>(size)) == 0) return true;
    }
    return false;
}

int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag) {
    if (runtime_state().host.game == nullptr || payload == nullptr || extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.serialize_item == nullptr ||
        payload_size <= 0 || !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return -1;
    }
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        void* item = nullptr;
        if (!inventory_slot_locked(target_bag, slot, &item)) continue;
        if (item == nullptr) continue;
        std::array<uint8_t, virtual_bag::kSerializedItemBuffer> serialized{};
        int size = 0;
        if (serialize_item_payload_locked(item, &serialized, &size) && size == payload_size &&
            std::memcmp(serialized.data(), payload, static_cast<size_t>(size)) == 0) {
            return slot;
        }
    }
    return -1;
}

int original_inventory_handle_slot_locked(qol::contracts::QolItemHandle expected,
                                          int target_bag) {
    if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->physical_item_at == nullptr ||
        !virtual_bag::valid_original_transaction_bag(target_bag)) {
        return -1;
    }
    std::array<qol::contracts::QolItemHandle, virtual_bag::kSlotCount> handles{};
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        qol::contracts::QolItemHandle handle{};
        if (qol::contracts::succeeded(
                runtime_state().host.item_facts->physical_item_at(target_bag, slot, &handle))) {
            handles[static_cast<size_t>(slot)] = handle;
        }
    }
    return qol::features::extension_bag::model::find_handle_slot(
        expected, handles.data(), handles.size());
}

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

void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock) {
    const virtual_bag::PendingTransfer pending = runtime_state().objects.bag.pending;
    if (!pending.valid) return;
    if (!virtual_bag::valid_pending_transfer_domain(pending)) {
        isolate_pending_locked(virtual_bag::isolation_reason::kInvalidTransactionDomain,
                               "pending recovery");
        VIRTBAG_LOG("pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u",
                    static_cast<unsigned int>(pending.direction),
                    static_cast<unsigned int>(pending.src_bag),
                    static_cast<unsigned int>(pending.src_slot),
                    static_cast<unsigned int>(pending.dst_bag),
                    static_cast<unsigned int>(pending.dst_slot));
        runtime_state().objects.bag.pending = {};
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        return;
    }
    if (!virtual_bag::valid_pending_transfer_payload(pending)) {
        const virtual_bag::PayloadValidation payload_validation =
            virtual_bag::validate_serialized_payload_buffer(pending.payload.data(),
                                                            pending.payload.size(),
                                                            pending.payload_size);
        const virtual_bag::PayloadValidation source_validation =
            pending.source_payload_size == 0
                ? virtual_bag::PayloadValidation::kOk
                : virtual_bag::validate_serialized_payload_buffer(
                      pending.source_payload.data(), pending.source_payload.size(),
                      pending.source_payload_size);
        isolate_pending_locked(virtual_bag::isolation_reason::kInvalidPayload,
                               payload_validation != virtual_bag::PayloadValidation::kOk
                                   ? virtual_bag::payload_validation_reason(payload_validation)
                                   : virtual_bag::payload_validation_reason(source_validation));
        VIRTBAG_LOG("pending isolated reason=invalid_payload payload=%s source_payload=%s src=%u/%u dst=%u/%u",
                    virtual_bag::payload_validation_reason(payload_validation),
                    virtual_bag::payload_validation_reason(source_validation),
                    static_cast<unsigned int>(pending.src_bag),
                    static_cast<unsigned int>(pending.src_slot),
                    static_cast<unsigned int>(pending.dst_bag),
                    static_cast<unsigned int>(pending.dst_slot));
        runtime_state().objects.bag.pending = {};
        extension_bag_mark_dirty_locked();
        persist_state_locked();
        return;
    }
    const bool restore_module = runtime_state().objects.module_view_installed;
    const int restore_bag = runtime_state().objects.module_view_index;
    const int restore_original_bag = runtime_state().objects.module_window_original_bag;
    const virtual_bag::Mode restore_mode = runtime_state().objects.bag.mode;
    const int restore_selected = runtime_state().objects.bag.selected;
    const int restore_inspected = runtime_state().objects.bag.inspected;
    if (restore_module) restore_module_view_locked();
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(runtime_state().objects.bag, pending);
    VIRTBAG_LOG("pending recovery direction=%d action=%d", static_cast<int>(pending.direction),
                static_cast<int>(action));
    bool clear_pending = false;
    if (pending.direction == virtual_bag::kTransferExtensionToOriginal) {
        const bool source_matches = virtual_bag::item_matches_payload(
            runtime_state().objects.bag.items[pending.src_bag][pending.src_slot], pending);
        const int target_slot = original_inventory_payload_slot_locked(
            pending.payload.data(), pending.payload_size, pending.dst_bag);
        const auto recovery = virtual_bag::ext2orig_recovery_action(
            source_matches, target_slot >= 0);
        if (recovery == virtual_bag::Ext2OrigRecoveryAction::kRollbackTarget) {
            void* target_item = nullptr;
            const bool target_read = inventory_slot_locked(pending.dst_bag, target_slot,
                                                           &target_item);
            if (target_read && target_item != nullptr &&
                remove_item_direct_unlocked(lock, pending.dst_bag, target_slot, target_item,
                                            "pending ext2orig rollback target")) {
                if (runtime_state().host.game != nullptr && runtime_state().host.write_port->ui.refresh_item_area != nullptr) {
                    virtual_bag_call_original_refresh_item_area_with_guard();
                }
                clear_pending = true;
            } else {
                VIRTBAG_LOG("pending recovery: ext2orig target rollback not confirmed; retain pending slot=%d",
                            target_slot);
            }
        } else if (recovery == virtual_bag::Ext2OrigRecoveryAction::kRollback ||
                   recovery == virtual_bag::Ext2OrigRecoveryAction::kComplete) {
            clear_pending = true;
        } else {
            set_original_bag_locked(pending.dst_bag);
            uint32_t item_handle = 0;
            void* item = load_item_payload_tracked_locked(
                pending.payload.data(), pending.payload_size,
                "recover_pending_extension_to_original", &item_handle);
            bool loaded = item != nullptr;
            if (loaded && extension_bag_write_api() != nullptr &&
                extension_bag_write_api()->inventory.save_item_on_empty != nullptr) {
                if (!extension_save_item_on_empty(item, pending.dst_bag)) {
                    if (original_inventory_contains_payload_locked(
                            pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                        handover_tracked_item_locked(item_handle, item,
                                                     "recover ext2orig failed-write target observed");
                        loaded = false;
                    } else {
                        release_tracked_item_locked(item_handle, item,
                                                    "recover ext2orig insert failed");
                        loaded = false;
                    }
                }
            } else if (item != nullptr) {
                release_tracked_item_locked(item_handle, item,
                                            "recover ext2orig save unavailable");
                loaded = false;
            }
            if (loaded && !original_inventory_contains_payload_locked(
                               pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                VIRTBAG_LOG("pending recovery: target payload not confirmed; retain pending");
                loaded = false;
            }
            if (loaded) {
                handover_tracked_item_locked(item_handle, item,
                                             "recover ext2orig handover");
                clear_pending = true;
            } else if (original_inventory_contains_payload_locked(
                           pending.payload.data(), pending.payload_size, pending.dst_bag)) {
                // 插入结果为假但目标载荷已可见时，目标已接管；不再释放该对象。
                clear_pending = true;
            } else {
                VIRTBAG_LOG("pending recovery: extension->original retry retained");
            }
        }
    } else {
        clear_pending = action == virtual_bag::RecoveryAction::kRollback;
    }
    if (pending.direction != virtual_bag::kTransferExtensionToOriginal &&
        action == virtual_bag::RecoveryAction::kComplete) {
        if (pending.direction == virtual_bag::kTransferOriginalToExtension) {
            void* src = nullptr;
            if (!inventory_slot_locked(pending.src_bag, pending.src_slot, &src) || src == nullptr) {
                clear_pending = true;
            } else if (pending.source_payload_size > 0 &&
                       extension_bag_write_api() != nullptr &&
                       extension_bag_write_api()->inventory.remove_item_direct != nullptr) {
                std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
                int source_size = 0;
                if (!serialize_item_payload_locked(src, &source_payload, &source_size)) {
                    VIRTBAG_LOG("pending recovery: source serialization rejected");
                    clear_pending = false;
                } else {
                    const bool same_source = source_size == pending.source_payload_size &&
                                             std::memcmp(source_payload.data(),
                                                         pending.source_payload.data(),
                                                         pending.source_payload_size) == 0;
                    if (same_source) {
                        const bool source_removed = remove_item_direct_unlocked(
                            lock, pending.src_bag, pending.src_slot, src, "pending recovery");
                        if (runtime_state().host.game != nullptr &&
                            runtime_state().host.write_port->ui.refresh_item_area != nullptr) {
                            virtual_bag_call_original_refresh_item_area_with_guard();
                        }
                        if (source_removed) {
                            clear_pending = true;
                        } else {
                            VIRTBAG_LOG("pending recovery: source removal not confirmed; retain pending");
                            clear_pending = false;
                        }
                    } else {
                        VIRTBAG_LOG("pending recovery: source identity mismatch; retain pending");
                    }
                }
            }
        } else if (pending.direction == virtual_bag::kTransferExtensionToExtension) {
            // ext→ext 恒纯逻辑（§4.1）：此分支 = dst 槽 payload 不匹配，仅损坏/手工
            // 构造的 sidecar 可达。pending.payload 是 dst 提交态而非可入原版库存的
            // 源载荷，禁止按 ext→orig 重放（§4.2 不尝试重放或推断目标）；
            // 按 kRollback 语义仅清 pending。
            clear_pending = true;
        }
    }
    if (clear_pending) {
        runtime_state().objects.bag.pending = {};
        extension_bag_mark_dirty_locked();
    }
    // 恢复操作临时卸载投影；在退出前按原宿主和袋重新装载，避免 mode 与 UI 投影分离。
    if (restore_module) {
        const int host_bag = virtual_bag::valid_original_transaction_bag(restore_original_bag)
                                 ? restore_original_bag
                                 : 0;
        set_original_bag_locked(host_bag);
        const bool installed = virtual_bag::valid_index(restore_bag) &&
                               install_module_view_locked(restore_bag) &&
                               runtime_state().objects.module_view_installed &&
                               runtime_state().objects.module_view_index == restore_bag &&
                               runtime_state().objects.projected_item_root != nullptr;
        if (installed) {
            runtime_state().objects.bag.mode = restore_mode;
            runtime_state().objects.bag.selected = restore_selected;
            runtime_state().objects.bag.inspected = restore_inspected;
        } else {
            if (runtime_state().objects.module_view_installed) restore_module_view_locked();
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR pending recovery could not restore module projection bag=%d host=%d",
                          restore_bag, host_bag);
            runtime_state().objects.bag.mode = virtual_bag::Mode::kOriginal;
            runtime_state().objects.bag.selected = host_bag;
            runtime_state().objects.bag.inspected = -1;
        }
    }
    persist_state_locked();
}
void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    return load_item_payload_locked(descriptor.payload.data(), descriptor.payload_size,
                                    "materialize_module_item");
}
bool persist_state_locked(bool force) {
    (void)force;
    // The bool reports acceptance of the in-memory mutation for callers' rollback decisions;
    // it does not report durable staging. slot_data capture/complete persists state in the
    // core.save transaction, and force cannot bypass that boundary.
    return true;
}
bool item_pointer_known_locked(void* item) {
    if (item == nullptr) return true;  // 空指针由调用方各自处理
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (runtime_state().objects.module_objects[bag][slot] == item) return true;
        }
    }
    if (runtime_state().host.item_facts != nullptr && runtime_state().host.item_facts->physical_item_at != nullptr) {
        for (int idx = 0; idx < 6 * virtual_bag::kSlotCount; ++idx) {
            void* physical = nullptr;
            if (inventory_slot_locked(idx / virtual_bag::kSlotCount,
                                      idx % virtual_bag::kSlotCount, &physical) && physical == item) {
                return true;
            }
        }
    }
    for (int role = 0; role < 3; ++role) {
        void* character = nullptr;
        if (runtime_state().host.game == nullptr || runtime_state().host.character_extras->read.party_member == nullptr ||
            !qol::contracts::succeeded(
                runtime_state().host.character_extras->read.party_member(role, &character))) continue;
        if (character == nullptr) continue;
        for (int slot = 0; slot < 10; ++slot) {
            void* equipped = nullptr;
            if (runtime_state().host.character_extras->read.equipped_item == nullptr ||
                !qol::contracts::succeeded(
                    runtime_state().host.character_extras->read.equipped_item(character, slot, &equipped))) {
                continue;
            }
            if (equipped == item) return true;
        }
    }
    return false;
}
#undef make_desc_equip_gate
#undef consume_extension_item_after_native_locked
#undef clear_original_desc_locked
#undef original_bag_locked
#undef set_original_bag_locked
#undef bag_size_word_locked
#undef set_bag_size_word_locked
#undef original_bag_button_index
#undef install_module_view_locked
#undef ensure_module_view_root_current_locked
#undef restore_module_view_locked
#undef refresh_projection_if_overwritten_locked
#undef refresh_projected_module_view_after_move_locked
#undef refresh_module_item_area_locked
#undef sync_projected_slot_locked
#undef module_item_locked
#undef prepare_main_menu_locked
#undef ensure_state_loaded_locked
#undef persist_state_locked
#undef log_exit_trace_locked
#undef delete_extension_tab_buttons_locked
#undef disable_extension_tab_buttons_locked
#undef install_extension_tab_buttons_locked
#undef extension_tab_buttons_complete_locked
#undef free_module_object_locked
#undef materialize_module_item_locked
#undef release_temporary_item_locked
#undef module_object_generation_advance_locked
#undef inventory_slot_locked
#undef log_ownership_audit_locked
#undef drain_deferred_frees_locked
#undef item_pointer_known_locked
#undef teardown_extension_bag_hosts_locked
#undef module_cache_clear_preflight_locked
#undef clear_module_cache_locked
#undef commit_pending_extension_tab_locked
#undef handle_bag_drop_release_locked
#undef route_deferred_extension_to_original_locked
#undef route_projected_session_drop_locked
#undef reset_extension_desc_unequip_hook_locked
#undef reset_extension_desc_item_hooks_locked
#undef restore_extension_desc_popup_callbacks_locked
#undef move_original_to_extension_locked
#undef move_original_to_extension_slot_locked
#undef move_extension_to_original_locked
#undef move_extension_to_extension_locked
#undef route_projected_session_to_tab_locked
#undef module_slot_is_assignable_locked
#undef module_object_replacement_allowed_locked
#undef extension_tab_hit
#undef serialize_item_payload_locked
#undef load_item_payload_locked
#undef generate_transaction_id_locked
#undef txn_record_pending_locked
#undef txn_abort_locked
#undef isolate_pending_locked
#undef isolate_ext2orig_failure_locked
#undef original_inventory_contains_payload_locked
#undef original_inventory_payload_slot_locked
#undef original_inventory_handle_slot_locked
#undef recover_pending_transaction_locked
#undef try_equip_on_extension_tab_drop_locked
#undef extension_source_should_equip_locked
#undef equip_extension_source_on_tab_locked
#undef remove_item_direct_unlocked

int original_bag_locked_internal() { return ::original_bag_locked(); }
void set_original_bag_locked_internal(int bag) { ::set_original_bag_locked(bag); }
bool bag_size_word_locked_internal(int bag, uint32_t* out_word) {
    return ::bag_size_word_locked(bag, out_word);
}
bool set_bag_size_word_locked_internal(int bag, uint32_t word) {
    return ::set_bag_size_word_locked(bag, word);
}
int original_bag_button_index_internal(int64_t x, int64_t y) {
    return ::original_bag_button_index(x, y);
}
bool install_module_view_locked_internal(int bag) { return ::install_module_view_locked(bag); }
void restore_module_view_locked_internal() { ::restore_module_view_locked(); }
void refresh_projected_module_view_after_move_locked_internal() {
    ::refresh_projected_module_view_after_move_locked();
}
void refresh_module_item_area_locked_internal(int index) {
    ::refresh_module_item_area_locked(index);
}
bool sync_projected_slot_locked_internal(int bag, int slot) {
    return ::sync_projected_slot_locked(bag, slot);
}
void* module_item_locked_internal(int bag, int slot) { return ::module_item_locked(bag, slot); }
bool free_module_object_locked_internal(int bag, int slot, void* use_token) {
    return ::free_module_object_locked(bag, slot, use_token);
}
void reset_extension_desc_item_hooks_locked_internal() {
    ::reset_extension_desc_item_hooks_locked();
}
void restore_extension_desc_popup_callbacks_locked_internal() {
    ::restore_extension_desc_popup_callbacks_locked();
}
}  // namespace

#undef current_env
#undef isolation_now_ms_locked
#undef extension_apply_stuff
#undef read_item_cumulate_count
#undef read_save_is_ok
#undef read_has_empty_mercenary_slot
#undef extension_description_set_off
#undef extension_use_item
#undef extension_make_mercenary
#undef extension_refresh_bag_area
#undef item_count_encoding
#undef original_count_view
#undef category_is_equip
#undef category_is_extension_backpack
#undef handle_extension_tab_click_locked

uint64_t isolation_now_ms_locked() { return isolation_now_ms_locked_internal(); }
void ensure_state_loaded_locked() { ensure_state_loaded_locked_internal(); }
bool persist_state_locked(bool force) { return persist_state_locked_internal(force); }
void log_exit_trace_locked(const char* phase, uint64_t event, uint64_t param,
                           uint64_t param2, int64_t x, int64_t y) {
    log_exit_trace_locked_internal(phase, event, param, param2, x, y);
}
void* materialize_module_item_locked(const virtual_bag::Item& descriptor) {
    return materialize_module_item_locked_internal(descriptor);
}
void release_temporary_item_locked(void* item) {
    release_temporary_item_locked_internal(item);
}
bool inventory_slot_locked(int bag, int slot, void** out) {
    return inventory_slot_locked_internal(bag, slot, out);
}
bool item_pointer_known_locked(void* item) { return item_pointer_known_locked_internal(item); }
void teardown_extension_bag_hosts_locked() {
    teardown_extension_bag_hosts_locked_internal();
}
bool module_cache_clear_preflight_locked() {
    return module_cache_clear_preflight_locked_internal();
}
bool clear_module_cache_locked() { return clear_module_cache_locked_internal(); }
bool serialize_item_payload_locked(
    void* item, std::array<uint8_t, virtual_bag::kSerializedItemBuffer>* out, int* out_size) {
    return serialize_item_payload_locked_internal(item, out, out_size);
}
void* load_item_payload_locked(const uint8_t* payload, int payload_size, const char* context) {
    return load_item_payload_locked_internal(payload, payload_size, context);
}
void generate_transaction_id_locked(char* out) {
    generate_transaction_id_locked_internal(out);
}
bool txn_record_pending_locked(virtual_bag::TransactionContext* txn) {
    return txn_record_pending_locked_internal(txn);
}
void txn_abort_locked(const virtual_bag::TransactionContext& txn,
                      virtual_bag::TxnStage failed_at, const char* reason) {
    txn_abort_locked_internal(txn, failed_at, reason);
}
void isolate_pending_locked(const char* reason, const char* detail) {
    isolate_pending_locked_internal(reason, detail);
}
void isolate_ext2orig_failure_locked(const virtual_bag::TransactionContext& txn,
                                    virtual_bag::TxnStage phase, const char* reason,
                                    const char* detail) {
    isolate_ext2orig_failure_locked_internal(txn, phase, reason, detail);
}
bool original_inventory_contains_payload_locked(const uint8_t* payload, int payload_size,
                                                int target_bag) {
    return original_inventory_contains_payload_locked_internal(payload, payload_size, target_bag);
}
int original_inventory_payload_slot_locked(const uint8_t* payload, int payload_size,
                                           int target_bag) {
    return original_inventory_payload_slot_locked_internal(payload, payload_size, target_bag);
}
int original_inventory_handle_slot_locked(qol::contracts::QolItemHandle expected,
                                          int target_bag) {
    return original_inventory_handle_slot_locked_internal(expected, target_bag);
}
void recover_pending_transaction_locked(std::unique_lock<std::mutex>& lock) {
    recover_pending_transaction_locked_internal(lock);
}
bool remove_item_direct_unlocked(std::unique_lock<std::mutex>& lock, int bag, int slot,
                                 void* expected_item, const char* context) {
    return remove_item_direct_unlocked_internal(lock, bag, slot, expected_item, context);
}
