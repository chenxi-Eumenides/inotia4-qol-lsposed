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
    return g_state_entry != nullptr &&
           enter == *reinterpret_cast<uintptr_t*>(g_state_entry + 0x10);
}

int virtual_bag_inventory_state_id() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state_entry == nullptr) return -1;
    return *reinterpret_cast<int32_t*>(g_state_entry);
}

// G-8：item proc 0x02（drop 到扩展格控件）路由——src=模块拖动状态（press 时
// projection_slot_at 记录），dst=落点控件索引。成功返回 true（wrapper 吞原版事件）。
bool virtual_bag_projection_drop_to_slot(void* dst_control, void* src_control) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || dst_control == nullptr ||
        src_control == nullptr || fn_ui_equip_get_item_slot_index == nullptr ||
        fn_control_object_get_data == nullptr) {
        return false;
    }
    void* src_data = fn_control_object_get_data(src_control);
    void* item = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
    int src_bag = -1, src_slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &src_bag, &src_slot)) {
        return false;
    }
    const int dst_slot = fn_ui_equip_get_item_slot_index(dst_control);
    if (dst_slot < 0 || dst_slot >= g_virtual_bag_state.capacities[src_bag]) {
        return false;
    }
    if (g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
        if (g_extension_drag_session.source_bag != src_bag ||
            g_extension_drag_session.source_slot != src_slot ||
            g_extension_drag_session.view_generation != g_extension_tab_generation) {
            return false;
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
            return false;
        }
    }
    const bool routed = move_extension_to_extension_locked(src_bag, src_slot, src_bag, dst_slot);
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
        virtual_bag::session_finish_transaction(&g_extension_drag_session, routed);
    }
    if (routed) {
        persist_state_locked();
        refresh_projected_module_view_after_move_locked();
        VIRTBAG_LOG("drop routed ext->ext dst_slot=%d", dst_slot);
    }
    return routed;
}

void virtual_bag_ui_start_auto_inject() {
    ensure_inject_thread();
    ensure_lifecycle_thread();
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

bool virtual_bag_allow_original_tab_drop(void* control, void* source_control) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_virtual_bag_enabled.load() || !g_module_view_installed ||
        control == nullptr || source_control == nullptr ||
        fn_ui_equip_get_item_slot_index == nullptr) {
        return false;
    }
    if (extension_tab_index(control) >= 0 ||
        fn_ui_equip_get_item_slot_index(control) < virtual_bag::kSlotCount) {
        return false;
    }
    void* source_data = fn_control_object_get_data != nullptr
                            ? fn_control_object_get_data(source_control)
                            : nullptr;
    void* item = source_data != nullptr ? *reinterpret_cast<void**>(source_data) : nullptr;
    int source_bag = -1;
    int source_slot = -1;
    const bool allowed = item != nullptr && module_slot_of_item_locked(item, &source_bag, &source_slot);
    if (allowed) {
        VIRTBAG_LOG("original tab drop allowed control=%p source=%d/%d",
                    control, source_bag, source_slot);
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
    if (enabled) virtual_bag_ui_start_auto_inject();
    return true;
}

bool virtual_bag_enabled() {
    return g_virtual_bag_enabled.load();
}

void virtual_bag_prepare_save_slot_load() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_module_view_installed) restore_module_view_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
}

void virtual_bag_prepare_main_menu() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    prepare_main_menu_locked();
}

bool virtual_bag_save_game() {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_module_view_installed) restore_module_view_locked();
        recover_pending_transaction_locked();
    }
    // fn_save 必须在锁外：内部 SAVE_SaveInventory 门禁 wrapper 需重新获锁做投影恢复，
    // 同线程重入 std::mutex 会自死锁（与 op_ok 锁内刷新同型）。
    g_explicit_save_in_progress = true;
    const int result = fn_save != nullptr ? fn_save() : 0;
    bool state_saved = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (result != 0) {
            state_saved = persist_state_locked(true);
            if (state_saved) {
                g_virtual_bag_state.pending = {};
                g_item_state_dirty = false;
            }
        }
    }
    g_explicit_save_in_progress = false;
    VIRTBAG_LOG("virtual bag save result native=%d sidecar=%d pending=%d",
                result != 0 ? 1 : 0, state_saved ? 1 : 0,
                g_virtual_bag_state.pending.valid ? 1 : 0);
    return state_saved && result != 0;
}

// G-14（方案 C）：原版物品操作后 RefreshItemArea 会以 INVEN 重刷控件，窗口袋投影需重写。
// 投影只占窗口袋；其他袋调用为 no-op。调用方（data_op_*）不持模块锁，此处安全获锁。
bool virtual_bag_sync_projected_slot(int display_bag, int slot) {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr) {
        return false;
    }
    if (!virtual_bag::valid_original_transaction_bag(display_bag) ||
        display_bag != original_bag_locked()) {
        return false;
    }
    if (slot < 0 || slot >= g_virtual_bag_state.capacities[g_module_view_index]) {
        return false;
    }
    void* live_root = g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA)
                                  : nullptr;
    void* ctrl = valid_child_locked(live_root, slot);
    void* item = g_module_objects[g_module_view_index][slot];
    if (ctrl == nullptr) return false;
    fn_control_item_set_item(ctrl, item);
    return true;
}

bool virtual_bag_sync_projected_item_control(void* control) {
    if (control == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index) ||
        g_projected_item_root == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr) {
        return false;
    }
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (fn_control_object_get_child(g_projected_item_root, slot) == control) {
            fn_control_item_set_item(control, g_module_objects[g_module_view_index][slot]);
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
}

std::string data_virtual_bag_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!game_in_world()) {
        return "{\"injected\":false,\"state\":{}}";
    }
    ensure_state_loaded_locked();
    return "{\"injected\":" + std::string(g_state_entry != nullptr ? "true" : "false") +
           ",\"extension_tab_button\":" +
           std::string(g_extension_tab_buttons[0] != nullptr ? "true" : "false") +
           ",\"state\":" + virtual_bag::state_json(g_virtual_bag_state, true) + "}";
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

