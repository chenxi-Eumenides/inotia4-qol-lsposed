struct PhysicalInventorySnapshot {
    std::array<void*, virtual_bag::kPhysicalInventorySnapshotSlots> slots{};
    uint64_t digest = 0;
    int nonnull = 0;
    bool valid = false;
};

PhysicalInventorySnapshot capture_physical_inventory_snapshot_locked() {
    PhysicalInventorySnapshot snapshot{};
    snapshot.digest = 1469598103934665603ULL;
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        void* item = nullptr;
        qol::contracts::QolItemHandle handle{};
        if (g_item_facts == nullptr || g_item_facts->physical_item_at == nullptr ||
            !qol::contracts::succeeded(g_item_facts->physical_item_at(
                static_cast<int32_t>(index / virtual_bag::kSlotCount),
                static_cast<int32_t>(index % virtual_bag::kSlotCount), &handle)) ||
            (handle.slot != 0 && !extension_bag_private_item_for_handle(handle, &item))) {
            return snapshot;
        }
        snapshot.slots[index] = item;
        if (item != nullptr) ++snapshot.nonnull;
        snapshot.digest ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(item));
        snapshot.digest *= 1099511628211ULL;
    }
    snapshot.valid = true;
    return snapshot;
}

bool restore_physical_inventory_snapshot_locked(const PhysicalInventorySnapshot& snapshot) {
    if (!snapshot.valid || g_game == nullptr ||
        g_write_port->inventory.set_physical_item == nullptr) return false;
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        if (!qol::contracts::succeeded(g_write_port->inventory.set_physical_item(
                static_cast<int32_t>(index / virtual_bag::kSlotCount),
                static_cast<int32_t>(index % virtual_bag::kSlotCount), snapshot.slots[index]))) {
            return false;
        }
    }
    return true;
}

void log_original_event_inventory_snapshot(const char* phase, uint64_t event,
                                           bool session_has_projected_item,
                                           int source_bag, int source_slot,
                                           const PhysicalInventorySnapshot& snapshot) {
    VIRTBAG_LOG("orig_event %s event=0x%llx session_projected=%d source=%d/%d inven_valid=%d digest=0x%llx nonnull=%d",
                phase, static_cast<unsigned long long>(event),
                session_has_projected_item ? 1 : 0, source_bag, source_slot,
                snapshot.valid ? 1 : 0,
                static_cast<unsigned long long>(snapshot.digest), snapshot.nonnull);
}

uint64_t call_original_event_with_inventory_guard(uint64_t event, uint64_t param,
                                                  uint64_t param2) {
    if (g_orig_event == nullptr) return 0;

    PhysicalInventorySnapshot before{};
    bool session_has_projected_item = false;
    int source_bag = -1;
    int source_slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        session_has_projected_item = projected_drag_session_active_locked();
        if (session_has_projected_item) {
            source_bag = g_extension_drag_session.source_bag;
            source_slot = g_extension_drag_session.source_slot;
        }
        before = capture_physical_inventory_snapshot_locked();
        log_original_event_inventory_snapshot("pre", event, session_has_projected_item,
                                              source_bag, source_slot, before);
    }

    const uint64_t result = g_orig_event(event, param, param2);
    bool restored = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const PhysicalInventorySnapshot after = capture_physical_inventory_snapshot_locked();
        log_original_event_inventory_snapshot("post", event, session_has_projected_item,
                                              source_bag, source_slot, after);
        if (session_has_projected_item && before.valid && after.valid &&
            virtual_bag::physical_inventory_snapshot_changed(before.slots, after.slots)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR original event changed physical inventory while projected session active "
                        "event=0x%llx before_digest=0x%llx after_digest=0x%llx",
                        static_cast<unsigned long long>(event),
                        static_cast<unsigned long long>(before.digest),
                        static_cast<unsigned long long>(after.digest));
            for (size_t index = 0; index < before.slots.size(); ++index) {
                if (before.slots[index] == after.slots[index]) continue;
                const int bag = static_cast<int>(index / virtual_bag::kSlotCount);
                const int slot = static_cast<int>(index % virtual_bag::kSlotCount);
                QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR physical inventory mutation bag=%d slot=%d old=%p new=%p",
                            bag, slot, before.slots[index], after.slots[index]);
            }
            restored = restore_physical_inventory_snapshot_locked(before);
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR physical inventory rollback event=0x%llx restored=%d",
                        static_cast<unsigned long long>(event), restored ? 1 : 0);
        }
    }
    if (restored) {
        const bool synced = virtual_bag_sync_projected_bag();
        VIRTBAG_LOG_ERROR("ERROR physical inventory rollback projection_sync=%d", synced ? 1 : 0);
    }
    return result;
}

