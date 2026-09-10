bool move_original_to_extension_slot_locked(int src_bag, int src_slot, int dst_bag) {
    if (!virtual_bag::valid_original_transaction_bag(src_bag)) {
        VIRTBAG_LOG("cross move reject original->extension invalid source bag=%d", src_bag);
        return false;
    }
    if (src_slot < 0 || src_slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("cross move reject original->extension invalid source slot=%d bag=%d",
                    src_slot, src_bag);
        return false;
    }
    if (!virtual_bag::valid_index(dst_bag) || g_virtual_bag_state.capacities[dst_bag] == 0 ||
        fn_save_save_item == nullptr || fn_get_cumulate_count == nullptr ||
        fn_remove_item_direct == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension dst=%d capacity=%d symbols=%d",
                    dst_bag,
                    virtual_bag::valid_index(dst_bag) ? g_virtual_bag_state.capacities[dst_bag] : 0,
                    fn_save_save_item != nullptr && fn_get_cumulate_count != nullptr &&
                    fn_remove_item_direct != nullptr ? 1 : 0);
        return false;
    }
    void* src_item = nullptr;
    if (!inventory_slot_locked(src_bag, src_slot, &src_item) || src_item == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension source empty bag=%d slot=%d",
                    src_bag, src_slot);
        return false;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> source_payload{};
    int serialized_size = 0;
    if (!serialize_item_payload_locked(src_item, &source_payload, &serialized_size)) {
        VIRTBAG_LOG("cross move: item serialization rejected");
        return false;
    }
    const int src_count = fn_get_cumulate_count(src_item);
    const int src_category = original_item_category_locked(src_item);
    const int capacity = g_virtual_bag_state.capacities[dst_bag];

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
        VIRTBAG_LOG("cross move reject original->extension destination full bag=%d capacity=%d",
                    dst_bag, capacity);
        return false;
    }
    txn.dst_slot = static_cast<uint8_t>(dst_slot);
    txn.merged = merged;
    txn.committed = committed;
    txn.previous_dst = g_virtual_bag_state.items[dst_bag][dst_slot];
    generate_transaction_id_locked(txn.transaction_id);
    if (txn.merged) {
        VIRTBAG_LOG("txn prepared id=%s original merge src=%d/%d -> extension=%d/%d count=%d+%d=%d",
                    txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, src_count,
                    txn.previous_dst.count, txn.committed.count);
    }

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：写扩展逻辑目标；物化失败即回滚（源保持原版实态）。
    virtual_bag::txn_apply_logical(&g_virtual_bag_state, txn);
    g_item_state_dirty = true;
    if (module_item_locked(dst_bag, dst_slot) == nullptr) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "target extension item could not be materialized; source retained");
        return false;
    }
    if (!module_object_release_allowed_locked(
            dst_bag, dst_slot, g_module_objects[dst_bag][dst_slot],
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
    fn_remove_item_direct(src_bag, src_slot);
    void* remaining_source = nullptr;
    if (inventory_slot_locked(src_bag, src_slot, &remaining_source) && remaining_source != nullptr) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "original source slot remained occupied");
        return false;
    }

    // committed：不做猜测性逆转。清 pending、回收模块投影对象、刷新原版网格。
    g_virtual_bag_state.pending = {};
    // 用户决策（2026-08-31）：orig→ext 后保持当前视图（对齐原版"移动后停留"）。
    // 不切 mode、不装投影：mode 与投影始终成对，此前"切 mode 不装投影"的
    // overlay 错位从根上不可达；P2 移动锁也不再因 API 移动触发。
    // 当前视图只同步发生变化的扩展槽；非扩展视图仍走原版刷新。
    if (g_module_view_installed && g_module_view_index == dst_bag) {
        sync_projected_slot_locked(dst_bag, dst_slot);
    } else if (fn_ui_equip_refresh_item_area != nullptr) {
        inventory_native_hook_call_refresh_item_area_original();
    }
    VIRTBAG_LOG("txn committed id=%s original->extension bag=%d slot=%d cat=%d count=%d src=%d/%d",
                txn.transaction_id, dst_bag, dst_slot, src_category, txn.committed.count,
                src_bag, src_slot);
    return true;
}

