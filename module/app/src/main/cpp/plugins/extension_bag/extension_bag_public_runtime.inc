JNIEnv* extension_bag_current_env() {
    return current_env();
}

jclass extension_bag_bridge_class() {
    return g_virtual_bag_bridge_class;
}

virtual_bag::State* extension_bag_state() {
    return &g_virtual_bag_state;
}

uint64_t extension_bag_isolation_now_ms() {
    return isolation_now_ms_locked();
}

// G-8/H4 共享扩展身份判据：所有事件层与函数层守卫都反查同一份
// g_module_objects，避免 source 判定在不同防线间漂移。调用方必须已持有
// g_virtual_bag_mtx。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr || out_bag == nullptr || out_slot == nullptr) return false;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            if (g_module_objects[bag][slot] == item) {
                *out_bag = bag;
                *out_slot = slot;
                return true;
            }
        }
    }
    return false;
}

uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2, void* param) {
    if (g_p5_observation_active || !p5_observation_enabled() ||
        !p5_should_begin_item_observation(event)) {
        return 0;
    }
    g_p5_observation_active = true;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const uint64_t observation_token =
        log_p5_observation_locked("item-pre", control, event, x2 != nullptr, param != nullptr,
                                  nullptr, false, 0);
    if (observation_token == 0) g_p5_observation_active = false;
    return observation_token;
}

void virtual_bag_observe_item_proc_source(uint64_t observation_token, void* control, uint64_t event,
                                          void* x2, void* param, void* source_control) {
    if (observation_token == 0) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    log_p5_observation_locked("item-source", control, event, x2 != nullptr, param != nullptr,
                              source_control, false, 0, -1, -1, observation_token);
}

void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control, uint64_t event,
                                        void* x2, void* param, uint64_t result) {
    if (observation_token == 0) return;
    (void)control;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        // The original proc may have rebuilt the control tree. Do not dereference its prior control.
        log_p5_observation_locked("item-post", nullptr, event, x2 != nullptr, param != nullptr,
                                  nullptr, true, result, -1, -1, observation_token);
    }
    g_p5_observation_active = false;
}

bool virtual_bag_is_inventory_enter(uintptr_t enter) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return g_inventory_enter != 0 && enter == g_inventory_enter;
}

bool virtual_bag_capture_move_item_observation(void* item, int target_bag, int target_slot,
                                               VirtualBagMoveItemObservation* out) {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    *out = {};
    out->module_view_index = g_module_view_index;
    out->drag_session_active = projected_drag_session_active_locked();
    out->extension_item = item != nullptr &&
                          module_slot_of_item_locked(item, &out->extension_bag,
                                                     &out->extension_slot);
    if (out->extension_item) {
        out->extension_handle =
            g_module_object_handles[out->extension_bag][out->extension_slot];
        for (const ownership::State state : {ownership::State::kModuleOwned,
                                             ownership::State::kBorrowedForView,
                                             ownership::State::kInventoryOwned,
                                             ownership::State::kReleased}) {
            if (ownership::live_state(g_ownership_ledger, out->extension_handle, state)) {
                out->ownership_state = static_cast<int>(state);
                break;
            }
        }
    }
    const PhysicalInventorySnapshot snapshot = capture_physical_inventory_snapshot_locked();
    out->physical_valid = snapshot.valid;
    out->physical_digest = snapshot.digest;
    out->physical_nonnull = snapshot.nonnull;
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        void* slot_item = snapshot.slots[index];
        if (item != nullptr && slot_item == item && out->source_bag < 0) {
            out->source_bag = static_cast<int>(index / virtual_bag::kSlotCount);
            out->source_slot = static_cast<int>(index % virtual_bag::kSlotCount);
            out->source_ptr = slot_item;
        }
    }
    if (target_bag >= 0 && target_bag < 6 && target_slot >= 0 &&
        target_slot < virtual_bag::kSlotCount) {
        out->target_ptr = snapshot.slots[target_bag * virtual_bag::kSlotCount + target_slot];
    }
    return true;
}

