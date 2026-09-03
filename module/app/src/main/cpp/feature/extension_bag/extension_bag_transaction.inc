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
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            if (existing.category != 0 || existing.count != 0) continue;
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
    free_module_object_locked(dst_bag, dst_slot);
    // 用户决策（2026-08-31）：orig→ext 后保持当前视图（对齐原版"移动后停留"）。
    // 不切 mode、不装投影：mode 与投影始终成对，此前"切 mode 不装投影"的
    // overlay 错位从根上不可达；P2 移动锁也不再因 API 移动触发。
    // 原版网格残影由 RefreshItemArea 清除（INVEN 全程真实，线程先例：install 路径）。
    if (g_module_view_installed) {
        // 扩展视图中原版刷新会先把当前原版袋绘制一帧，随后投影覆盖，
        // 从而出现装备后短暂闪回原版背包。当前视图容量未变化，只需修复
        // 已清空的源控件和已更新的扩展标签对象。
        refresh_projection_if_overwritten_locked();
    } else if (fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();
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
            refresh_projection_if_overwritten_locked();
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
            refresh_projection_if_overwritten_locked();
        }
        reset_drag_state_locked(nullptr);
        return false;
    }
    // P4.3：入库已确认 → 原版库存接管终态（此后模块不得触碰 item 指针）。
    handover_tracked_item_locked(item_handle, item, "ext2orig handover");

    // committed：清扩展源逻辑项、恢复投影、清 pending。已移交对象不参与模块回滚。
    g_virtual_bag_state.items[src_bag][src_slot] = {};
    g_item_state_dirty = true;
    free_module_object_locked(src_bag, src_slot);
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
        const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][requested_dst_slot];
        if (!try_merge(requested_dst_slot)) {
            if (existing.category != 0 || existing.count != 0) {
                VIRTBAG_LOG("cross move reject extension->extension destination occupied=%d/%d",
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
            const virtual_bag::Item& existing = g_virtual_bag_state.items[dst_bag][slot];
            if (existing.category != 0 || existing.count != 0) continue;
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
    free_module_object_locked(src_bag, src_slot);
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
            const int category = drag_source_category_locked();
            if (g_virtual_bag_state.types[index] == 0 && category >= 1 && category <= 4) {
                return try_equip_on_extension_tab_drop_locked(index, moving_control);
            }
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
                                      category >= 1 && category <= 4;
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
    auto& session = g_extension_drag_session;
    if (session.phase == virtual_bag::DragPhase::kIdle ||
        g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_extension_logical_bag(target_bag) ||
        target_bag == session.source_bag ||
        !virtual_bag::drag_session_generation_current(session, g_extension_tab_generation)) {
        return false;
    }
    if (session.phase == virtual_bag::DragPhase::kPressed) {
        advance_projected_drag_session_locked();
    }
    const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
        target_bag, -1, true);
    if (virtual_bag::session_resolve_target(
            &session, g_extension_tab_generation, target_kind) ==
            virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_begin_transaction(&session, g_extension_tab_generation) ==
            virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_on_release(&session, g_extension_tab_generation) !=
            virtual_bag::DragTransition::kAdvanced ||
        !virtual_bag::session_claim_transaction(&session)) {
        return false;
    }
    const bool should_equip = extension_source_should_equip_locked(target_bag);
    bool handled = false;
    if (should_equip) {
        handled = try_equip_on_extension_tab_drop_locked(
            target_bag, touch_moving_item_control_locked());
    } else {
        handled = move_extension_to_extension_locked(
            session.source_bag, session.source_slot, target_bag, -1);
        if (handled) {
            persist_state_locked();
            refresh_projected_module_view_after_move_locked();
        }
    }
    virtual_bag::session_finish_transaction(&session, handled);
    VIRTBAG_LOG("p5 session tab drop source=%d/%d target_bag=%d handled=%d equip=%d",
                session.source_bag, session.source_slot, target_bag, handled ? 1 : 0,
                should_equip ? 1 : 0);
    return handled;
}