bool move_original_to_extension_locked(int dst_bag, void* moving_control) {
    if (!virtual_bag::valid_index(dst_bag) || g_virtual_bag_state.capacities[dst_bag] == 0 ||
        fn_ui_equip_get_item_slot_index == nullptr) {
        VIRTBAG_LOG("cross move reject original->extension dst=%d control=%p capacity=%d symbol=%d",
                    dst_bag, moving_control,
                    virtual_bag::valid_index(dst_bag) ? g_virtual_bag_state.capacities[dst_bag] : 0,
                    fn_ui_equip_get_item_slot_index != nullptr ? 1 : 0);
        return false;
    }
    const int src_bag = original_bag_locked();
    const int src_slot = fn_ui_equip_get_item_slot_index(moving_control);
    const bool moved = move_original_to_extension_slot_locked(src_bag, src_slot, dst_bag);
    if (moved) reset_drag_state_locked(moving_control);
    return moved;
}

bool move_extension_to_original_locked(int src_bag, int src_slot, int target_bag) {
    if (!virtual_bag::valid_original_transaction_bag(target_bag) ||
        !virtual_bag::valid_index(src_bag) ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount ||
        fn_inven_save_item_on_empty == nullptr || fn_remove_item_direct == nullptr) {
        VIRTBAG_LOG("cross move reject extension->original src=%d/%d target_bag=%d", src_bag,
                    src_slot, target_bag);
        return false;
    }
    if (fn_get_bag_size != nullptr && fn_get_bag_size(target_bag) <= 0) {
        VIRTBAG_LOG("cross move reject extension->original target bag unavailable=%d", target_bag);
        return false;
    }
    // 满包预检：INVEN_SaveItemOnEmpty 是纯首空槽扫描（0x104be0，满则返回 0 无副作用），
    // 事务入口先行拒绝，避免为注定失败的插入创建 pending/隔离记录或切换当前袋。
    if (fn_get_bag_size != nullptr) {
        const int bag_size = fn_get_bag_size(target_bag);
        bool has_empty = false;
        for (int slot = 0; slot < bag_size && slot < virtual_bag::kSlotCount; ++slot) {
            void* existing = nullptr;
            if (!inventory_slot_locked(target_bag, slot, &existing) || existing == nullptr) {
                has_empty = true;
                break;
            }
        }
        if (!has_empty) {
            VIRTBAG_LOG("cross move reject extension->original target full=%d size=%d",
                        target_bag, bag_size);
            return false;
        }
    }
    const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        VIRTBAG_LOG("cross move reject extension->original source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    const virtual_bag::PayloadValidation source_validation =
        virtual_bag::validate_serialized_payload_buffer(source.payload.data(), source.payload.size(),
                                                        source.payload_size);
    if (source_validation != virtual_bag::PayloadValidation::kOk) {
        VIRTBAG_LOG("cross move reject extension->original reason=%s src=%d/%d size=%u",
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
    if (item != nullptr) set_original_bag_locked(target_bag);
    if (item == nullptr || !fn_inven_save_item_on_empty(item, target_bag)) {
        // P4.5：Load 失败或插入失败隔离。扩展源逻辑物品保留（payload 字节未丢），
        // 事务上下文留作只读诊断；不构造可移动替代项。
        isolate_ext2orig_failure_locked(txn,
                                        item == nullptr ? virtual_bag::TxnStage::kLogicalUpdated
                                                        : virtual_bag::TxnStage::kOriginalUpdated,
                                        item == nullptr ? virtual_bag::isolation_reason::kLoadFailed
                                                        : virtual_bag::isolation_reason::kInsertFailed,
                                        item == nullptr ? "move_extension_to_original load"
                                                        : "move_extension_to_original insert");
        txn_abort_locked(txn,
                         item != nullptr ? virtual_bag::TxnStage::kOriginalUpdated
                                         : virtual_bag::TxnStage::kLogicalUpdated,
                         "target insertion failed; extension source retained");
        release_tracked_item_locked(item_handle, item, "ext2orig insert failed");
        if (g_module_view_installed && virtual_bag::valid_index(g_module_window_original_bag)) {
            set_original_bag_locked(g_module_window_original_bag);
            refresh_module_item_area_locked(g_module_view_index);
        }
        reset_drag_state_locked(nullptr);
        return false;
    }

    // original-state-updated：确认入库落位并以原版实态复核（指针身份扫描优先）。
    int inserted_slot = -1;
    void* inserted_item = nullptr;
    for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
        if (inventory_slot_locked(target_bag, slot, &inserted_item) && inserted_item == item) {
            inserted_slot = slot;
            break;
        }
    }
    if (inserted_slot < 0) {
        inserted_slot = original_inventory_payload_slot_locked(
            source.payload.data(), source.payload_size, target_bag);
    }
    if (inserted_slot < 0) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kSlotNotFound,
                                        "ext2orig insertion verify");
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "insertion slot not found; extension source retained");
        release_tracked_item_locked(item_handle, item, "ext2orig slot-not-found rollback");
        if (g_module_view_installed && virtual_bag::valid_index(g_module_window_original_bag)) {
            set_original_bag_locked(g_module_window_original_bag);
            refresh_module_item_area_locked(g_module_view_index);
        }
        reset_drag_state_locked(nullptr);
        return false;
    }
    if (!module_object_release_allowed_locked(
            src_bag, src_slot, g_module_objects[src_bag][src_slot],
            "extension-to-original commit", false, nullptr)) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kInsertFailed,
                                        "source extension object is in use");
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "source extension object release rejected");
        fn_remove_item_direct(target_bag, inserted_slot);
        release_tracked_item_locked(item_handle, item, "ext2orig release rejected rollback");
        reset_drag_state_locked(nullptr);
        return false;
    }
    // P4.3：入库已确认 → 原版库存接管终态（此后模块不得触碰 item 指针）。
    if (!free_module_object_locked(src_bag, src_slot)) {
        isolate_ext2orig_failure_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                                        virtual_bag::isolation_reason::kInsertFailed,
                                        "source extension object release rejected");
        txn_abort_locked(txn, virtual_bag::TxnStage::kOriginalUpdated,
                         "source extension object release rejected");
        fn_remove_item_direct(target_bag, inserted_slot);
        release_tracked_item_locked(item_handle, item, "ext2orig release rejected rollback");
        reset_drag_state_locked(nullptr);
        return false;
    }
    handover_tracked_item_locked(item_handle, item, "ext2orig handover");

    // committed：清扩展源逻辑项、恢复投影、清 pending。已移交对象不参与模块回滚。
    g_virtual_bag_state.items[src_bag][src_slot] = {};
    g_item_state_dirty = true;
    g_virtual_bag_state.pending = {};
    // 保持当前视图：仅拖放上下文（扩展视图安装中 = 当前视图即扩展袋）停留该视图；
    // API 路径（原版视图）不改写视图字段（用户 2026-08-31 决策）。
    if (g_module_view_installed) {
        g_virtual_bag_state.mode = virtual_bag::Mode::kModule;
        g_virtual_bag_state.selected = src_bag;
        g_virtual_bag_state.inspected = -1;
    }
    set_original_bag_locked(target_bag);
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("txn committed id=%s extension->original src=%d/%d cat=%d count=%d target_bag=%d slot=%d",
                txn.transaction_id, src_bag, src_slot, txn.source.category, txn.source.count,
                target_bag, inserted_slot);
    return true;
}

