uintptr_t arm64_bl_target(uintptr_t call_addr, uint32_t instruction) {
    int64_t immediate = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((immediate & 0x02000000LL) != 0) immediate |= ~0x03ffffffLL;
    return static_cast<uintptr_t>(static_cast<int64_t>(call_addr) + (immediate << 2));
}

struct SaveCallsiteSpec {
    uintptr_t offset;
    uint32_t original_instruction;
    const char* name;
};

bool patch_save_callsite(const SaveCallsiteSpec& spec, size_t index) {
    if (g_save_callsite_patch_addrs[index] != 0) return true;
    const uintptr_t call_addr = g_base + spec.offset;
    constexpr size_t kPageSize = 4096;
    const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&module_save_game);
    const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
    uintptr_t branch_target = wrapper;
    if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
        branch_target = reinterpret_cast<uintptr_t>(allocate_draw_thunk(call_addr, wrapper));
        if (branch_target == 0) {
            VIRTBAG_LOG("save callsite thunk allocation failed name=%s call=%p", spec.name,
                        reinterpret_cast<void*>(call_addr));
            return false;
        }
    }
    const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
    if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
        VIRTBAG_LOG("save callsite branch out of range name=%s call=%p target=%p", spec.name,
                    reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(branch_target));
        return false;
    }
    const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        VIRTBAG_LOG("save callsite mprotect failed name=%s errno=%d", spec.name, errno);
        return false;
    }
    const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
    if (current != spec.original_instruction && current != replacement) {
        VIRTBAG_LOG("save callsite mismatch name=%s got=0x%08x", spec.name, current);
        return false;
    }
    *reinterpret_cast<uint32_t*>(call_addr) = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                             reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
    g_save_callsite_patch_addrs[index] = call_addr;
    VIRTBAG_LOG("save callsite hooked name=%s call=%p", spec.name,
                reinterpret_cast<void*>(call_addr));
    return true;
}

bool patch_all_save_callsites() {
    constexpr SaveCallsiteSpec specs[] = {
        {F_SAVE_CALLSITE_REVIVE_VMA, 0x9401945e, "revive"},
        {F_SAVE_CALLSITE_QUEST_CLEAR_1_VMA, 0x940175d8, "quest-clear-1"},
        {F_SAVE_CALLSITE_QUEST_CLEAR_2_VMA, 0x940175ad, "quest-clear-2"},
        {F_SAVE_CALLSITE_QUEST_REVIEW_VMA, 0x94000e5e, "quest-review"},
        {F_SAVE_CALLSITE_PROCESS_VMA, 0x97ffff6c, "process-save"},
        {F_SAVE_CALLSITE_NETWORK_ADD_VMA, 0x97ff2fbe, "network-add"},
        {F_SAVE_CALLSITE_NETWORK_PROCESS_1_VMA, 0x97ff2edf, "network-process-1"},
        {F_SAVE_CALLSITE_NETWORK_PROCESS_2_VMA, 0x97ff2e44, "network-process-2"},
    };
    for (size_t index = 0; index < sizeof(specs) / sizeof(specs[0]); ++index) {
        if (!patch_save_callsite(specs[index], index)) return false;
    }
    return true;
}

struct PhysicalInventorySnapshot {
    std::array<void*, virtual_bag::kPhysicalInventorySnapshotSlots> slots{};
    void* inventory = nullptr;
    uint64_t digest = 0;
    int nonnull = 0;
    bool valid = false;
};

PhysicalInventorySnapshot capture_physical_inventory_snapshot_locked() {
    PhysicalInventorySnapshot snapshot{};
    snapshot.inventory = g_inven;
    snapshot.digest = 1469598103934665603ULL;
    if (g_inven == nullptr) return snapshot;
    void** inventory = static_cast<void**>(g_inven);
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        void* item = inventory[index];
        snapshot.slots[index] = item;
        if (item != nullptr) ++snapshot.nonnull;
        snapshot.digest ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(item));
        snapshot.digest *= 1099511628211ULL;
    }
    snapshot.valid = true;
    return snapshot;
}

