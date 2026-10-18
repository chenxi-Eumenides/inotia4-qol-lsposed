#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/assembly/move_merge.h"

#include <algorithm>
#include <cstring>

namespace {

bool swap_extension_slots_locked(int src_bag, int src_slot, int dst_bag, int dst_slot) {
    const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    const virtual_bag::Item destination = g_virtual_bag_state.items[dst_bag][dst_slot];
    if (source.category <= 0 || source.count <= 0 || destination.category <= 0 ||
        destination.count <= 0 || !virtual_bag::valid_payload(destination)) {
        return false;
    }

    const ModuleUseState& source_use = g_module_object_use[src_bag][src_slot];
    const ModuleUseState& destination_use = g_module_object_use[dst_bag][dst_slot];
    if (!virtual_bag::extension_swap_tokens_available(
            source_use.active, source_use.pending_release, destination_use.active,
            destination_use.pending_release)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "txn reject extension swap token active src=%d/%d active=%d pending=%d "
                    "dst=%d/%d active=%d pending=%d",
                    src_bag, src_slot, source_use.active ? 1 : 0,
                    source_use.pending_release ? 1 : 0, dst_bag, dst_slot,
                    destination_use.active ? 1 : 0,
                    destination_use.pending_release ? 1 : 0);
        return false;
    }

    const auto previous_source_object = g_module_objects[src_bag][src_slot];
    const auto previous_destination_object = g_module_objects[dst_bag][dst_slot];
    const int previous_source_category = g_module_object_categories[src_bag][src_slot];
    const int previous_destination_category = g_module_object_categories[dst_bag][dst_slot];
    const uint32_t previous_source_hash = g_module_object_hashes[src_bag][src_slot];
    const uint32_t previous_destination_hash = g_module_object_hashes[dst_bag][dst_slot];
    const uint32_t previous_source_handle = g_module_object_handles[src_bag][src_slot];
    const uint32_t previous_destination_handle = g_module_object_handles[dst_bag][dst_slot];
    const ModuleUseState previous_source_use = source_use;
    const ModuleUseState previous_destination_use = destination_use;
    const ownership::Ledger previous_ledger = g_ownership_ledger;
    const bool previous_dirty = g_item_state_dirty;
    bool objects_swapped = false;