int virtual_bag_inventory_state_id() {
    // g_state_entry is the inventory PopupState entry set during acquire; world_facts.state is
    // global and may trigger per-frame main-menu cleanup while the inventory panel is open.
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state_entry == nullptr) return -1;
    return *reinterpret_cast<int32_t*>(g_state_entry);
}

bool apply_extension_material_to_slot_locked(std::unique_lock<std::mutex>& lock,
                                             int src_bag, int src_slot, int dst_bag,
                                             int dst_slot, void* material, void* target_item) {
    const int source_category = g_virtual_bag_state.items[src_bag][src_slot].category;
    const bool source_is_jewel = read_item_is_jewel(source_category);
    const char* kind = source_is_jewel ? "jewel" : "scroll";
    int32_t apply_allowed = 0;
    if (g_game == nullptr || g_character_extras->read.can_apply_stuff == nullptr ||
        !qol::contracts::succeeded(g_character_extras->read.can_apply_stuff(
            target_item, material, &apply_allowed)) || apply_allowed == 0) {
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=not_applicable kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    const auto* save_facts = qol::features::extension_bag::save_facts();
    if (save_facts == nullptr || save_facts->is_ok == nullptr ||
        extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->equipment.apply_stuff == nullptr) {
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=symbol_unresolved kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    ModuleUseToken use_token{};
    if (!module_use_begin_locked(src_bag, src_slot, material, &use_token)) {
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=stale_token kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    VIRTBAG_LOG("apply branch kind=%s equip=%p material=%p", kind, target_item, material);
    ModuleUseToken* previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    lock.unlock();
    const bool save_ok = read_save_is_ok();
    const bool apply_ok = save_ok && extension_apply_stuff(target_item, material);
    g_active_module_use_token = previous_use_token;
    lock.lock();

    if (!apply_ok) {
        module_use_abort_locked(use_token);
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=is_ok_failed kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    bool consumed = false;
    ModuleUseReleasePlan release_plan{};
    const bool finished = module_use_finish_locked(use_token, &consumed, &release_plan);
    if (!finished || !consumed) {
        if (finished) {
            module_use_abort_isolate_locked(use_token);
        } else if (!module_use_abort_locked(use_token)) {
            module_use_abort_isolate_locked(use_token);
        }
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=%s kind=%s equip=%p material=%p",
                    finished ? "native_apply_failed" : "stale_token",
                    kind, target_item, material);
        return false;
    }
    if (release_plan.pending) {
        lock.unlock();
        const bool released = module_use_release_native(release_plan);
        lock.lock();
        if (!released || !module_use_finalize_release_locked(release_plan)) {
            if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
                virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
            }
            VIRTBAG_LOG("apply reject reason=material_release_failed kind=%s equip=%p material=%p",
                        kind, target_item, material);
            return false;
        }
    }
    if (g_module_objects[dst_bag][dst_slot] != target_item) {
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=stale_target kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(target_item, &payload, &payload_size)) {
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&g_extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=target_payload_sync_failed kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    virtual_bag::Item& target_descriptor = g_virtual_bag_state.items[dst_bag][dst_slot];
    target_descriptor.payload = payload;
    target_descriptor.payload_size = payload_size;
    // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）。
    if (read_item_cumulate_count(target_item, &target_descriptor.count)) {
        target_descriptor.count = static_cast<int>(virtual_bag_canonical_count(target_item, 1));
    }
    g_module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(target_descriptor);
    g_item_state_dirty = true;
    persist_state_locked();
    refresh_projected_module_view_after_move_locked();
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
        virtual_bag::session_finish_transaction(&g_extension_drag_session, true);
    }
    VIRTBAG_LOG("apply committed kind=%s equip=%p material=%p", kind, target_item, material);
    return true;
}

// G-8：item proc 0x02（drop 到扩展格控件）路由——src=模块拖动状态（press 时
// projection_slot_at 记录），dst=落点控件索引。必须区分“非扩展源”“扩展事务成功”
// 和“扩展源但事务拒绝”：最后一种也必须吞掉原版事件，避免 INVEN_MoveItem 改写
// 真实物理槽。src/dst 必须属于当前 g_module_view_index 对应的 projected root；
// stale 控件或错视图一律是 kRejected。
VirtualBagProjectionDropResult virtual_bag_projection_drop_to_slot(void* dst_control,
                                                                     void* src_control) {
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    if (src_control == nullptr) {
        return VirtualBagProjectionDropResult::kNotExtension;
    }
    void* src_data = extension_bag_ui_data(src_control);
    void* item = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
    int src_bag = -1, src_slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &src_bag, &src_slot)) {
        return VirtualBagProjectionDropResult::kNotExtension;
    }
    if (!g_module_view_installed || dst_control == nullptr ||
        g_sdk == nullptr || g_sdk->ui == nullptr ||
        g_module_view_index != src_bag || g_projected_item_root == nullptr) {
        VIRTBAG_LOG("drop rejected: extension source view mismatch src=%d/%d view=%d root=%p",
                    src_bag, src_slot, g_module_view_index, g_projected_item_root);
        return VirtualBagProjectionDropResult::kRejected;
    }
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    void* projected_source = valid_child_locked(g_projected_item_root, src_slot);
    if (live_root == nullptr || live_root != g_projected_item_root ||
        projected_source != src_control) {
        VIRTBAG_LOG("drop rejected: extension source control is not current projection "
                    "src=%d/%d source=%p expected=%p root=%p live_root=%p",
                    src_bag, src_slot, src_control, projected_source,
                    g_projected_item_root, live_root);
        return VirtualBagProjectionDropResult::kRejected;
    }
    const int dst_slot = extension_bag_ui_item_slot(dst_control);
    const bool destination_control_matches =
        dst_slot >= 0 && dst_slot < g_virtual_bag_state.capacities[g_module_view_index] &&
        valid_child_locked(g_projected_item_root, dst_slot) == dst_control;
    if (!virtual_bag::projection_drop_context_matches(
            g_module_view_index, src_bag, true, true, destination_control_matches)) {
        VIRTBAG_LOG("drop rejected: extension source invalid projected target src=%d/%d dst=%d",
                    src_bag, src_slot, dst_slot);
        return VirtualBagProjectionDropResult::kRejected;
    }
    if (g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
        if (g_extension_drag_session.source_bag != src_bag ||
            g_extension_drag_session.source_slot != src_slot ||
            g_extension_drag_session.view_generation != g_extension_tab_generation ||
            !virtual_bag::drag_session_source_identity_matches(
                g_extension_drag_session, src_bag, src_slot,
                g_module_object_handles[src_bag][src_slot])) {
            VIRTBAG_LOG("drop rejected: extension source session mismatch src=%d/%d phase=%d",
                        src_bag, src_slot, static_cast<int>(g_extension_drag_session.phase));
            return VirtualBagProjectionDropResult::kRejected;
        }
        if (g_extension_drag_session.phase == virtual_bag::DragPhase::kPressed) {
            advance_projected_drag_session_locked();
        }
        const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
            virtual_bag::kExtensionLogicalBagFirst + src_bag, dst_slot);
        if (virtual_bag::session_resolve_target(&g_extension_drag_session,
                                                g_extension_tab_generation, target_kind) ==
                 virtual_bag::DragTransition::kIllegal ||
             virtual_bag::session_begin_transaction(&g_extension_drag_session,
                                                    g_extension_tab_generation) ==
                 virtual_bag::DragTransition::kIllegal ||
             virtual_bag::session_on_release(&g_extension_drag_session,
                                             g_extension_tab_generation) !=
                 virtual_bag::DragTransition::kAdvanced ||
             !virtual_bag::session_claim_transaction(&g_extension_drag_session)) {
            VIRTBAG_LOG("drop rejected: extension source session transition failed src=%d/%d",
                        src_bag, src_slot);
            return VirtualBagProjectionDropResult::kRejected;
        }
    }
    const int dst_bag = g_module_view_index;
    const int source_category = g_virtual_bag_state.items[src_bag][src_slot].category;
    const bool source_is_apply_material =
        read_item_is_jewel(source_category) || read_item_is_enchant_scroll(source_category);
    void* dst_data = extension_bag_ui_data(dst_control);
    void* target_item = dst_data != nullptr ? *reinterpret_cast<void**>(dst_data) : nullptr;
    // S-03 apply 准入（R-50）：同袋 + 扩展宝石/强化卷轴源 + 目标为装备类 +
    // IsApplyStuff 判真。目标类别用 item_is_equip 判定（kNotEncoded=装备类；
    // 数据不可用返回 false，按非装备 fail-closed 回退 swap），不得按
    // 「不可堆叠」近似装备。非 apply 候选必须 fallthrough 到下方扩展事务，
    // 不得吞事件或 Blocked。
    int32_t apply_allowed = 0;
    const bool apply_stuff_allowed = target_item != nullptr && g_game != nullptr &&
        g_character_extras->read.can_apply_stuff != nullptr &&
        qol::contracts::succeeded(g_character_extras->read.can_apply_stuff(
            target_item, item, &apply_allowed)) && apply_allowed != 0;
    const bool apply_candidate = extension_bag_is_apply_candidate(
        src_bag == dst_bag, source_is_apply_material,
        target_item != nullptr && item_is_equip(target_item), apply_stuff_allowed);
    if (apply_candidate) {
        return apply_extension_material_to_slot_locked(
            lock, src_bag, src_slot, dst_bag, dst_slot, item, target_item)
            ? VirtualBagProjectionDropResult::kHandled
            : VirtualBagProjectionDropResult::kRejected;
    }

    const bool routed = move_extension_to_extension_locked(src_bag, src_slot, src_bag, dst_slot);
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
        virtual_bag::session_finish_transaction(&g_extension_drag_session, routed);
    }
    if (routed) {
        persist_state_locked();
        refresh_projected_module_view_after_move_locked();
        VIRTBAG_LOG("drop routed ext->ext dst_slot=%d", dst_slot);
        return VirtualBagProjectionDropResult::kHandled;
    }
    VIRTBAG_LOG("drop rejected: extension transaction refused src=%d/%d dst=%d",
                src_bag, src_slot, dst_slot);
    return VirtualBagProjectionDropResult::kRejected;
}