bool restore_physical_inventory_snapshot_locked(const PhysicalInventorySnapshot& snapshot) {
    if (!snapshot.valid || g_inven == nullptr || g_inven != snapshot.inventory) return false;
    void** inventory = static_cast<void**>(g_inven);
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        inventory[index] = snapshot.slots[index];
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
            VIRTBAG_LOG("ERROR original event changed physical inventory while projected session active "
                        "event=0x%llx before_digest=0x%llx after_digest=0x%llx",
                        static_cast<unsigned long long>(event),
                        static_cast<unsigned long long>(before.digest),
                        static_cast<unsigned long long>(after.digest));
            for (size_t index = 0; index < before.slots.size(); ++index) {
                if (before.slots[index] == after.slots[index]) continue;
                const int bag = static_cast<int>(index / virtual_bag::kSlotCount);
                const int slot = static_cast<int>(index % virtual_bag::kSlotCount);
                VIRTBAG_LOG("ERROR physical inventory mutation bag=%d slot=%d old=%p new=%p",
                            bag, slot, before.slots[index], after.slots[index]);
            }
            restored = restore_physical_inventory_snapshot_locked(before);
            VIRTBAG_LOG("ERROR physical inventory rollback event=0x%llx restored=%d",
                        static_cast<unsigned long long>(event), restored ? 1 : 0);
        }
    }
    if (restored) {
        const bool synced = virtual_bag_sync_projected_bag();
        VIRTBAG_LOG("ERROR physical inventory rollback projection_sync=%d", synced ? 1 : 0);
    }
    return result;
}

