uintptr_t arm64_bl_target(uintptr_t call_addr, uint32_t instruction) {
    int64_t immediate = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((immediate & 0x02000000LL) != 0) immediate |= ~0x03ffffffLL;
    return static_cast<uintptr_t>(static_cast<int64_t>(call_addr) + (immediate << 2));
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
        if (event == 0x17 || event == 0x18 || event == 0x19) {
            VIRTBAG_LOG("touch bypass event=0x%llx enabled=%d gamestate=%u base=0x%llx hooked=%d",
                        static_cast<unsigned long long>(event), extension_enabled ? 1 : 0,
                        gamestate, static_cast<unsigned long long>(g_base), g_state_entry != nullptr ? 1 : 0);
        }
        const uint64_t result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
        if (p5_panel_observation_enabled()) {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
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
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("event_release", event, param, param2, x, y);
        if (g_extension_touch_capture) {
            if (param == 0 && x == 0 && y == 0 && g_extension_drag.active) {
                x = g_extension_drag.current_x;
                y = g_extension_drag.current_y;
            }
            if (param == 0 && x == 0 && y == 0) {
                g_extension_drag = {};
                g_extension_touch_capture = false;
                reset_drag_state_locked(nullptr);
                log_p5_observation_locked("panel-capture-release-empty", nullptr, event,
                                          param2 != 0, param != 0, nullptr, true, 1, x, y);
                return 1;
            }
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
            } else {
                const bool handled = g_extension_drag.active &&
                                     handle_bag_drop_release_locked(x, y);
                (void)handled;
            }
            reset_drag_state_locked(nullptr);
            g_extension_drag = {};
            g_extension_touch_capture = false;
            log_p5_observation_locked("panel-capture-release", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal &&
            handle_bag_drop_release_locked(x, y)) {
            log_p5_observation_locked("panel-original-release-handled", nullptr, event,
                                      param2 != 0, param != 0, nullptr, true, 1, x, y);
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
        const uint64_t original_result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
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
    const uint64_t result = g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        log_exit_trace_locked("delegate_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, result, x, y);
        if (event == 0x17 || event == 0x19) advance_projected_drag_session_locked();
        if (event == 0x18 && g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
            // TouchHandle releases on the original bag tab without dispatching
            // the item-proc callback used by SaveItemOnEmpty. Resolve that
            // coordinate here, after the native callback has returned, so the
            // deferred ext->orig transaction still follows the real drag path.
            const int original_target_bag = original_bag_button_index(x, y);
            if (original_target_bag >= 0 &&
                virtual_bag::valid_original_transaction_bag(original_target_bag)) {
                g_extension_drag_session.target_kind =
                    virtual_bag::DragTargetKind::kOriginalBag;
                g_extension_drag_session.target_bag = original_target_bag;
                g_extension_drag_session.target_slot = -1;
                VIRTBAG_LOG("p5 session original tab resolved x=%lld y=%lld target_bag=%d",
                            static_cast<long long>(x), static_cast<long long>(y),
                            original_target_bag);
            }
            if (g_extension_drag_session.target_kind ==
                virtual_bag::DragTargetKind::kOriginalBag) {
                route_deferred_extension_to_original_locked();
            } else if (g_extension_drag_session.phase == virtual_bag::DragPhase::kPressed ||
                g_extension_drag_session.phase == virtual_bag::DragPhase::kNativeMoving ||
                g_extension_drag_session.phase == virtual_bag::DragPhase::kTargetResolved) {
                route_projected_session_drop_locked(x, y);
            }
            if (g_extension_drag_session.phase != virtual_bag::DragPhase::kCommitted &&
                g_extension_drag_session.phase != virtual_bag::DragPhase::kRejected &&
                g_extension_drag_session.phase != virtual_bag::DragPhase::kCancelled) {
                cancel_projected_drag_session_locked("release-without-target");
            }
            g_extension_drag_session = {};
        }
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
            if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
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
    if (g_state_entry != nullptr) return true;
    uint8_t* entry = find_inventory_state_entry();
    if (entry == nullptr) return false;
    const uintptr_t expected_event = g_base + fn_resolve("F_SCENE_EVENT_EQUIP_VMA", F_SCENE_EVENT_EQUIP_VMA);
    if (*reinterpret_cast<uintptr_t*>(entry + 0x38) != expected_event) {
        VIRTBAG_LOG("inventory state callbacks differ from expected symbols");
        return false;
    }
    g_orig_event = reinterpret_cast<PopupEventFn>(*reinterpret_cast<uintptr_t*>(entry + 0x38));
    g_orig_f3 = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x28));
    g_orig_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(entry + 0x10));

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
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&virtual_bag_f3_wrapper);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&virtual_bag_event);
    *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&virtual_bag_inventory_enter_wrapper);
    uint8_t* save_entry = find_popup_state_entry(
        g_base + fn_resolve("F_PANEL_SAVE_SLOT_ENTER", F_PANEL_SAVE_SLOT_ENTER));
    if (save_entry != nullptr && save_entry != entry) {
        g_save_state_entry = save_entry;
        g_orig_save_enter = reinterpret_cast<PopupNoArgFn>(*reinterpret_cast<uintptr_t*>(save_entry + 0x10));
        *reinterpret_cast<uintptr_t*>(save_entry + 0x10) =
            reinterpret_cast<uintptr_t>(&virtual_bag_save_panel_enter_wrapper);
    }
    g_state_entry = entry;
    VIRTBAG_LOG("inventory event callback wrapped; original renderer is reused for module views");
    return true;
}

void ensure_inject_thread() {
    if (g_inject_thread_started.exchange(true)) return;
    std::thread([]() {
        while (g_state_entry == nullptr) {
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
                    prepare_main_menu_locked();
                }
                last_state = state;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }).detach();
}

void prepare_main_menu_locked() {
    if (g_module_view_installed) restore_module_view_locked();
    clear_module_cache_locked();
    g_virtual_bag_state = {};
    g_loaded_slot = -2;
    g_item_state_dirty = false;
    g_inventory_frame_active = false;
    disable_extension_tab_buttons_locked();
    g_extension_tab_buttons.fill(nullptr);
    cancel_projected_drag_session_locked("main-menu");
}