    // 非空缓存若与 descriptor 不一致，不能让 materialize 先释放旧指针再尝试
    // swap，否则中途失败无法恢复原对象。该事务只对一致缓存或空缓存（可新建）开放。
    if ((previous_source_object != nullptr &&
         (previous_source_category != source.category ||
          previous_source_hash != virtual_bag::payload_hash(source))) ||
        (previous_destination_object != nullptr &&
         (previous_destination_category != destination.category ||
          previous_destination_hash != virtual_bag::payload_hash(destination)))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "txn reject extension swap stale endpoint src=%d/%d dst=%d/%d",
                    src_bag, src_slot, dst_bag, dst_slot);
        return false;
    }
    void* source_object = previous_source_object == nullptr
        ? module_item_locked(src_bag, src_slot) : previous_source_object;
    void* destination_object = previous_destination_object == nullptr
        ? module_item_locked(dst_bag, dst_slot) : previous_destination_object;
    const bool source_materialized = previous_source_object == nullptr && source_object != nullptr;
    const bool destination_materialized =
        previous_destination_object == nullptr && destination_object != nullptr;
    // 物化可能创建新 handle；只有仍绑定原对象的快照 handle 才可复用，冷缓存
    // 的新 handle 必须从当前槽读取，不能被旧的 0 覆盖。
    const uint32_t source_handle_for_object =
        virtual_bag::handle_if_object_matches(
            source_object, g_module_objects[src_bag][src_slot],
            g_module_object_handles[src_bag][src_slot]);
    const uint32_t destination_handle_for_object =
        virtual_bag::handle_if_object_matches(
            destination_object, g_module_objects[dst_bag][dst_slot],
            g_module_object_handles[dst_bag][dst_slot]);

    auto restore = [&](const virtual_bag::TransactionContext* txn, const char* reason) {
        if (txn != nullptr) {
            txn_abort_locked(*txn, virtual_bag::TxnStage::kLogicalUpdated, reason);
        }
        if (source_materialized) {
            const int bag = objects_swapped ? dst_bag : src_bag;
            const int slot = objects_swapped ? dst_slot : src_slot;
            // 提交后 source_object 已在目标槽，不能再用入参 source_slot 查找。
            if (g_module_objects[bag][slot] == source_object) {
                free_module_object_locked(bag, slot);
            }
        }
        if (destination_materialized) {
            const int bag = objects_swapped ? src_bag : dst_bag;
            const int slot = objects_swapped ? src_slot : dst_slot;
            // 提交后 destination_object 已在源槽；回滚必须按交换后的实际槽位清理。
            if (g_module_objects[bag][slot] == destination_object) {
                free_module_object_locked(bag, slot);
            }
        }
        g_module_objects[src_bag][src_slot] = previous_source_object;
        g_module_objects[dst_bag][dst_slot] = previous_destination_object;
        g_module_object_categories[src_bag][src_slot] = previous_source_category;
        g_module_object_categories[dst_bag][dst_slot] = previous_destination_category;
        g_module_object_hashes[src_bag][src_slot] = previous_source_hash;
        g_module_object_hashes[dst_bag][dst_slot] = previous_destination_hash;
        g_module_object_handles[src_bag][src_slot] = previous_source_handle;
        g_module_object_handles[dst_bag][dst_slot] = previous_destination_handle;
        g_module_object_use[src_bag][src_slot] = previous_source_use;
        g_module_object_use[dst_bag][dst_slot] = previous_destination_use;
        g_ownership_ledger = previous_ledger;
        g_item_state_dirty = previous_dirty;
        if (g_module_view_installed) {
            if (g_module_view_index == src_bag) {
                void* control = valid_child_locked(g_projected_item_root, src_slot);
                if (control != nullptr) (void)extension_bag_ui_set_control_item(control, previous_source_object);
            }
            if (g_module_view_index == dst_bag) {
                void* control = valid_child_locked(g_projected_item_root, dst_slot);
                if (control != nullptr) (void)extension_bag_ui_set_control_item(control, previous_destination_object);
            }
        }
    };

    if (source_object == nullptr || destination_object == nullptr) {
        restore(nullptr, "swap endpoint materialization failed");
        QOL_LOG_WARN(QolDomain::kExtensionBag, "txn reject extension swap endpoint materialization failed src=%d/%d dst=%d/%d",
                    src_bag, src_slot, dst_bag, dst_slot);
        return false;
    }

    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferExtensionToExtension;
    txn.original_stage_applicable = false;
    txn.swapped = true;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(dst_bag);
    txn.dst_slot = static_cast<uint8_t>(dst_slot);
    txn.source = source;
    txn.committed = source;
    txn.previous_dst = destination;
    generate_transaction_id_locked(txn.transaction_id);
    if (!txn_record_pending_locked(&txn)) {
        restore(nullptr, "swap pending record failed");
        return false;
    }
    virtual_bag::txn_apply_logical(&g_virtual_bag_state, txn);

    g_module_objects[src_bag][src_slot] = destination_object;
    g_module_objects[dst_bag][dst_slot] = source_object;
    g_module_object_categories[src_bag][src_slot] = destination.category;
    g_module_object_categories[dst_bag][dst_slot] = source.category;
    g_module_object_hashes[src_bag][src_slot] = virtual_bag::payload_hash(destination);
    g_module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(source);
    g_module_object_handles[src_bag][src_slot] = destination_handle_for_object;
    g_module_object_handles[dst_bag][dst_slot] = source_handle_for_object;
    objects_swapped = true;

    if (!module_object_generation_advance_locked(src_bag, src_slot, "extension swap source") ||
        !module_object_generation_advance_locked(dst_bag, dst_slot, "extension swap destination")) {
        restore(&txn, "swap generation advance failed");
        return false;
    }

    auto sync_view_ownership = [&](int bag, int /*slot*/, uint32_t handle) {
        if (handle == 0) return true;
        const bool visible = g_module_view_installed && g_module_view_index == bag;
        if (visible && ownership::live_state(g_ownership_ledger, handle,
                                             ownership::State::kModuleOwned)) {
            return ownership::borrow_for_view(&g_ownership_ledger, handle) == ownership::Outcome::kOk;
        }
        if (!visible && ownership::live_state(g_ownership_ledger, handle,
                                              ownership::State::kBorrowedForView)) {
            return ownership::return_from_view(&g_ownership_ledger, handle) == ownership::Outcome::kOk;
        }
        return true;
    };
    if (!sync_view_ownership(src_bag, src_slot, destination_handle_for_object) ||
        !sync_view_ownership(dst_bag, dst_slot, source_handle_for_object)) {
        restore(&txn, "swap view ownership update failed");
        return false;
    }
    if (g_module_view_installed) {
        if (g_module_view_index == src_bag) {
            void* control = valid_child_locked(g_projected_item_root, src_slot);
            if (control != nullptr) (void)extension_bag_ui_set_control_item(control, destination_object);
        }
        if (g_module_view_index == dst_bag) {
            void* control = valid_child_locked(g_projected_item_root, dst_slot);
            if (control != nullptr) (void)extension_bag_ui_set_control_item(control, source_object);
        }
    }
    g_virtual_bag_state.pending = {};
    extension_bag_mark_dirty_locked();
    reset_drag_state_locked(nullptr);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "txn committed id=%s extension swap %d/%d <-> %d/%d generation=%llu/%llu",
                txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot,
                static_cast<unsigned long long>(g_module_object_use[src_bag][src_slot].generation),
                static_cast<unsigned long long>(g_module_object_use[dst_bag][dst_slot].generation));
    return true;
}