bool consume_projected_release_locked(std::unique_lock<std::mutex>& lock, int64_t x, int64_t y) {
    if (!projected_drag_session_active_locked()) return false;
    auto& session = g_extension_drag_session;
    if (session.phase == virtual_bag::DragPhase::kCommitted ||
        session.phase == virtual_bag::DragPhase::kRejected ||
        session.phase == virtual_bag::DragPhase::kCancelled) {
        VIRTBAG_LOG("projected release consumed terminal session event=0x18 token=%llu phase=%d",
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
        routed = route_deferred_extension_to_original_locked();
    } else {
        routed = route_projected_session_drop_locked(lock, x, y);
    }
    if (session.phase != virtual_bag::DragPhase::kCommitted &&
        session.phase != virtual_bag::DragPhase::kRejected &&
        session.phase != virtual_bag::DragPhase::kCancelled) {
        cancel_projected_drag_session_locked("release-intercepted-without-target");
    }
    VIRTBAG_LOG("projected release intercepted event=0x18 source=%d/%d target_original=%d routed=%d",
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
    const uint32_t gamestate = g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
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
                    } else if (g_base != 0) {
                        const uint8_t* touch_state =
                            reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
                        release_x = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_X);
                        release_y = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_Y);
                    }
                    cancel_projected_drag_session_locked("release-bypassed-gamestate");
                    g_extension_drag_session = {};
                    VIRTBAG_LOG("projected release intercepted event=0x18 reason=gamestate=%u",
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
            VIRTBAG_LOG("touch bypass event=0x%llx enabled=%d gamestate=%u base=0x%llx hooked=%d",
                        static_cast<unsigned long long>(event), extension_enabled ? 1 : 0,
                        gamestate, static_cast<unsigned long long>(g_base), g_state_entry != nullptr ? 1 : 0);
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
        VIRTBAG_LOG("touch bounds event=0x%llx x=%lld y=%lld grid=%lld,%lld %lldx%lld grid_hit=%d tab_origin=%lld,%lld mode=%d selected=%d overlay=%d root=%p root_gen=%llu state_gen=%llu",
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
        } else if (g_base != 0) {
            const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            x = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_X);
            y = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_Y);
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
                    VIRTBAG_LOG("extension item click bag=%d slot=%d category=%d count=%d",
                                g_extension_drag.bag, g_extension_drag.slot, item.category,
                                item.count);
                    log_exit_trace_locked("extension_item_click", event, param, param2, x, y);
                    // The module consumed the click; still clear native selection state.
                    release_cleanup_handled = false;
                } else {
                    const bool handled = g_extension_drag.active &&
                                         handle_bag_drop_release_locked(x, y);
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
            handle_bag_drop_release_locked(x, y)) {
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
        } else if (g_base != 0) {
            const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            x = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_X);
            y = *reinterpret_cast<const int64_t*>(touch_state + TOUCH_STATE_RELEASE_Y);
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
            if (fn_ui_equip_refresh_item_area != nullptr) {
                inventory_native_hook_call_refresh_item_area_original();
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

bool inject_locked() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!g_virtual_bag_enabled.load()) return false;
    if (!set_extension_source_protection_enabled(true)) {
        VIRTBAG_LOG("extension source protection install pending base=0x%llx",
                    static_cast<unsigned long long>(g_base));
        return false;
    }
    const bool install_inventory_hook = g_state_entry == nullptr;
    uint8_t* entry = g_state_entry;
    if (install_inventory_hook) {
        entry = find_inventory_state_entry();
        if (entry == nullptr) return false;
        const uintptr_t expected_event =
            g_base + fn_resolve("F_SCENE_EVENT_EQUIP_VMA", F_SCENE_EVENT_EQUIP_VMA);
        if (*reinterpret_cast<uintptr_t*>(entry + 0x38) != expected_event) {
            VIRTBAG_LOG("inventory state callbacks differ from expected symbols");
            return false;
        }
        g_orig_event = reinterpret_cast<PopupEventFn>(*reinterpret_cast<uintptr_t*>(entry + 0x38));
        g_orig_f3 = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x28));
        g_orig_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x10));
    }

    const uintptr_t equip_draw = g_base + fn_resolve("F_UIEQUIP_DRAW_VMA", F_UIEQUIP_DRAW_VMA);
    const uintptr_t item_draw_call = equip_draw + 0x94;
    constexpr uint32_t kOriginalItemDrawCall = 0x97fffe33;
    constexpr size_t kPageSize = 4096;
    if (g_item_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_inven_item_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(item_draw_call);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_item_draw_thunk = allocate_draw_thunk(item_draw_call, wrapper);
            if (g_item_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory item draw thunk allocation failed");
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_item_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(item_draw_call);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory item draw target out of range");
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = item_draw_call & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory item draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(item_draw_call);
        if (current != kOriginalItemDrawCall && current != replacement) {
            VIRTBAG_LOG("inventory item draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(item_draw_call) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(item_draw_call),
                                 reinterpret_cast<char*>(item_draw_call + sizeof(uint32_t)));
        g_item_draw_patch_addr = item_draw_call;
        VIRTBAG_LOG("inventory item draw hook patched replacement=0x%08x", replacement);
    }

    const uintptr_t bag_draw_call = equip_draw + 0x98;
    constexpr uint32_t kOriginalBagDrawCall = 0x97fffee8;
    if (g_bag_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_original_bag_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(bag_draw_call);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_bag_draw_thunk = allocate_draw_thunk(bag_draw_call, wrapper);
            if (g_bag_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory bag draw thunk allocation failed");
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_bag_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(bag_draw_call);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory bag draw target out of range");
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = bag_draw_call & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory bag draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(bag_draw_call);
        if (current != kOriginalBagDrawCall && current != replacement) {
            VIRTBAG_LOG("inventory bag draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(bag_draw_call) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(bag_draw_call),
                                reinterpret_cast<char*>(bag_draw_call + sizeof(uint32_t)));
        g_bag_draw_patch_addr = bag_draw_call;
        VIRTBAG_LOG("inventory bag highlight hook patched replacement=0x%08x", replacement);
    }

    const uintptr_t call_addr = g_base + fn_resolve("F_SCENE_DRAW_EQUIP_VMA", F_SCENE_DRAW_EQUIP_VMA) + 0x210;
    constexpr uint32_t kOriginalEndCall = 0x97fd11d2;
    if (g_draw_patch_addr == 0) {
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&virtual_bag_draw_end_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_draw_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_draw_thunk == nullptr) {
                VIRTBAG_LOG("inventory draw wrapper out of BL range and thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(wrapper));
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_draw_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("inventory draw branch target out of BL range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), reinterpret_cast<void*>(branch_target));
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("inventory draw patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalEndCall && current != replacement) {
            VIRTBAG_LOG("inventory draw patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr), reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_draw_patch_addr = call_addr;
        VIRTBAG_LOG("inventory draw restore hook patched call=%p replacement=0x%08x", reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_save_inventory_patch_addr == 0) {
        const uintptr_t call_addr =
            g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_CALLSITE_VMA",
                                F_SAVE_SAVE_INVENTORY_CALLSITE_VMA);
        constexpr uint32_t kOriginalSaveCall = 0x97fff987;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&save_inventory_wrapper);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_save_inventory_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_save_inventory_thunk == nullptr) {
                VIRTBAG_LOG("save gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_save_inventory_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("save gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("save gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalSaveCall && current != replacement) {
            VIRTBAG_LOG("save gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_save_inventory_patch_addr = call_addr;
        VIRTBAG_LOG("save gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (!patch_all_save_callsites()) return false;

    if (g_drop_gate_patch_addr == 0) {
        const uintptr_t call_addr = g_base + 0xb8cc0;
        constexpr uint32_t kOriginalDropCall = 0x94012fc8;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&save_item_on_empty_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_drop_gate_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_drop_gate_thunk == nullptr) {
                VIRTBAG_LOG("drop gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_drop_gate_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("drop gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("drop gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDropCall && current != replacement) {
            VIRTBAG_LOG("drop gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_drop_gate_patch_addr = call_addr;
        VIRTBAG_LOG("drop gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_draw_gate_patch_addr == 0) {
        const uintptr_t call_addr = g_base + 0xaaf28;
        constexpr uint32_t kOriginalDrawCall = 0x94016d49;
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&item_draw_porting_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_draw_gate_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_draw_gate_thunk == nullptr) {
                VIRTBAG_LOG("draw gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_draw_gate_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("draw gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("draw gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDrawCall && current != replacement) {
            VIRTBAG_LOG("draw gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_draw_gate_patch_addr = call_addr;
        VIRTBAG_LOG("draw gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }

    if (g_desc_open_patch_addr == 0) {
        // InvenItemControlEventProc 事件 0x80（物品 desc 打开）调 MakeDesc 的
        // 唯一 bl：包装后在 desc 菜单上装装备按钮 hook（Path A 装备溢出）。
        const uintptr_t call_addr =
            g_base + fn_resolve("F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA",
                                F_UIEQUIP_ITEM_DESC_MAKE_DESC_CALL_VMA);
        constexpr uint32_t kOriginalDescOpenCall = 0x97fffdfe;  // bl 0xb8980（imm26=-0x202，真机核对）
        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(&make_desc_equip_gate);
        const int64_t direct_delta = static_cast<int64_t>(wrapper) - static_cast<int64_t>(call_addr);
        uintptr_t branch_target = wrapper;
        if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL || direct_delta >= 0x08000000LL) {
            g_desc_open_thunk = allocate_draw_thunk(call_addr, wrapper);
            if (g_desc_open_thunk == nullptr) {
                VIRTBAG_LOG("desc open gate thunk allocation failed call=%p wrapper=%p",
                            reinterpret_cast<void*>(call_addr), wrapper);
                return false;
            }
            branch_target = reinterpret_cast<uintptr_t>(g_desc_open_thunk);
        }
        const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
        if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) {
            VIRTBAG_LOG("desc open gate branch target out of range call=%p target=%p",
                        reinterpret_cast<void*>(call_addr), branch_target);
            return false;
        }
        const uint32_t replacement = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
        const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            VIRTBAG_LOG("desc open gate patch mprotect failed errno=%d", errno);
            return false;
        }
        const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
        if (current != kOriginalDescOpenCall && current != replacement) {
            VIRTBAG_LOG("desc open gate patch mismatch got=0x%08x", current);
            return false;
        }
        *reinterpret_cast<uint32_t*>(call_addr) = replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                                reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
        g_desc_open_patch_addr = call_addr;
        VIRTBAG_LOG("desc open gate hook patched call=%p replacement=0x%08x",
                    reinterpret_cast<void*>(call_addr), replacement);
    }
    if (install_inventory_hook) {
        *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&virtual_bag_f3_wrapper);
        *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&virtual_bag_event);
        *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&virtual_bag_inventory_enter_wrapper);
        g_state_entry = entry;
        VIRTBAG_LOG("inventory event callback wrapped; original renderer is reused for module views");
    }
    if (!g_save_panel_hook_installed) {
        uint8_t* save_entry = find_popup_state_entry(
            g_base + fn_resolve("F_PANEL_SAVE_SLOT_ENTER", F_PANEL_SAVE_SLOT_ENTER));
        if (save_entry != nullptr && save_entry != entry) {
            g_save_state_entry = save_entry;
            g_orig_save_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(save_entry + 0x10));
            *reinterpret_cast<uintptr_t*>(save_entry + 0x10) =
                reinterpret_cast<uintptr_t>(&virtual_bag_save_panel_enter_wrapper);
            g_save_panel_hook_installed = true;
            VIRTBAG_LOG("save panel enter hook installed entry=%p", save_entry);
        } else {
            VIRTBAG_LOG("save panel entry not ready; injection will retry");
        }
    }
    if (!install_store_hooks_locked()) return false;
    return true;
}

void ensure_inject_thread() {
    if (g_inject_thread_started.exchange(true)) return;
    std::thread([]() {
        while (g_state_entry == nullptr || !g_save_panel_hook_installed ||
               !store_hooks_installed()) {
            inject_locked();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

void ensure_lifecycle_thread() {
    if (g_lifecycle_thread_started.exchange(true)) return;
    std::thread([]() {
        int last_state = -1;
        for (;;) {
            if (g_state != nullptr) {
                const int state = static_cast<int>(*reinterpret_cast<uint16_t*>(g_state));
                if (last_state == 5 && state != 5) {
                    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
                    if (!prepare_main_menu_locked()) {
                        g_main_menu_cleanup_dirty.store(true, std::memory_order_release);
                        VIRTBAG_LOG("lifecycle main-menu cleanup failed; retry scheduled");
                    } else {
                        g_main_menu_cleanup_dirty.store(false, std::memory_order_release);
                    }
                } else if (state != 5 &&
                           g_main_menu_cleanup_dirty.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
                    if (prepare_main_menu_locked()) {
                        g_main_menu_cleanup_dirty.store(false, std::memory_order_release);
                        VIRTBAG_LOG("lifecycle main-menu cleanup retry succeeded");
                    } else {
                        VIRTBAG_LOG("lifecycle main-menu cleanup retry failed");
                    }
                }
                last_state = state;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

bool prepare_main_menu_locked() {
    if (!module_cache_clear_preflight_locked()) {
        VIRTBAG_LOG("prepare main-menu preflight failed");
        return false;
    }
    store_teardown_locked();
    if (g_module_view_installed) restore_module_view_locked();
    if (!clear_module_cache_locked()) {
        VIRTBAG_LOG("prepare main-menu blocked by active module object");
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