bool virtual_bag_module_view_installed() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return g_module_view_installed;
}

bool virtual_bag_original_item_input_blocked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const bool blocked = g_virtual_bag_enabled.load() &&
                         (g_virtual_bag_state.mode == virtual_bag::Mode::kModule ||
                          g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule);
    if (blocked) {
        VIRTBAG_LOG("original item input blocked mode=%d selected=%d",
                    static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected);
    }
    return blocked;
}

bool virtual_bag_allow_original_tab_drop(void* control, void* source_control,
                                         uint64_t caller_offset) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_virtual_bag_enabled.load() || !g_module_view_installed ||
        control == nullptr || source_control == nullptr ||
        g_sdk == nullptr || g_sdk->ui == nullptr) {
        return false;
    }
    if (extension_tab_index(control) >= 0) {
        return false;
    }
    const int target_slot = extension_bag_ui_item_slot(control);
    if (target_slot < 0) {
        return false;
    }
    void* source_data = extension_bag_ui_data(source_control);
    void* item = source_data != nullptr ? *reinterpret_cast<void**>(source_data) : nullptr;
    int source_bag = -1;
    int source_slot = -1;
    const bool extension_source = item != nullptr &&
                                   module_slot_of_item_locked(item, &source_bag, &source_slot);
    if (extension_source) {
        VIRTBAG_LOG(
            "drop rejected: extension source entering original proc item=%p caller=libgame+0x%llx "
            "view=%d session=%d source=%d/%d target_slot=%d",
            item, static_cast<unsigned long long>(caller_offset), g_module_view_index,
            g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle ? 1 : 0,
            source_bag, source_slot, target_slot);
        return false;
    }
    const bool allowed = item != nullptr;
    if (allowed) {
        VIRTBAG_LOG("original drop allowed control=%p source=original item=%p target_slot=%d",
                    control, item, target_slot);
    }
    return allowed;
}