bool sync_ext2ext_destination_view_locked(int bag, int slot, void* expected_item) {
    if (!g_module_view_installed || g_module_view_index != bag) return true;
    if (!virtual_bag::valid_index(bag) || slot < 0 ||
        slot >= g_virtual_bag_state.capacities[bag] || g_projected_item_root == nullptr ||
        g_module_objects[bag][slot] != expected_item) {
        return false;
    }
    const uint32_t handle = g_module_object_handles[bag][slot];
    if ((expected_item == nullptr) != (handle == 0)) return false;
    void* control = valid_child_locked(g_projected_item_root, slot);
    if (control == nullptr || !sync_projected_slot_locked(bag, slot)) return false;
    void* data = extension_bag_ui_data(control);
    if (data == nullptr || *reinterpret_cast<void**>(data) != expected_item) return false;

    if (expected_item != nullptr) {
        if (ownership::live_state(g_ownership_ledger, handle,
                                  ownership::State::kModuleOwned)) {
            if (ownership::borrow_for_view(&g_ownership_ledger, handle) !=
                ownership::Outcome::kOk) {
                return false;
            }
        } else if (!ownership::live_state(g_ownership_ledger, handle,
                                          ownership::State::kBorrowedForView)) {
            return false;
        }
        if (g_module_objects[bag][slot] != expected_item ||
            g_module_object_handles[bag][slot] != handle ||
            !ownership::live_state(g_ownership_ledger, handle,
                                   ownership::State::kBorrowedForView) ||
            *reinterpret_cast<void**>(data) != expected_item) {
            return false;
        }
    }
    return true;
}