bool route_deferred_extension_to_original_locked() {
    virtual_bag::ExtensionDragSession& session = g_extension_drag_session;
    if (session.target_kind != virtual_bag::DragTargetKind::kOriginalBag ||
        !virtual_bag::valid_original_transaction_bag(session.target_bag) ||
        !virtual_bag::drag_session_generation_current(session, g_extension_tab_generation) ||
        !virtual_bag::valid_index(session.source_bag) ||
        session.source_slot < 0 || session.source_slot >= virtual_bag::kSlotCount) {
        return false;
    }
    if (session.phase == virtual_bag::DragPhase::kPressed) {
        virtual_bag::session_on_native_moving(&session, g_extension_tab_generation);
    }
    if (virtual_bag::session_resolve_target(&session, g_extension_tab_generation,
                                            virtual_bag::DragTargetKind::kOriginalBag) ==
            virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_begin_transaction(&session, g_extension_tab_generation) ==
            virtual_bag::DragTransition::kIllegal ||
        !virtual_bag::session_claim_transaction(&session)) {
        VIRTBAG_LOG("drop deferred ext->orig rejected source=%d/%d target_bag=%d phase=%d",
                    session.source_bag, session.source_slot, session.target_bag,
                    static_cast<int>(session.phase));
        return false;
    }
    const int src_bag = session.source_bag;
    const int src_slot = session.source_slot;
    const int target_bag = session.target_bag;
    const bool moved = move_extension_to_original_locked(src_bag, src_slot, target_bag);
    virtual_bag::session_finish_transaction(&session, moved);
    if (moved) {
        persist_state_locked();
        refresh_projected_module_view_after_move_locked();
        VIRTBAG_LOG("p5 session drop committed source=%d/%d target=%d/%d kind=%d deferred=1",
                    src_bag, src_slot, target_bag, -1,
                    static_cast<int>(session.target_kind));
    } else {
        VIRTBAG_LOG("p5 session drop rejected source=%d/%d target=%d/%d kind=%d deferred=1",
                    src_bag, src_slot, target_bag, -1,
                    static_cast<int>(session.target_kind));
    }
    return moved;
}

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
        VIRTBAG_LOG("txn reject extension swap token active src=%d/%d active=%d pending=%d "
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
        VIRTBAG_LOG("txn reject extension swap stale endpoint src=%d/%d dst=%d/%d",
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
        if (g_module_view_installed && fn_control_item_set_item != nullptr) {
            if (g_module_view_index == src_bag) {
                void* control = valid_child_locked(g_projected_item_root, src_slot);
                if (control != nullptr) fn_control_item_set_item(control, previous_source_object);
            }
            if (g_module_view_index == dst_bag) {
                void* control = valid_child_locked(g_projected_item_root, dst_slot);
                if (control != nullptr) fn_control_item_set_item(control, previous_destination_object);
            }
        }
    };

    if (source_object == nullptr || destination_object == nullptr) {
        restore(nullptr, "swap endpoint materialization failed");
        VIRTBAG_LOG("txn reject extension swap endpoint materialization failed src=%d/%d dst=%d/%d",
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

    auto sync_view_ownership = [&](int bag, int slot, uint32_t handle) {
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
    if (g_module_view_installed && fn_control_item_set_item != nullptr) {
        if (g_module_view_index == src_bag) {
            void* control = valid_child_locked(g_projected_item_root, src_slot);
            if (control != nullptr) fn_control_item_set_item(control, destination_object);
        }
        if (g_module_view_index == dst_bag) {
            void* control = valid_child_locked(g_projected_item_root, dst_slot);
            if (control != nullptr) fn_control_item_set_item(control, source_object);
        }
    }
    g_virtual_bag_state.pending = {};
    g_item_state_dirty = true;
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("txn committed id=%s extension swap %d/%d <-> %d/%d generation=%llu/%llu",
                txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot,
                static_cast<unsigned long long>(g_module_object_use[src_bag][src_slot].generation),
                static_cast<unsigned long long>(g_module_object_use[dst_bag][dst_slot].generation));
    return true;
}

bool move_extension_to_extension_locked(int src_bag, int src_slot, int dst_bag,
                                        int requested_dst_slot) {
    if (!virtual_bag::valid_index(src_bag) || !virtual_bag::valid_index(dst_bag) ||
        g_virtual_bag_state.capacities[dst_bag] == 0 || src_slot < 0 ||
        src_slot >= virtual_bag::kSlotCount ||
        (requested_dst_slot >= 0 &&
         !virtual_bag::valid_ext2ext_transfer_slots(src_bag, src_slot, dst_bag,
                                                    requested_dst_slot))) {
        VIRTBAG_LOG("cross move reject extension->extension src=%d/%d dst_bag=%d requested_dst=%d",
                    src_bag, src_slot, dst_bag, requested_dst_slot);
        return false;
    }
    const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
    if (source.category <= 0 || source.count <= 0) {
        VIRTBAG_LOG("cross move reject extension->extension source empty=%d/%d", src_bag,
                    src_slot);
        return false;
    }
    const virtual_bag::PayloadValidation source_validation =
        virtual_bag::validate_serialized_payload_buffer(source.payload.data(), source.payload.size(),
                                                        source.payload_size);
    if (source_validation != virtual_bag::PayloadValidation::kOk) {
        VIRTBAG_LOG("cross move reject extension->extension reason=%s src=%d/%d size=%u",
                    virtual_bag::payload_validation_reason(source_validation), src_bag, src_slot,
                    static_cast<unsigned int>(source.payload_size));
        return false;
    }

    const int capacity = g_virtual_bag_state.capacities[dst_bag];
    virtual_bag::Item committed{};
    int dst_slot = -1;
    bool merged = false;
    auto try_merge = [&](int slot) {
        if (!virtual_bag::same_extension_bag_merge_allowed(src_bag, dst_bag) ||
            slot == src_slot) {
            return false;
        }
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
        if (!move_merge_enabled() ||
            !virtual_bag::same_extension_bag_mergeable_items(existing, source)) {
            return false;
        }
        const uint32_t limit = stack_codec::max_count(stack_limit_enabled());
        const uint64_t total = static_cast<uint64_t>(existing.count) +
                               static_cast<uint64_t>(source.count);
        if (total > limit) return false;
        committed = existing;
        committed.count = static_cast<int>(virtual_bag::merge_count(
            existing.count, source.count, stack_limit_enabled()));
        virtual_bag::patch_payload_count(&committed, static_cast<uint32_t>(committed.count));
        dst_slot = slot;
        merged = true;
        return true;
    };
    if (requested_dst_slot >= 0) {
        if (requested_dst_slot >= capacity) {
            VIRTBAG_LOG("cross move reject extension->extension invalid destination=%d capacity=%d",
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
                    VIRTBAG_LOG("cross move reject extension->extension swap failed=%d/%d -> %d/%d",
                                src_bag, src_slot, dst_bag, requested_dst_slot);
                    return false;
                }
                VIRTBAG_LOG("cross move reject extension->extension destination unavailable=%d/%d",
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
        VIRTBAG_LOG("cross move reject extension->extension destination full=%d capacity=%d",
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
        VIRTBAG_LOG("txn prepared id=%s extension merge %d/%d -> %d/%d count=%d+%d=%d",
                    txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, source.count,
                    txn.previous_dst.count, txn.committed.count);
    }
    if (!module_object_release_allowed_locked(
            src_bag, src_slot, g_module_objects[src_bag][src_slot],
            "extension-merge source", false, nullptr)) {
        VIRTBAG_LOG("txn reject id=%s extension merge source object in use",
                    txn.transaction_id);
        return false;
    }

    // pending-recorded
    if (!txn_record_pending_locked(&txn)) return false;

    // logical-state-updated：仅更新扩展逻辑 source/target（不调用原版库存移动，不建 journal）。
    virtual_bag::txn_apply_logical(&g_virtual_bag_state, txn);
    g_item_state_dirty = true;

    // committed
    g_virtual_bag_state.pending = {};
    // 事务只更新 descriptor；提交前的目标控件仍绑定旧对象（或 nullptr）。
    // 先按新 descriptor 物化目标对象，后续 projection refresh 才能把新物品
    // 立即绑定到目标槽，而不是等切换标签重新 install 视图时才显示。
    void* destination_item = module_item_locked(dst_bag, dst_slot);
    if (destination_item == nullptr) {
        VIRTBAG_LOG("txn committed but destination materialize failed id=%s dst=%d/%d",
                    txn.transaction_id, dst_bag, dst_slot);
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "destination materialize failed");
        return false;
    } else if (g_module_view_installed && g_module_view_index == dst_bag) {
        const uint32_t handle = g_module_object_handles[dst_bag][dst_slot];
        if (ownership::live_state(g_ownership_ledger, handle,
                                  ownership::State::kModuleOwned)) {
            if (ownership::borrow_for_view(&g_ownership_ledger, handle) !=
                ownership::Outcome::kOk) {
                VIRTBAG_LOG("txn destination view borrow failed id=%s dst=%d/%d",
                            txn.transaction_id, dst_bag, dst_slot);
            }
        }
    }
    if (!free_module_object_locked(src_bag, src_slot)) {
        txn_abort_locked(txn, virtual_bag::TxnStage::kLogicalUpdated,
                         "extension merge source release rejected");
        VIRTBAG_LOG("txn abort id=%s extension merge source release rejected",
                    txn.transaction_id);
        return false;
    }
    reset_drag_state_locked(nullptr);
    VIRTBAG_LOG("txn committed id=%s extension->extension %d/%d -> %d/%d cat=%d count=%d",
                txn.transaction_id, src_bag, src_slot, dst_bag, dst_slot, txn.source.category,
                txn.committed.count);
    return true;
}

bool handle_bag_drop_release_locked(int64_t x, int64_t y) {
    ensure_state_loaded_locked();
    recover_pending_transaction_locked();
    VIRTBAG_LOG("drop release mode=%d selected=%d x=%lld y=%lld moving=%p extension_drag=%d",
                static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
                static_cast<long long>(x), static_cast<long long>(y),
                touch_moving_item_control_locked(), g_extension_drag.active ? 1 : 0);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        void* moving_control = touch_moving_item_control_locked();
        if (moving_control == nullptr) {
            VIRTBAG_LOG("drop reject original->extension no moving original control");
            return false;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (!extension_tab_hit(index, x, y)) continue;
            void* item = nullptr;
            if (fn_control_object_get_data != nullptr) {
                void* data = fn_control_object_get_data(moving_control);
                item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
            }
            const int category = original_item_category_locked(item);
            if (g_virtual_bag_state.types[index] == 0 && is_backpack_category(category)) {
                // 原版背包来源的袋物品拖到空扩展标签也必须走“袋槽装备”，
                // 不能直接落入普通扩展物品槽。
                return try_equip_on_extension_tab_drop_locked(index, moving_control);
            }
            // 目标非空，或来源为装备/其他物品：保持原版物品的普通移动语义。
            return move_original_to_extension_locked(index, moving_control);
        }
        VIRTBAG_LOG("drop reject original->extension no extension tab hit x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
        return false;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule && g_extension_drag.active) {
    const int target_slot = extension_bag_grid_slot_index(x, y, kGridX, kGridY);
            if (target_slot >= 0) {
            const bool handled = move_extension_to_extension_locked(
                g_extension_drag.bag, g_extension_drag.slot,
                g_virtual_bag_state.selected, target_slot);
            if (handled) refresh_projected_module_view_after_move_locked();
            VIRTBAG_LOG("drop result direction=extension->extension handled=%d src=%d/%d dst=%d/%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot,
                        g_virtual_bag_state.selected, target_slot);
            return handled;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (index == g_extension_drag.bag) continue;
            if (!extension_tab_hit(index, x, y)) continue;
            const int category = g_virtual_bag_state.items[g_extension_drag.bag][g_extension_drag.slot].category;
            const bool should_equip = g_virtual_bag_state.types[index] == 0 &&
                                      is_backpack_category(category);
            const bool handled = should_equip
                ? try_equip_on_extension_tab_drop_locked(index, touch_moving_item_control_locked())
                : move_extension_to_extension_locked(
                    g_extension_drag.bag, g_extension_drag.slot, index, -1);
            if (handled) refresh_projected_module_view_after_move_locked();
            VIRTBAG_LOG("drop result direction=extension->tab handled=%d equip=%d src=%d/%d dst_bag=%d",
                        handled ? 1 : 0, should_equip ? 1 : 0, g_extension_drag.bag,
                        g_extension_drag.slot, index);
            return handled;
        }
        const int target_bag = original_bag_button_index(x, y);
        if (target_bag >= 0 && target_bag < 6) {
            if (!virtual_bag::valid_original_transaction_bag(target_bag)) {
                // 索引 5 = 原版任务袋（ADR-006）：不得成为扩展移动目标（触摸路径与 API 门禁对齐）。
                VIRTBAG_LOG("drop reject extension->original reason=invalid_transaction_domain target_bag=%d",
                            target_bag);
                return false;
            }
            const bool handled = move_extension_to_original_locked(
                g_extension_drag.bag, g_extension_drag.slot, target_bag);
            if (handled) refresh_projected_module_view_after_move_locked();
            VIRTBAG_LOG("drop result direction=extension->original handled=%d src=%d/%d target_bag=%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot, target_bag);
            return handled;
        }
        VIRTBAG_LOG("drop reject extension destination not found x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
    } else if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        VIRTBAG_LOG("drop reject extension no active drag selected=%d",
                    g_virtual_bag_state.selected);
    }
    return false;
}

bool route_projected_session_drop_locked(int64_t x, int64_t y) {
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kIdle ||
        g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        return false;
    }
    // Tab rects and grid rows overlap in the current UI coordinate space; a tab hit is
    // therefore the explicit switch-bag intent and must take precedence over a slot hit.
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const int target_bag = index;
        const int target_slot = -1;
        if (target_bag == g_extension_drag_session.source_bag ||
            !extension_tab_hit(target_bag, x, y)) {
            continue;
        }
        const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
            target_bag, target_slot, true);
        if (target_kind == virtual_bag::DragTargetKind::kExtensionTab) {
            return route_projected_session_to_tab_locked(
                virtual_bag::extension_logical_bag(target_bag));
        }
    }
    int target_bag = g_virtual_bag_state.selected;
    int target_slot = extension_bag_grid_slot_index(x, y, kGridX, kGridY);
    virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
        virtual_bag::kExtensionLogicalBagFirst + target_bag, target_slot);
    if (target_slot < 0 || target_slot >= g_virtual_bag_state.capacities[target_bag]) {
        target_bag = -1;
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (index == g_extension_drag_session.source_bag) continue;
            if (!extension_tab_hit(index, x, y)) continue;
            target_bag = index;
            break;
        }
        if (target_bag < 0) {
            VIRTBAG_LOG("p5 session target miss x=%lld y=%lld source=%d/%d",
                        static_cast<long long>(x), static_cast<long long>(y),
                        g_extension_drag_session.source_bag,
                        g_extension_drag_session.source_slot);
            return false;
        }
        target_slot = -1;
        target_kind = virtual_bag::classify_drag_target(target_bag, target_slot, true);
    }
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kPressed) {
        advance_projected_drag_session_locked();
    }
    if (virtual_bag::session_resolve_target(&g_extension_drag_session,
                                            g_extension_tab_generation,
                                            target_kind) ==
            virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_begin_transaction(&g_extension_drag_session,
                                               g_extension_tab_generation) ==
             virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_on_release(&g_extension_drag_session,
                                        g_extension_tab_generation) !=
            virtual_bag::DragTransition::kAdvanced ||
        !virtual_bag::session_claim_transaction(&g_extension_drag_session)) {
        return false;
    }
    const bool should_equip = target_kind == virtual_bag::DragTargetKind::kExtensionTab &&
                              extension_source_should_equip_locked(target_bag);
    bool handled = false;
    if (should_equip) {
        handled = equip_extension_source_on_tab_locked(
            target_bag, g_extension_drag_session.source_bag,
            g_extension_drag_session.source_slot);
    } else {
        handled = move_extension_to_extension_locked(
            g_extension_drag_session.source_bag, g_extension_drag_session.source_slot,
            target_bag, target_slot);
        if (handled) {
            persist_state_locked();
            refresh_projected_module_view_after_move_locked();
        }
    }
    virtual_bag::session_finish_transaction(&g_extension_drag_session, handled);
    VIRTBAG_LOG("p5 session drop %s source=%d/%d target=%d/%d kind=%d equip=%d",
                handled ? "committed" : "rejected",
                g_extension_drag_session.source_bag, g_extension_drag_session.source_slot,
                target_bag, target_slot, static_cast<int>(target_kind), should_equip ? 1 : 0);
    return handled;
}