bool consume_projected_release_locked(std::unique_lock<std::mutex>& lock, int64_t x, int64_t y) {
    if (!projected_drag_session_active_locked()) return false;
    auto& session = g_extension_drag_session;
    if (session.phase == virtual_bag::DragPhase::kCommitted ||
        session.phase == virtual_bag::DragPhase::kRejected ||
        session.phase == virtual_bag::DragPhase::kCancelled) {
        QOL_LOG_DEBUG(QolDomain::kExtensionBag, "projected release consumed terminal session event=0x18 token=%llu phase=%d",
                    static_cast<unsigned long long>(session.token),
                    static_cast<int>(session.phase));
        session = {};
        return true;
    }
    const int original_target_bag = original_bag_button_index(x, y);
    bool routed = false;
    if (original_target_bag >= 0 &&
        virtual_bag::valid_original_transaction_bag(original_target_bag)) {
        session.target_kind = virtual_bag::DragTargetKind::kOriginalBag;
        session.target_bag = original_target_bag;
        session.target_slot = -1;
        routed = route_deferred_extension_to_original_locked(lock);
    } else {
        routed = route_projected_session_drop_locked(lock, x, y);
    }
    if (session.phase != virtual_bag::DragPhase::kCommitted &&
        session.phase != virtual_bag::DragPhase::kRejected &&
        session.phase != virtual_bag::DragPhase::kCancelled) {
        cancel_projected_drag_session_locked("release-intercepted-without-target");
    }
    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "projected release intercepted event=0x18 source=%d/%d target_original=%d routed=%d",
                session.source_bag, session.source_slot, original_target_bag, routed ? 1 : 0);
    session = {};
    return true;
}

uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (p5_panel_observation_enabled()) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        log_p5_observation_locked("panel-entry", nullptr, event, param2 != 0, param != 0,
                                  nullptr, false, 0);
    }
    const bool extension_enabled = g_virtual_bag_enabled.load();
    int32_t state_open = 0;
    const auto* world_facts = qol::features::extension_bag::world_facts();
    const bool state_ready = world_facts != nullptr && world_facts->game_state_gate != nullptr &&
        qol::contracts::succeeded(world_facts->game_state_gate(&state_open));
    const uint32_t gamestate = state_ready && state_open != 0 ? 0U : 1U;
    if (!extension_enabled || gamestate != 0) {
        if (extension_enabled && event == 0x18) {
            bool release_cleanup_needed = false;
            int64_t release_x = 0;
            int64_t release_y = 0;
            {
                std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
                if (projected_drag_session_active_locked()) {
                    if (param != 0) {
                        release_x = *reinterpret_cast<const int64_t*>(param);
                        release_y = *reinterpret_cast<const int64_t*>(param + 8);
                    } else {
                        void* moving = nullptr;
                        void* drop_source = nullptr;
                        (void)extension_bag_ui_touch_state(&moving, &drop_source, &release_x, &release_y);
                    }
                    cancel_projected_drag_session_locked("release-bypassed-gamestate");
                    g_extension_drag_session = {};
                    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "projected release intercepted event=0x18 reason=gamestate=%u",
                                gamestate);
                    // Non-world cancellation is unhandled: clear native selection as original does.
                    release_cleanup_needed = true;
                }
            }
            if (release_cleanup_needed) {
                complete_original_release_cleanup_locked(nullptr, release_x, release_y, param2, false);
                return 1;
            }
        }
        if (event == 0x17 || event == 0x18 || event == 0x19) {
            VIRTBAG_LOG("touch bypass event=0x%llx enabled=%d gamestate=%u hooked=%d",
                        static_cast<unsigned long long>(event), extension_enabled ? 1 : 0,
                        gamestate, g_state_entry != nullptr ? 1 : 0);
        }
        const uint64_t result = call_original_event_with_inventory_guard(event, param, param2);
        if (p5_panel_observation_enabled()) {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            log_p5_observation_locked("panel-bypass-post", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, result);
        }
        return result;
    }
    int64_t x = 0;
    int64_t y = 0;
    bool exiting_to_original = false;
    if (event == 0x17 && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2, x, y);
        const int64_t grid_width = 4 * kGridStep - (kGridStep - kGridCell);
        const int64_t grid_height = 4 * kGridStep - (kGridStep - kGridCell);
        QOL_LOG_DEBUG(QolDomain::kExtensionBag, "touch bounds event=0x%llx x=%lld y=%lld grid=%lld,%lld %lldx%lld grid_hit=%d tab_origin=%lld,%lld mode=%d selected=%d overlay=%d root=%p root_gen=%llu state_gen=%llu",
                    static_cast<unsigned long long>(event), static_cast<long long>(x), static_cast<long long>(y),
                    static_cast<long long>(kGridX), static_cast<long long>(kGridY),
                    static_cast<long long>(grid_width), static_cast<long long>(grid_height),
        extension_bag_grid_hit(x, y) ? 1 : 0, static_cast<long long>(kCellX),
                    static_cast<long long>(kCellY), static_cast<int>(g_virtual_bag_state.mode),
                    g_virtual_bag_state.selected, g_module_view_installed ? 1 : 0,
                    g_extension_tab_root, static_cast<unsigned long long>(g_extension_tab_generation),
                    static_cast<unsigned long long>(g_inventory_generation));
        if (g_extension_touch_capture) {
            log_p5_observation_locked("panel-capture-press", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
    if (extension_bag_grid_hit(x, y)) {
            // G-6/G-7 零干预原则：投影常驻时网格触摸完全放行原版控件链
            // （TouchHandle 自带命中/选中动画/详情；操作按钮由 menu_gate 拦截；
            // 拖动 drop 由 SaveItemOnEmpty 门禁拦截）。模块不做任何状态清理——
            // press 时 reset/clear 会破坏 TouchHandle 的按压记录导致点击失效。
        }
    const int projected_slot = extension_bag_grid_slot_index(x, y, kOriginalGridX, kOriginalGridY);
        bool projected_item_press = false;
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            g_module_view_installed && virtual_bag::valid_index(g_module_view_index) &&
            projected_slot >= 0 &&
            projected_slot < g_virtual_bag_state.capacities[g_module_view_index] &&
            g_projected_item_root != nullptr) {
            projected_item_press = valid_child_locked(g_projected_item_root, projected_slot) != nullptr;
        }
        if (g_virtual_bag_state.mode != virtual_bag::Mode::kOriginal &&
            projected_slot >= 0 && !projected_item_press) {
            // The original grid is hidden underneath the module grid layout.
            // Consume it even when no extension drag is active.
            g_extension_drag = {};
            reset_drag_state_locked(nullptr);
            g_extension_touch_capture = true;
            log_p5_observation_locked("panel-hidden-grid-press", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        if (projected_item_press && g_extension_drag_session.phase == virtual_bag::DragPhase::kIdle) {
            begin_projected_drag_session_locked(projected_slot);
        }
        const int target_original_bag = original_bag_button_index(x, y);
        if (!g_extension_touch_capture && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            target_original_bag >= 0) {
            restore_module_view_locked();
            virtual_bag::begin_exit_module(&g_virtual_bag_state);
            clear_original_bag_selection_locked();
            g_exit_display_bag = g_original_current_got < kNoOriginalBagSelected ?
                                 g_original_current_got : g_original_current_direct;
            if (bind_original_exit_display_bag_locked(target_original_bag)) {
                g_exit_display_bag = static_cast<uint8_t>(target_original_bag);
            }
            exiting_to_original = true;
            log_exit_trace_locked("original_press_pre_orig", event, param, param2, x, y);
            log_p5_observation_locked("panel-original-pre", nullptr, event, param2 != 0,
                                      param != 0, nullptr, false, 0, x, y);
        }
    }
    if (event == 0x18) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else {
            void* moving = nullptr;
            void* drop_source = nullptr;
            (void)extension_bag_ui_touch_state(&moving, &drop_source, &x, &y);
        }
        bool release_cleanup_needed = false;
        bool release_cleanup_handled = false;
        {
            std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
            ensure_state_loaded_locked();
            log_exit_trace_locked("event_release", event, param, param2, x, y);
        if (virtual_bag::projected_release_owns_event(
                event, projected_drag_session_active_locked())) {
            // 0x18 是投影拖拽的唯一提交 owner。必须在原版 TouchHandle/INVEN_MoveItem
            // 之前完成扩展事务；失败也吞掉原版，防止真实物理槽被二次改写。
            consume_projected_release_locked(lock, x, y);
                // Owner/terminal release is handled by the module; preserve native selection.
                release_cleanup_needed = true;
                release_cleanup_handled = true;
            } else if (g_extension_touch_capture) {
            if (param == 0 && x == 0 && y == 0 && g_extension_drag.active) {
                x = g_extension_drag.current_x;
                y = g_extension_drag.current_y;
            }
            if (param == 0 && x == 0 && y == 0) {
                g_extension_drag = {};
                g_extension_touch_capture = false;
                log_p5_observation_locked("panel-capture-release-empty", nullptr, event,
                                          param2 != 0, param != 0, nullptr, true, 1, x, y);
                // Empty capture has no module transaction; use the unhandled cleanup variant.
                release_cleanup_needed = true;
                release_cleanup_handled = false;
            } else {
                constexpr int64_t kTouchSlop = 24;
                const int64_t dx = x - g_extension_drag.press_x;
                const int64_t dy = y - g_extension_drag.press_y;
                const bool is_click = g_extension_drag.active &&
                                      dx >= -kTouchSlop && dx <= kTouchSlop &&
                                      dy >= -kTouchSlop && dy <= kTouchSlop;
                if (is_click) {
                    g_virtual_bag_state.inspected = static_cast<int>(g_extension_drag.slot);
                    const virtual_bag::Item& item =
                        g_virtual_bag_state.items[g_extension_drag.bag][g_extension_drag.slot];
                    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "extension item click bag=%d slot=%d category=%d count=%d",
                                g_extension_drag.bag, g_extension_drag.slot, item.category,
                                item.count);
                    log_exit_trace_locked("extension_item_click", event, param, param2, x, y);
                    // The module consumed the click; still clear native selection state.
                    release_cleanup_handled = false;
                } else {
                    const bool handled = g_extension_drag.active &&
                                         handle_bag_drop_release_locked(lock, x, y);
                    // Drop keeps the transaction result; failed/no-source drops use unhandled cleanup.
                    release_cleanup_handled = handled;
                }
                g_extension_drag = {};
                g_extension_touch_capture = false;
                log_p5_observation_locked("panel-capture-release", nullptr, event, param2 != 0,
                                          param != 0, nullptr, true, 1, x, y);
                release_cleanup_needed = true;
            }
        } else if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal &&
            handle_bag_drop_release_locked(lock, x, y)) {
            log_p5_observation_locked("panel-original-release-handled", nullptr, event,
                                      param2 != 0, param != 0, nullptr, true, 1, x, y);
            // Original-bag drop was consumed by the module route; skip selected-control clearing.
            release_cleanup_needed = true;
            release_cleanup_handled = true;
        }
        }
        if (release_cleanup_needed) {
            complete_original_release_cleanup_locked(nullptr, x, y, param2, release_cleanup_handled);
            return 1;
        }
        // G-10：tab 点击的坐标兜底已删除——tab 点击由控件回调
        // （extension_tab_button_clicked → queue_extension_tab_click）唯一处理，
        // 消除坐标常量与控件树的双路径分歧。
        // Phase one is deliberately read-only. Do not route release events to
        // the legacy projection/move transaction handlers.
    }
    if (event == 0x19) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else {
            void* moving = nullptr;
            void* drop_source = nullptr;
            (void)extension_bag_ui_touch_state(&moving, &drop_source, &x, &y);
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (g_extension_touch_capture) {
            // 0x19 is the native MOVE event. Keep the source and capture
            // alive until 0x18 performs click-vs-drop classification.
            if (g_extension_drag.active) {
                g_extension_drag.current_x = x;
                g_extension_drag.current_y = y;
            }
            log_p5_observation_locked("panel-capture-move", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
    }
    if (g_extension_touch_capture && event != 0x17 && event != 0x18 && event != 0x19) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_extension_drag = {};
        g_extension_touch_capture = false;
        reset_drag_state_locked(nullptr);
        log_exit_trace_locked("event_cancel", event, param, param2, x, y);
        log_p5_observation_locked("panel-capture-cancel", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, 1, x, y);
        return 1;
    }
    if (g_extension_touch_capture) {
        // Consume every event in the captured sequence, not only release and cancel.
        if (p5_panel_observation_enabled()) {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            log_p5_observation_locked("panel-capture-consume", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
        }
        return 1;
    }
    if (event != 0x17) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_enter", event, param, param2);
    }
    if (exiting_to_original) {
        const uint64_t original_result = call_original_event_with_inventory_guard(event, param, param2);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("original_press_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-original-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, original_result, x, y);
        return original_result;
    }
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_pre_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-pre", nullptr, event, param2 != 0,
                                  param != 0, nullptr, false, 0, x, y);
    }
    const uint64_t result = call_original_event_with_inventory_guard(event, param, param2);
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, result, x, y);
        // 0x17/0x19 只负责原版 TouchHandle 的 press/move 握手，不提交扩展事务。
        // 投影 session 的 0x18 已在 delegate 前返回，不能在这里再出现第二个 owner。
        if (event == 0x17 || event == 0x19) advance_projected_drag_session_locked();
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
            if (event == 0x18 && !complete_original_exit_locked()) {
                cancel_original_exit_locked();
            }
            return result;
        }
        // 物品格点击也会产生 0x17；原版袋按钮已在上方单独处理。
        // 这里仅处理原版事件确实改写当前袋的兜底退出路径。
        const bool original_bag_changed = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                                          original_bag_locked() != g_original_current_got;
        if (event == 0x17 && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            original_bag_changed) {
            const int selected_original_bag = original_bag_locked();
            restore_module_view_locked();
            set_original_bag_locked(selected_original_bag);
            if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
                virtual_bag_call_original_refresh_item_area_with_guard();
            }
            virtual_bag::enter_original(&g_virtual_bag_state, selected_original_bag);
            persist_state_locked();
            g_exit_display_bag = kNoOriginalBagSelected;
        } else if (event == 0x17 || event == 0x18) {
            g_virtual_bag_state.original_selected = original_bag_locked();
        }
    }
    return result;
}