bool set_virtual_bag_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_virtual_bag_enabled.store(enabled);
        if (!enabled) {
            restore_module_view_locked();
            g_virtual_bag_state.mode = virtual_bag::Mode::kOriginal;
            g_virtual_bag_state.selected = -1;
            g_virtual_bag_state.inspected = -1;
        }
    }
    if (!set_extension_source_protection_enabled(enabled)) {
        VIRTBAG_LOG("extension source protection update deferred enabled=%d",
                    enabled ? 1 : 0);
    }
    if (enabled) {
        ensure_lifecycle_task();
    } else {
        remove_lifecycle_task();
    }
    return true;
}

bool virtual_bag_enabled() {
    return g_virtual_bag_enabled.load();
}

bool virtual_bag_prepare_save_slot_load() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!module_cache_clear_preflight_locked()) {
        VIRTBAG_LOG("prepare save-slot load preflight failed");
        return false;
    }
    if (g_module_view_installed) restore_module_view_locked();
    if (!clear_module_cache_locked()) {
        VIRTBAG_LOG("prepare save-slot load blocked by active module object");
        return false;
    }
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
    return true;
}

bool virtual_bag_prepare_main_menu() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    return prepare_main_menu_locked();
}

// 模块级保存 prepare 中扩展背包自己的校验部分。模块级协调在
// Host SDK 存档参与者的 prepare 阶段，各功能只负责自己数据的
// 校验与收敛；本函数即扩展背包在 prepare 中的唯一校验/清理落点。调用方必须已持有
// g_virtual_bag_mtx。
// 1) 归一化 types → capacities（派生容量的唯一来源）；
// 2) 剔除派生容量之外的槽位数据：超容数据不参与 sidecar 序列化、不落盘；
// 3) 容量外槽若仍持有物化对象或账本句柄（写入路径全部收紧后不可达），拒绝保存并记
//    ERROR，避免半持久化与悬挂引用。
bool virtual_bag_module_save_validate_locked() {
    virtual_bag::normalize(&g_virtual_bag_state);
    int discarded = 0;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        for (int slot = capacity; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = g_virtual_bag_state.items[bag][slot];
            if (item.category <= 0 && item.count <= 0) continue;
            if (g_module_objects[bag][slot] != nullptr ||
                g_module_object_handles[bag][slot] != 0) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "save validation reject: over-capacity slot holds live object bag=%d slot=%d",
                              bag, slot);
                return false;
            }
            ++discarded;
        }
    }
    if (discarded > 0) {
        virtual_bag::discard_over_capacity(&g_virtual_bag_state);
        g_item_state_dirty = true;
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "save validation discarded %d over-capacity item(s)", discarded);
    }
    return true;
}

