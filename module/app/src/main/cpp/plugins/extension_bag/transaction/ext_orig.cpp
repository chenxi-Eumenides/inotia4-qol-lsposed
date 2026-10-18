#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/assembly/move_merge.h"

#include <algorithm>
#include <cstring>

bool move_original_to_extension_slot_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                             int src_slot, int dst_bag,
                                             qol::contracts::QolItemHandle expected_item) {
    if (!virtual_bag::valid_original_transaction_bag(src_bag)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject original->extension invalid source bag=%d", src_bag);
        return false;
    }
    if (src_slot < 0 || src_slot >= virtual_bag::kSlotCount) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject original->extension invalid source slot=%d bag=%d",
                    src_slot, src_bag);
        return false;
    }
    if (!virtual_bag::valid_index(dst_bag) || runtime_state().objects.bag.capacities[dst_bag] == 0 ||
        extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.serialize_item == nullptr ||
        runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->cumulate_count == nullptr ||
        runtime_state().host.item_facts->item_ref_at == nullptr ||
        extension_bag_write_api()->inventory.remove_item_direct == nullptr) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject original->extension dst=%d capacity=%d symbols=%d",
                    dst_bag,
                    virtual_bag::valid_index(dst_bag) ? runtime_state().objects.bag.capacities[dst_bag] : 0,
                    extension_bag_write_api() != nullptr &&
                    extension_bag_write_api()->inventory.serialize_item != nullptr &&
                    runtime_state().host.item_facts != nullptr && runtime_state().host.item_facts->cumulate_count != nullptr &&
                    extension_bag_write_api()->inventory.remove_item_direct != nullptr ? 1 : 0);
        return false;
    }
    void* src_item = nullptr;
    if (!inventory_slot_locked(src_bag, src_slot, &src_item) || src_item == nullptr) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject original->extension source empty bag=%d slot=%d",
                    src_bag, src_slot);
        return false;
    }
    qol::contracts::QolInventoryItemRef source_ref{};
    if (!qol::contracts::succeeded(
            runtime_state().host.item_facts->item_ref_at(src_bag, src_slot, &source_ref)) ||
        source_ref.bag != src_bag || source_ref.slot != src_slot ||
        source_ref.item.slot == 0 || source_ref.item.generation == 0 ||
        (expected_item.slot != 0 &&
         (source_ref.item.slot != expected_item.slot ||
          source_ref.item.generation != expected_item.generation))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension stale source identity bag=%d slot=%d",
                     src_bag, src_slot);
        return false;
    }
    qol::contracts::QolItemHandle source_handle{};
    if (!extension_bag_item_handle_for_pointer(src_item, &source_handle) ||
        source_handle.slot != source_ref.item.slot ||
        source_handle.generation != source_ref.item.generation) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension replaced source bag=%d slot=%d",
                     src_bag, src_slot);
        return false;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
    int serialized_size = 0;
    if (!serialize_item_payload_locked(src_item, &source_payload, &serialized_size)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move: item serialization rejected");
        return false;
    }
    // descriptor.count 恒为 canonical 128a+b（R-38/R-48）：orig→ext 收编保留物品
    // 全部位段，堆叠上限关闭态（R-47 决策 b）下 getter 只返回 b 视图，这里必须
    // 用 canonical 读取，否则 sidecar 读档会把 a 段对齐抹掉。
    const int src_count = static_cast<int>(virtual_bag_canonical_count(src_item, 1));
    const int src_category = original_item_category_locked(src_item);
    const int capacity = runtime_state().objects.bag.capacities[dst_bag];

    // prepared：收集不可变事务快照。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferOriginalToExtension;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(dst_bag);
    txn.source.category = src_category;
    txn.source.count = src_count;
    txn.source.payload_size = static_cast<uint16_t>(serialized_size);
    txn.source.payload = source_payload;

    virtual_bag::Item committed{};
    int dst_slot = -1;
    bool merged = false;
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            if (!module_slot_is_assignable_locked(dst_bag, slot)) continue;
            committed = txn.source;
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject original->extension destination full bag=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }
    txn.dst_slot = static_cast<uint8_t>(dst_slot);
    txn.merged = merged;
    txn.committed = committed;
    txn.previous_dst = runtime_state().objects.bag.items[dst_bag][dst_slot];
    generate_transaction_id_locked(txn.transaction_id);
    if (txn.merged) {
        QOL_LOG_INFO(QolDomain::kExtensionBag, "txn prepared id=%s original merge src=%d/%d -> extension=%d/%d count=%d+%d=%d",
                    txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, src_count,
                    txn.previous_dst.count, txn.committed.count);
    }

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：写扩展逻辑目标；物化失败即回滚（源保持原版实态）。
    virtual_bag::txn_apply_logical(&runtime_state().objects.bag, txn);
    extension_bag_mark_dirty_locked();
    if (module_item_locked(dst_bag, dst_slot) == nullptr) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "target extension item could not be materialized; source retained");
        return false;
    }
    if (!module_object_release_allowed_locked(
            dst_bag, dst_slot, runtime_state().objects.module_objects[dst_bag][dst_slot],
            "original-to-extension commit", false, nullptr)) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "target extension object is in use; source retained");
        return false;
    }
    if (!free_module_object_locked(dst_bag, dst_slot)) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "target extension object release rejected");
        return false;
    }

    // original-state-updated：删除原版源并以原版实态复核（不只信删除函数返回值）。
    qol::contracts::QolInventoryItemRef before_remove_ref{};
    if (!qol::contracts::succeeded(
            runtime_state().host.item_facts->item_ref_at(src_bag, src_slot, &before_remove_ref)) ||
        before_remove_ref.item.slot != source_handle.slot ||
        before_remove_ref.item.generation != source_handle.generation) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "original source identity changed before removal");
        return false;
    }
    if (!remove_item_direct_unlocked(lock, src_bag, src_slot, src_item,
                                     "original-to-extension")) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "original source slot remained occupied");
        return false;
    }

    // committed：不做猜测性逆转。清 pending、回收模块投影对象、刷新原版网格。
    runtime_state().objects.bag.pending = {};
    // orig→ext 后保持当前视图（对齐原版"移动后停留"）。
    // 不切 mode、不装投影：mode 与投影始终成对，此前"切 mode 不装投影"的
    // overlay 错位从根上不可达；P2 移动锁也不再因 API 移动触发。
    // 当前视图只同步发生变化的扩展槽；非扩展视图仍走原版刷新。
    if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == dst_bag) {
        sync_projected_slot_locked(dst_bag, dst_slot);
    } else if (runtime_state().host.game != nullptr && runtime_state().host.write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    QOL_LOG_INFO(QolDomain::kExtensionBag, "txn committed id=%s original->extension bag=%d slot=%d cat=%d count=%d src=%d/%d",
                txn.transaction_id, dst_bag, dst_slot, src_category, txn.committed.count,
                src_bag, src_slot);
    return true;
}