bool route_projected_session_to_tab_locked(int target_bag) {
    // 入口 target_bag 使用扩展逻辑袋号 6..10；事务状态和 State 数组统一使用
    // 内部索引 0..4。成功装备由 finish_extension_equip_ui_locked
    // 复用 handle_extension_tab_click_locked/install_module_view_locked 同步当前视图，
    // 不能只结束 session 后继续沿用源袋的 g_module_view_index。
    auto& session = g_extension_drag_session;
    const int target_internal = virtual_bag::extension_internal_bag(target_bag);
    const auto reject = [&](const char* reason) {
        VIRTBAG_LOG("cross tab reject reason=%s bag=%d slot=%d target_bag=%d target_internal=%d "
                    "phase=%d generation=%llu/%llu/%llu",
                    reason, session.source_bag, session.source_slot, target_bag, target_internal,
                    static_cast<int>(session.phase),
                    static_cast<unsigned long long>(session.view_generation),
                    static_cast<unsigned long long>(g_extension_tab_generation),
                    static_cast<unsigned long long>(g_inventory_generation));
        return false;
    };
    if (session.phase == virtual_bag::DragPhase::kIdle) {
        return reject("session_idle");
    }
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return reject("mode_not_module");
    }
    if (target_internal < 0) {
        return reject("target_bag_invalid");
    }
    if (target_internal == session.source_bag) {
        return reject("same_source_target");
    }
    if (!virtual_bag::drag_session_generation_current(session, g_extension_tab_generation)) {
        return reject("session_generation_mismatch");
    }
    if (g_extension_tab_generation != g_inventory_generation) {
        return reject("tab_generation_mismatch");
    }
    if (!virtual_bag::valid_index(session.source_bag) || session.source_slot < 0 ||
        session.source_slot >= virtual_bag::kSlotCount) {
        return reject("source_session_invalid");
    }
    if (session.phase == virtual_bag::DragPhase::kPressed) {
        advance_projected_drag_session_locked();
    }
    const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
        target_bag, -1, true);
    if (target_kind != virtual_bag::DragTargetKind::kExtensionTab) {
        return reject("target_kind_invalid");
    }
    if (virtual_bag::session_resolve_target(
            &session, g_extension_tab_generation, target_kind) ==
        virtual_bag::DragTransition::kIllegal) {
        return reject("target_transition_illegal");
    }
    if (session.target_kind != target_kind) {
        return reject("session_target_kind_mismatch");
    }
    if (virtual_bag::session_begin_transaction(&session, g_extension_tab_generation) ==
        virtual_bag::DragTransition::kIllegal) {
        return reject("transaction_begin_illegal");
    }
    if (virtual_bag::session_on_release(&session, g_extension_tab_generation) !=
        virtual_bag::DragTransition::kAdvanced) {
        return reject("release_transition_illegal");
    }
    if (!virtual_bag::session_claim_transaction(&session)) {
        return reject("transaction_claim_failed");
    }
    const bool should_equip = extension_source_should_equip_locked(target_internal);
    bool handled = false;
    if (should_equip) {
        handled = equip_extension_source_on_tab_locked(
            target_internal, session.source_bag, session.source_slot);
    } else {
        handled = move_extension_to_extension_locked(
            session.source_bag, session.source_slot, target_internal, -1);
        if (handled) {
            persist_state_locked();
            refresh_projected_module_view_after_move_locked();
        }
    }
    virtual_bag::session_finish_transaction(&session, handled);
    if (handled) {
        VIRTBAG_LOG("tab commit handled=1 bag=%d slot=%d target_bag=%d target_kind=%d equip=%d",
                    session.source_bag, session.source_slot, target_bag,
                    static_cast<int>(target_kind), should_equip ? 1 : 0);
    } else {
        VIRTBAG_LOG("tab reject reason=transaction_failed bag=%d slot=%d target_bag=%d "
                    "target_kind=%d equip=%d",
                    session.source_bag, session.source_slot, target_bag,
                    static_cast<int>(target_kind), should_equip ? 1 : 0);
    }
    return handled;
}