// P3c：SlotBound 写入统一经 save.slot_data 的 request_write（安全点排空），provider 在
// 原版保存事务内捕获提交；旧 ModuleSaveCoordinator 参与者序退役，本三函数删除。
// 扫描入口的准入拒绝见 virtual_bag_for_each_logical_item 与各写操作入口。

// G-14（方案 C）：原版物品操作后 RefreshItemArea 会以 INVEN 重刷控件，窗口袋投影需重写。
// 投影只占窗口袋；其他袋调用为 no-op。调用方（data_op_*）不持模块锁，此处安全获锁。
bool virtual_bag_sync_projected_slot(int display_bag, int slot) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || g_sdk == nullptr || g_sdk->ui == nullptr) {
        return false;
    }
    if (!virtual_bag::valid_original_transaction_bag(display_bag) ||
        display_bag != original_bag_locked()) {
        return false;
    }
    if (slot < 0 || slot >= g_virtual_bag_state.capacities[g_module_view_index]) {
        return false;
    }
    return sync_projected_slot_locked(g_module_view_index, slot);
}

bool virtual_bag_sync_projected_item_control(void* control) {
    if (control == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || g_sdk == nullptr || g_sdk->ui == nullptr) {
        return false;
    }
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (extension_bag_ui_child_at(g_projected_item_root, slot) == control) {
            (void)extension_bag_ui_set_control_item(
                control, g_module_objects[g_module_view_index][slot]);
            return true;
        }
    }
    return false;
}