// 生命周期节拍（D2a-3c）：原 150ms 轮询线程体的主线程化，改由 infra::scheduler 帧任务
// （kLogicPre，节流 30 帧，count=0 常驻）。锁纪律与原线程逐点一致：g_virtual_bag_mtx
// 短临界区包住 prepare_main_menu_locked；R-44 不变量不变。
int32_t extension_bag_lifecycle_tick(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    int world_state = -1;
    if (!read_world_state_id(&world_state)) return 1;
    const bool due = qol::features::extension_bag::model::main_menu_cleanup_due(
        g_lifecycle_last_state, world_state, true,
        g_main_menu_cleanup_dirty.load(std::memory_order_acquire));
    if (due) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!extension_bag_session_admitted_locked()) {
            // An unloaded or degraded slot is not an empty inventory that may be reset.
            g_main_menu_cleanup_dirty.store(false, std::memory_order_release);
        } else if (!prepare_main_menu_locked()) {
            g_main_menu_cleanup_dirty.store(true, std::memory_order_release);
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "lifecycle main-menu cleanup failed; retry scheduled");
        } else {
            g_main_menu_cleanup_dirty.store(false, std::memory_order_release);
        }
    }
    g_lifecycle_last_state = world_state;
    return 1;
}

void ensure_lifecycle_task() {
    if (g_lifecycle_task != 0) return;
    g_lifecycle_last_state = -1;
    if (g_sdk == nullptr || g_sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    const QolResult result = g_sdk->scheduler.add(1, &extension_bag_lifecycle_tick, nullptr, 30, 0,
                                                  &task);
    if (result.ok == kQolOk) g_lifecycle_task = task;
}

void remove_lifecycle_task() {
    if (g_lifecycle_task == 0) return;
    if (g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_lifecycle_task);
    }
    g_lifecycle_task = 0;
}

bool prepare_main_menu_locked() {
    if (!extension_bag_session_admitted_locked()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "prepare main-menu rejected: slot data session not admitted");
        return false;
    }
    if (!module_cache_clear_preflight_locked()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "prepare main-menu preflight failed");
        return false;
    }
    store_teardown_locked();
    if (g_module_view_installed) restore_module_view_locked();
    if (!clear_module_cache_locked()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "prepare main-menu blocked by active module object");
        return false;
    }
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    g_item_state_dirty = false;
    g_inventory_frame_active = false;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
    cancel_projected_drag_session_locked("main-menu");
    return true;
}