bool restore_ext2ext_destination_locked(const virtual_bag::TransactionContext& txn) {
    const int bag = txn.dst_bag;
    const int slot = txn.dst_slot;
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const virtual_bag::Item& previous = g_virtual_bag_state.items[bag][slot];
    void* item = nullptr;
    if (previous.category > 0 && previous.count > 0) {
        item = module_item_locked(bag, slot);
        if (item == nullptr) return false;
    } else if (g_module_objects[bag][slot] != nullptr) {
        if (!free_module_object_locked(bag, slot)) return false;
    }
    return sync_ext2ext_destination_view_locked(bag, slot, item);
}

}
bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot) {
    if (!virtual_bag::valid_index(src_bag) || !virtual_bag::valid_index(dst_bag) ||
        g_virtual_bag_state.capacities[dst_bag] == 0 || src_slot < 0 ||
        src_slot >= virtual_bag::kSlotCount ||
        (requested_dst_slot >= 0 &&
         !virtual_bag::valid_ext2ext_transfer_slots(src_bag, src_slot, dst_bag,
                                                    requested_dst_slot))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension src=%d/%d dst_bag=%d requested_dst=%d",
                    src_bag, src_slot, dst_bag, requested_dst_slot);
        return false;
    }
    const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    const virtual_bag::PayloadValidation source_validation =
        virtual_bag::validate_serialized_payload_buffer(source.payload.data(), source.payload.size(),
                                                        source.payload_size);
    if (source_validation != virtual_bag::PayloadValidation::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension reason=%s src=%d/%d size=%u",
                    virtual_bag::payload_validation_reason(source_validation), src_bag, src_slot,
                    static_cast<unsigned int>(source.payload_size));
        return false;
    }

    const int capacity = g_virtual_bag_state.capacities[dst_bag];
    virtual_bag::Item committed{};
    int dst_slot = -1;
    bool merged = false;
    bool merge_mode_snapshot_loaded = false;
    bool merge_mode_snapshot_available = false;
    bool merge_extended_mode = false;
    auto try_merge = [&](int slot) {
        if (!virtual_bag::same_extension_bag_merge_allowed(src_bag, dst_bag) ||
            slot == src_slot) {
            return false;
        }
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
        if (!move_merge_enabled() ||
            !virtual_bag::same_extension_bag_mergeable_items(
                existing, source, virtual_bag_category_uses_stack_count)) {
            return false;
        }
        if (!merge_mode_snapshot_loaded) {
            merge_mode_snapshot_loaded = true;
            const auto* count_api = item_count_api();
            if (count_api == nullptr || count_api->read_extended_mode == nullptr) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "cross move reject extension->extension merge count mode unavailable src=%d/%d dst=%d/%d",
                             src_bag, src_slot, dst_bag, slot);
                return false;
            }
            uint8_t extended_value = 0;
            const QolResult mode_result = count_api->read_extended_mode(&extended_value);
            if (mode_result.code == kQolErrNotReady) {
                // The host reports NotReady while 999 mode has not been applied, so
                // merges use the same legacy view and 99 limit as an explicit off snapshot.
                merge_extended_mode = false;
                merge_mode_snapshot_available = true;
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "extension merge count mode not ready; using legacy view src=%d/%d dst=%d/%d",
                             src_bag, src_slot, dst_bag, slot);
            } else if (!qol::contracts::succeeded(mode_result) || extended_value > 1) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "cross move reject extension->extension merge count mode read failed src=%d/%d dst=%d/%d code=%d value=%u",
                             src_bag, src_slot, dst_bag, slot, mode_result.code,
                             static_cast<unsigned int>(extended_value));
                return false;
            } else {
                merge_extended_mode = extended_value != 0;
                merge_mode_snapshot_available = true;
            }
        }
        if (!merge_mode_snapshot_available) return false;
        // S2 合并判定在模式视图上进行（R-47 决策 b）：启用态用 descriptor
        // canonical（128a+b）；关闭态只按 b 视图相加（a 不参与运算），上限
        // 999/99。拆段写回经 patch_payload_count 的模式感知写：启用态全量进位
        // 内建，关闭态只写 b、保留 a；descriptor.count 恒回写 canonical
        // （重开后读回完整值）。
        const auto* count_api = item_count_api();
        if (count_api == nullptr || count_api->view_from_canonical == nullptr ||
            count_api->view_limit == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "cross move reject extension->extension merge count facade unavailable src=%d/%d dst=%d/%d",
                         src_bag, src_slot, dst_bag, slot);
            return false;
        }
        uint32_t existing_view = 0;
        uint32_t source_view = 0;
        uint32_t limit = 0;
        const QolResult existing_view_result = count_api->view_from_canonical(
            static_cast<uint32_t>(existing.count), merge_extended_mode ? 1 : 0,
            &existing_view);
        const QolResult source_view_result = count_api->view_from_canonical(
            static_cast<uint32_t>(source.count), merge_extended_mode ? 1 : 0,
            &source_view);
        const QolResult limit_result = count_api->view_limit(
            merge_extended_mode ? 1 : 0, &limit);
        if (!qol::contracts::succeeded(existing_view_result) ||
            !qol::contracts::succeeded(source_view_result) ||
            !qol::contracts::succeeded(limit_result)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "cross move reject extension->extension merge count snapshot failed src=%d/%d dst=%d/%d existing=%d source=%d limit=%d mode=%d",
                         src_bag, src_slot, dst_bag, slot, existing_view_result.code,
                         source_view_result.code, limit_result.code,
                         merge_extended_mode ? 1 : 0);
            return false;
        }
        const uint64_t total = static_cast<uint64_t>(existing_view) +
                               static_cast<uint64_t>(source_view);
        if (total > limit) return false;
        committed = existing;
        if (!virtual_bag::patch_payload_count(
                &committed,
                virtual_bag::merge_count(static_cast<int>(existing_view),
                                         static_cast<int>(source_view), merge_extended_mode),
                virtual_bag_category_uses_stack_count, merge_extended_mode)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension merge count gate src=%d/%d dst=%d/%d",
                        src_bag, src_slot, dst_bag, slot);
            return false;
        }
        committed.count = static_cast<int>(virtual_bag_canonical_from_field(
            virtual_bag::payload_count(committed), static_cast<uint32_t>(committed.count)));
        dst_slot = slot;
        merged = true;
        return true;
    };
    if (requested_dst_slot >= 0) {
        if (requested_dst_slot >= capacity) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension invalid destination=%d capacity=%d",
                        requested_dst_slot, capacity);
            return false;
        }
        if (!try_merge(requested_dst_slot)) {
            if (!module_slot_is_assignable_locked(dst_bag, requested_dst_slot)) {
                const virtual_bag::Item& destination =
                    g_virtual_bag_state.items[dst_bag][requested_dst_slot];
                if (destination.category > 0 && destination.count > 0 &&
                    (src_bag != dst_bag || src_slot != requested_dst_slot)) {
                    if (swap_extension_slots_locked(src_bag, src_slot, dst_bag,
                                                    requested_dst_slot)) {
                        return true;
                    }
                    QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension swap failed=%d/%d -> %d/%d",
                                src_bag, src_slot, dst_bag, requested_dst_slot);
                    return false;
                }
                QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension destination unavailable=%d/%d",
                            dst_bag, requested_dst_slot);
                return false;
            }
            committed = source;
            dst_slot = requested_dst_slot;
        }
    } else if (move_merge_enabled()) {
        for (int slot = 0; slot < capacity && dst_slot < 0; ++slot) {
            try_merge(slot);
        }
    }
    if (dst_slot < 0) {
        for (int slot = 0; slot < capacity; ++slot) {
            if (!module_slot_is_assignable_locked(dst_bag, slot)) continue;
            committed = source;
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross move reject extension->extension destination full=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }

    // prepared：纯逻辑事务快照。ext→ext 恒无 original 阶段。
    virtual_bag::TransactionContext txn{};
    txn.direction = virtual_bag::kTransferExtensionToExtension;
    txn.original_stage_applicable = false;
    txn.src_bag = static_cast<uint8_t>(src_bag);
    txn.src_slot = static_cast<uint8_t>(src_slot);
    txn.dst_bag = static_cast<uint8_t>(dst_bag);
    txn.dst_slot = static_cast<uint8_t>(dst_slot);
    txn.merged = merged;
    txn.source = source;
    txn.committed = committed;
    txn.previous_dst = g_virtual_bag_state.items[dst_bag][dst_slot];
    generate_transaction_id_locked(txn.transaction_id);
    if (txn.merged) {
        QOL_LOG_INFO(QolDomain::kExtensionBag, "txn prepared id=%s extension merge %d/%d -> %d/%d count=%d+%d=%d",
                    txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, source.count,
                    txn.previous_dst.count, txn.committed.count);
    }
    if (!module_object_release_allowed_locked(
            src_bag, src_slot, g_module_objects[src_bag][src_slot],
            "extension-merge source", false, nullptr)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "txn reject id=%s extension merge source object in use",
                    txn.transaction_id);
        return false;
    }

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：仅更新扩展逻辑 source/target（不调用原版库存移动，不建 journal）。
    virtual_bag::txn_apply_logical(&g_virtual_bag_state, txn);
    extension_bag_mark_dirty_locked();

    // committed
    g_virtual_bag_state.pending = {};
    // 事务只更新 descriptor；提交前的目标控件仍绑定旧对象（或 nullptr）。
    // 先按新 descriptor 物化目标对象，后续 projection refresh 才能把新物品
    // 立即绑定到目标槽，而不是等切换标签重新 install 视图时才显示。
    void* destination_item = module_item_locked(dst_bag, dst_slot);
    if (destination_item == nullptr) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "txn committed but destination materialize failed id=%s dst=%d/%d",
                    txn.transaction_id, dst_bag, dst_slot);
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "destination materialize failed");
        if (!restore_ext2ext_destination_locked(txn)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR ext2ext destination view rollback failed id=%s dst=%d/%d",
                          txn.transaction_id, dst_bag, dst_slot);
        }
        return false;
    } else if (g_module_view_installed && g_module_view_index == dst_bag) {
        if (!sync_ext2ext_destination_view_locked(dst_bag, dst_slot, destination_item)) {
            // 投影槽与目标 handle/borrow 必须同时确认，否则事务回到逻辑更新前且保留源。
            QOL_LOG_WARN(QolDomain::kExtensionBag, "txn destination view borrow failed id=%s dst=%d/%d",
                         txn.transaction_id, dst_bag, dst_slot);
            txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                             "destination view borrow or identity failed");
            if (!restore_ext2ext_destination_locked(txn)) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR ext2ext destination view rollback failed id=%s dst=%d/%d",
                              txn.transaction_id, dst_bag, dst_slot);
            }
            return false;
        }
    }
    if (!free_module_object_locked(src_bag, src_slot)) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "extension merge source release rejected");
        if (!restore_ext2ext_destination_locked(txn)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR ext2ext destination view rollback failed id=%s dst=%d/%d",
                          txn.transaction_id, dst_bag, dst_slot);
        }
        QOL_LOG_WARN(QolDomain::kExtensionBag, "txn abort id=%s extension merge source release rejected",
                    txn.transaction_id);
        return false;
    }
    reset_drag_state_locked(nullptr);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "txn committed id=%s extension->extension %d/%d -> %d/%d cat=%d count=%d",
                txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, txn.source.category,
                txn.committed.count);
    return true;
}