bool virtual_bag_sync_projected_bag() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    refresh_projection_if_overwritten_locked();
    return g_module_view_installed;
}

void virtual_bag_ui_register_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_bridge_class != nullptr) env->DeleteGlobalRef(g_virtual_bag_bridge_class);
    g_virtual_bag_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
    // P3c：旧 module_save 参与者注册已退出——保存协调唯一走 save.slot_data + core.save
    // 协调器；此处不得保留第二套参与者顺序。
}

void virtual_bag_for_each_logical_item(LogicalInventoryItemFn fn, void* ctx) {
    if (fn == nullptr || !g_virtual_bag_enabled.load() || !read_world_in_world()) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    // P3c 准入：session 未加载/epoch 未登记/降级阻断时明确拒绝扫描（不产出任何物品，
    // 不再惰性 IO 兜底空状态）。
    if (!extension_bag_session_admitted_locked()) {
        VIRTBAG_LOG("logical item scan rejected: session not admitted");
        return;
    }
    // 输出模式视图数量（R-47 决策 b）：启用态 canonical、关闭态 b。占位判定
    // 仍按 descriptor canonical（结构性判断与模式无关）。
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        for (int slot = 0; slot < capacity; ++slot) {
            const virtual_bag::Item& item = g_virtual_bag_state.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            qol::contracts::QolItemHandle item_handle = qol::contracts::kInvalidItemHandle;
            void* native_item = g_module_objects[bag][slot];
            if (native_item != nullptr && extension_bag_register_item_handle(native_item)) {
                (void)extension_bag_item_handle_for_pointer(native_item, &item_handle);
            }
            if (fn(virtual_bag::kExtensionLogicalBagFirst + bag, slot, item.category,
                   static_cast<int>(virtual_bag_effective_count(
                        g_module_objects[bag][slot], static_cast<uint32_t>(item.count))),
                   item_handle, ctx)) {
                return;
            }
        }
    }
}

bool virtual_bag_logical_capacity(int32_t logical_bag, int32_t* out_capacity) {
    if (out_capacity == nullptr || !virtual_bag::valid_extension_logical_bag(logical_bag) ||
        !g_virtual_bag_enabled.load() || !read_world_in_world()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    const int internal_bag = virtual_bag::extension_internal_bag(logical_bag);
    if (!virtual_bag::valid_index(internal_bag)) return false;
    *out_capacity = g_virtual_bag_state.capacities[internal_bag];
    return true;
}

std::string data_virtual_bag_test_equip(int index, int bag_type) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        updated = virtual_bag::set_test_equipped(&g_virtual_bag_state, index, bag_type);
        if (updated) g_item_state_dirty = true;
    }
    if (!updated) return op_err("bad virtual bag index or bag type");
    return op_ok();
}

std::string data_virtual_bag_test_item(int index, int slot, int category, int count) {
    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        updated = virtual_bag::set_item(&g_virtual_bag_state, index, slot, category, count);
        if (updated) g_item_state_dirty = true;
    }
    if (!updated) return op_err("bad virtual bag item");
    return op_ok();
}
