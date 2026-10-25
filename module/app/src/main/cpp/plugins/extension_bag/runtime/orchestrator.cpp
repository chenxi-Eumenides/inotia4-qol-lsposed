#include "game_ui_virtbag.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/runtime/ownership.h"
#include "plugins/extension_bag/runtime/lifecycle.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/ui/host_ui.h"
#include "plugins/extension_bag/runtime/tabs.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/transaction/transaction_coordinator.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/model/move_merge.h"
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
#include "plugins/extension_bag/runtime/projection.h"

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

bool remove_item_direct_unlocked(::qol::extension_bag::ReentryLock& lock, int bag, int slot,
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
bool module_cache_clear_preflight_locked() {
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
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
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
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

void recover_pending_transaction_locked(::qol::extension_bag::ReentryLock& lock) {
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
    for (int bag = 0; bag < virtual_bag::active_extension_bags(); ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (runtime_state().objects.module_objects[bag][slot] == item) return true;
        }
    }
    if (original_inventory_contains_cached_locked(item)) return true;
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