bool move_original_to_extension_locked(std::unique_lock<std::mutex>& lock, int dst_bag,
                                       void* moving_control) {
    if (!virtual_bag::valid_index(dst_bag) || runtime_state().objects.bag.capacities[dst_bag] == 0 ||
        runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->detail_item_context == nullptr) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject original->extension dst=%d control=%p capacity=%d symbol=%d",
                     dst_bag, moving_control,
                     virtual_bag::valid_index(dst_bag) ? runtime_state().objects.bag.capacities[dst_bag] : 0,
                     runtime_state().host.item_facts != nullptr && runtime_state().host.item_facts->detail_item_context != nullptr ? 1 : 0);
        return false;
    }
    qol::contracts::QolItemDetailContext source{};
    if (!qol::contracts::succeeded(runtime_state().host.item_facts->detail_item_context(&source)) ||
        source.desc_type != 2 || source.bag != original_bag_locked() ||
        source.slot < 0 || source.slot >= virtual_bag::kSlotCount ||
        source.item.slot == 0 || source.item.generation == 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "cross move reject original->extension detail context stale");
        return false;
    }
    const bool moved = move_original_to_extension_slot_locked(
        lock, source.bag, source.slot, dst_bag, source.item);
    if (moved) reset_drag_state_locked(moving_control);
    return moved;
}
namespace {

bool rollback_ext2orig_insertion_locked(std::unique_lock<std::mutex>& lock, int target_bag,
                                        int inserted_slot, void* item,
                                        qol::contracts::QolItemHandle item_handle) {
    // 落位复核可能已通过对象回退找到槽；回滚必须复用该槽并重新核对句柄身份。
    if (inserted_slot < 0 || inserted_slot >= virtual_bag::kSlotCount || item == nullptr ||
        runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->item_ref_at == nullptr) {
        return false;
    }
    void* current = nullptr;
    if (!inventory_slot_locked(target_bag, inserted_slot, &current) || current != item) {
        return false;
    }
    qol::contracts::QolItemHandle current_handle{};
    qol::contracts::QolInventoryItemRef current_ref{};
    if (!extension_bag_item_handle_for_pointer(current, &current_handle) ||
        current_handle.slot != item_handle.slot ||
        current_handle.generation != item_handle.generation ||
        !qol::contracts::succeeded(
            runtime_state().host.item_facts->item_ref_at(target_bag, inserted_slot, &current_ref)) ||
        current_ref.bag != target_bag || current_ref.slot != inserted_slot ||
        current_ref.item.slot != item_handle.slot ||
        current_ref.item.generation != item_handle.generation) {
        return false;
    }
    return remove_item_direct_unlocked(lock, target_bag, inserted_slot, item,
                                        "ext2orig insertion verification rollback");
}

}
bool move_extension_to_original_locked(std::unique_lock<std::mutex>& lock, int src_bag,
                                       int src_slot, int target_bag) {
    if (!virtual_bag::valid_original_transaction_bag(target_bag) ||
        !virtual_bag::valid_index(src_bag) ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount ||
        extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->inventory.save_item_on_empty == nullptr ||
        extension_bag_write_api()->inventory.remove_item_direct == nullptr) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->original src=%d/%d target_bag=%d", src_bag,
                    src_slot, target_bag);
        return false;
    }
    // VM-40 宿主容量守卫：目标袋为投影宿主时（扩展视图宿主常是原版袋 0——
    // 用户拖回宿主袋是最常见路径），事务期把被投影替换的容量字恢复真实值，
    // 使下方 fn_get_bag_size 满包预检与 INVEN_SaveItemOnEmpty 空槽扫描按真实
    // 容量判定（扩展容量 < 真实容量时宿主袋被误判已满、> 真实容量时越窗扫描）；
    // RAII 保证任何出口写回投影值（成功后由调用方 refresh 重刷控件）。
    const bool host_restore = virtual_bag::ext2orig_requires_host_capacity_restore(
        runtime_state().objects.module_view_installed, target_bag, runtime_state().objects.module_window_original_bag);
    constexpr uint32_t kTxnCapacityMask = (1u << 25) - 1u;
    const int host_word_bag = host_restore ? runtime_state().objects.original_bag_size_bag : -1;
    uint32_t host_projected_word = 0;
    struct HostCapacityGuard {
        int bag = -1;
        uint32_t projected = 0;
        bool active = false;

        bool restore() {
            if (!active) return true;
            if (!set_bag_size_word_locked(bag, projected)) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR ext2orig projected host capacity restore failed bag=%d word=0x%x",
                              bag, projected);
                return false;
            }
            active = false;
            return true;
        }

        ~HostCapacityGuard() {
            if (active && !restore()) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR ext2orig host capacity remains unprojected bag=%d",
                              bag);
            }
        }
    } host_capacity_guard{};
    if (host_restore) {
        if (host_word_bag < 0 || !bag_size_word_locked(host_word_bag, &host_projected_word)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR ext2orig host capacity read failed target=%d host=%d",
                          target_bag, host_word_bag);
            return false;
        }
        // 只有成功保存投影字后才启用 guard，避免读取失败时用零覆盖宿主容量。
        host_capacity_guard.bag = host_word_bag;
        host_capacity_guard.projected = host_projected_word;
        host_capacity_guard.active = true;
        const uint32_t real_word =
            (host_projected_word & ~kTxnCapacityMask) |
            (runtime_state().objects.original_bag_size & kTxnCapacityMask);
        if (!set_bag_size_word_locked(host_word_bag, real_word)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR ext2orig real host capacity write failed bag=%d word=0x%x",
                          host_word_bag, real_word);
            const bool restored = host_capacity_guard.restore();
            if (!restored) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR ext2orig host capacity recovery failed bag=%d",
                              host_word_bag);
            }
            return false;
        }
        QOL_LOG_INFO(QolDomain::kExtensionBag, "ext2orig host capacity restored target=%d window=%d projected=0x%x real=0x%x",
                    target_bag, runtime_state().objects.module_window_original_bag, host_projected_word,
                    runtime_state().objects.original_bag_size);
    }

    const auto run_transaction = [&]() -> bool {
    int target_size = 0;
    if (runtime_state().host.game != nullptr && runtime_state().host.inventory_extras->read.bag_size != nullptr &&
        qol::contracts::succeeded(runtime_state().host.inventory_extras->read.bag_size(target_bag, &target_size)) &&
        target_size <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->original target bag unavailable=%d", target_bag);
        return false;
    }
    // 满包预检：INVEN_SaveItemOnEmpty 是纯首空槽扫描（0x104be0，满则返回 0 无副作用），
    // 事务入口先行拒绝，避免为注定失败的插入创建 pending/隔离记录或切换当前袋。
    if (runtime_state().host.game != nullptr && runtime_state().host.inventory_extras->read.bag_size != nullptr &&
        qol::contracts::succeeded(runtime_state().host.inventory_extras->read.bag_size(target_bag, &target_size))) {
        const int bag_size = target_size;
        bool has_empty = false;
        for (int slot = 0; slot < bag_size && slot < virtual_bag::kSlotCount; ++slot) {
            void* existing = nullptr;
            if (!inventory_slot_locked(target_bag, slot, &existing) || existing == nullptr) {
                has_empty = true;
                break;
            }
        }
        if (!has_empty) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->original target full=%d size=%d",
                        target_bag, bag_size);
            return false;
        }
    }
    const virtual_bag::Item source = runtime_state().objects.bag.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->original source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    const virtual_bag::PayloadValidation source_validation =
        virtual_bag::validate_serialized_payload_buffer(source.payload.data(), source.payload.size(),
                                                        source.payload_size);
    if (source_validation != virtual_bag::PayloadValidation::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->original reason=%s src=%d/%d size=%u",
                    virtual_bag::payload_validation_reason(source_validation), src_bag, src_slot,
                    static_cast<unsigned int>(source.payload_size));
        return false;
    }

    // prepared：源载荷已验证；dst_slot 语义为占位 0（实际落位由 SaveItemOnEmpty 决定，
    // 恢复流程按 payload 扫描定位，不依赖该字段）。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferExtensionToOriginal;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(target_bag);
    txn.source = source;
    txn.committed = source;
    generate_transaction_id_locked(txn.transaction_id);

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：Load 临时对象并入账（P4.3：Load 成功即 allocate；扩展逻辑源未变）。
    // 失败先回滚逻辑态与 pending，再释放仍归模块所有的临时对象。
    uint32_t item_handle = 0;
    void* item = load_item_payload_tracked_locked(source.payload.data(), source.payload_size,
                                                  "move_extension_to_original", &item_handle);
    qol::contracts::QolItemHandle inserted_handle{};
    if (item != nullptr &&
        (!extension_bag_register_item_handle(item) ||
         !extension_bag_item_handle_for_pointer(item, &inserted_handle))) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                                        virtual_bag::isolation_reason::kInsertFailed,
                                        "ext2orig item handle registration");
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "loaded item handle registration failed; extension source retained");
        release_tracked_item_locked(item_handle, item, "ext2orig handle registration failed");
        if (runtime_state().objects.module_view_installed && virtual_bag::valid_index(runtime_state().objects.module_window_original_bag)) {
            set_original_bag_locked(runtime_state().objects.module_window_original_bag);
            refresh_module_item_area_locked(runtime_state().objects.module_view_index);
        }
        reset_drag_state_locked(nullptr);
        return false;
    }
    // VM-40 宿主身份守卫：投影安装中 CUR_BAG 是宿主身份的一部分（R-27 成对写），
    // 事务内切换会让宿主记录与 CUR_BAG 分叉（下一次 install 把用户目标袋误选为
    // 宿主——原版袋容量字被写扩展容量、扩展内容投影上去）。投影中不切换；
    // SaveItemOnEmpty 显式携带 target_bag，不依赖 CUR_BAG。
    if (item != nullptr &&
        virtual_bag::ext2orig_should_switch_display_bag(runtime_state().objects.module_view_installed)) {
        set_original_bag_locked(target_bag);
    }
    const auto find_inserted_slot = [&]() {
        if (item == nullptr) return -1;
        int slot = original_inventory_handle_slot_locked(inserted_handle, target_bag);
        if (slot >= 0) return slot;
        for (int candidate = 0; candidate < virtual_bag::kSlotCount; ++candidate) {
            void* current = nullptr;
            if (inventory_slot_locked(target_bag, candidate, &current) && current == item) {
                return candidate;
            }
        }
        return original_inventory_payload_slot_locked(
            source.payload.data(), source.payload_size, target_bag);
    };
    if (item == nullptr || !extension_save_item_on_empty(item, target_bag)) {
        // P4.5：Load 失败或插入失败隔离。扩展源逻辑物品保留（payload 字节未丢），
        // 事务上下文留作只读诊断；不构造可移动替代项。
        isolate_ext2orig_failure_locked(txn,
                                        item == nullptr ? virtual_bag::TxnStage::kLogicalUpdated
                                                        : virtual_bag::TxnStage::kOriginalUpdated,
                                        item == nullptr ? virtual_bag::isolation_reason::kLoadFailed
                                                        : virtual_bag::isolation_reason::kInsertFailed,
                                        item == nullptr ? "move_extension_to_original load"
                                                        : "move_extension_to_original insert");
        const int failed_insert_slot = find_inserted_slot();
        const bool rollback_confirmed = failed_insert_slot < 0 ||
            rollback_ext2orig_insertion_locked(lock, target_bag, failed_insert_slot,
                                               item, inserted_handle);
        if (rollback_confirmed) {
            txn_abort_locked(txn,
                             item != nullptr ? virtual_bag::TxnStage::kOriginalUpdated
                                             : virtual_bag::TxnStage::kLogicalUpdated,
                             "target insertion failed; extension source retained");
        }
        if (!rollback_confirmed) {
            handover_tracked_item_locked(item_handle, item,
                                         "ext2orig insertion failure rollback unconfirmed");
        } else {
            release_tracked_item_locked(item_handle, item, "ext2orig insert failed");
        }
        if (runtime_state().objects.module_view_installed && virtual_bag::valid_index(runtime_state().objects.module_window_original_bag)) {
            set_original_bag_locked(runtime_state().objects.module_window_original_bag);
            refresh_module_item_area_locked(runtime_state().objects.module_view_index);
        }
        reset_drag_state_locked(nullptr);
        return false;
    }

    // original-state-updated：用预登记 handle 确认物理槽，避免将插件私表未绑定的
    // Game-owned handle 当成空槽；pointer/payload 仅作为兼容回退核验。
    const int inserted_slot = find_inserted_slot();
    if (inserted_slot < 0) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kSlotNotFound,
                                        "ext2orig insertion verify");
        // 未能定位实际目标时保留 pending 和临时对象归属，恢复器可再次核验两侧。
        handover_tracked_item_locked(item_handle, item,
                                     "ext2orig slot-not-found rollback unconfirmed");
        if (runtime_state().objects.module_view_installed && virtual_bag::valid_index(runtime_state().objects.module_window_original_bag)) {
            set_original_bag_locked(runtime_state().objects.module_window_original_bag);
            refresh_module_item_area_locked(runtime_state().objects.module_view_index);
        }
        reset_drag_state_locked(nullptr);
        return false;
    }
    // 在提交或释放扩展源前检查投影容量恢复；失败时目标回滚，扩展源保持不变。
    if (!host_capacity_guard.restore()) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kInsertFailed,
                                        "projected host capacity restore failed");
        if (rollback_ext2orig_insertion_locked(lock, target_bag, inserted_slot,
                                               item, inserted_handle)) {
            txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                             "host capacity restore failed; extension source retained");
            release_tracked_item_locked(item_handle, item,
                                        "ext2orig host capacity rollback");
        } else {
            handover_tracked_item_locked(item_handle, item,
                                         "ext2orig host capacity rollback unconfirmed");
        }
        reset_drag_state_locked(nullptr);
        return false;
    }
    if (!module_object_release_allowed_locked(
            src_bag, src_slot, runtime_state().objects.module_objects[src_bag][src_slot],
            "extension-to-original commit", false, nullptr)) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kInsertFailed,
                                        "source extension object is in use");
        if (!rollback_ext2orig_insertion_locked(lock, target_bag, inserted_slot,
                                                item, inserted_handle)) {
            handover_tracked_item_locked(item_handle, item,
                                         "ext2orig rollback removal unconfirmed");
            reset_drag_state_locked(nullptr);
            return false;
        }
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "source extension object release rejected");
        release_tracked_item_locked(item_handle, item, "ext2orig release rejected rollback");
        reset_drag_state_locked(nullptr);
        return false;
    }
    // P4.3：入库已确认 → 原版库存接管终态（此后模块不得触碰 item 指针）。
    if (!free_module_object_locked(src_bag, src_slot)) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kInsertFailed,
                                        "source extension object release rejected");
        if (!rollback_ext2orig_insertion_locked(lock, target_bag, inserted_slot,
                                                item, inserted_handle)) {
            handover_tracked_item_locked(item_handle, item,
                                         "ext2orig rollback removal unconfirmed");
            reset_drag_state_locked(nullptr);
            return false;
        }
        // 释放调用失败时保留 pending；恢复器会依据源/目标实态裁决，不能提前宣告回滚完成。
        release_tracked_item_locked(item_handle, item, "ext2orig release rejected rollback");
        reset_drag_state_locked(nullptr);
        return false;
    }
    handover_tracked_item_locked(item_handle, item, "ext2orig handover");

    // committed：清扩展源逻辑项、恢复投影、清 pending。已移交对象不参与模块回滚。
    runtime_state().objects.bag.items[src_bag][src_slot] = {};
    extension_bag_mark_dirty_locked();
    runtime_state().objects.bag.pending = {};
    // 保持当前视图：仅拖放上下文（扩展视图安装中 = 当前视图即扩展袋）停留该视图；
    // API 路径（原版视图）不改写视图字段。
    if (runtime_state().objects.module_view_installed) {
        runtime_state().objects.bag.mode = virtual_bag::Mode::kModule;
        runtime_state().objects.bag.selected = src_bag;
        runtime_state().objects.bag.inspected = -1;
    }
    // 提交后的显示袋切换同样受 VM-40 宿主身份守卫：投影安装中宿主不变，
    // 拖放调用方的 refresh_projected_module_view_after_move_locked 负责刷新；
    // API（非投影）路径保留切到目标袋的既有语义。
    if (virtual_bag::ext2orig_should_switch_display_bag(runtime_state().objects.module_view_installed)) {
        set_original_bag_locked(target_bag);
    }
    reset_drag_state_locked(nullptr);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "txn committed id=%s extension->original src=%d/%d cat=%d count=%d target_bag=%d slot=%d",
                txn.transaction_id, src_bag, src_slot, txn.source.category, txn.source.count,
                target_bag, inserted_slot);
    return true;
    };
    const bool moved = run_transaction();
    const bool projected_capacity_restored = host_capacity_guard.restore();
    if (!projected_capacity_restored && host_restore) {
        const int host_bag = virtual_bag::valid_original_transaction_bag(
                                 runtime_state().objects.module_window_original_bag)
                                 ? runtime_state().objects.module_window_original_bag
                                 : 0;
        if (runtime_state().objects.module_view_installed) restore_module_view_locked();
        runtime_state().objects.bag.mode = virtual_bag::Mode::kOriginal;
        runtime_state().objects.bag.selected = host_bag;
        runtime_state().objects.bag.inspected = -1;
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR ext2orig disabled projected view after capacity restore failure host=%d",
                      host_bag);
    }
    return moved && projected_capacity_restored;
}
