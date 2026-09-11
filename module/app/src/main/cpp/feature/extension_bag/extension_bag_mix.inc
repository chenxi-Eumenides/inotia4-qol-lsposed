// 合成器（UIMix）宿主：只管理 UIMix 自己的页签、投影和刷新，不复用 UIEquip/商店视图
// 状态。结构参考 extension_bag_store.inc：
//   - PopupState ENTER/F3 回调包装（enter 装页签、f3 teardown）；
//   - Scene_Draw_POPUP_SC_MIX 内 bl GRPX_End 调用点 BL patch 挂 draw_end wrapper；
//   - 自建 5 个扩展袋页签挂到 UIMix 袋组 [g_uimix+UIMIX_SLOT_BAG_GROUP]（原版 6 袋按钮同父）；
//   - 投影安装/恢复写 UIMix 物品网格组 [g_uimix+UIMIX_SLOT_ITEM_GROUP] 的 16 个子控件。
// 宿主物理袋沿用 original_bag_locked()（UIMix 与 UIEquip 共用 [0x2f56d8] 当前袋 GOT，
// UIMix_RefreshInvenItem 反汇编确认），合法 0..4；落在任务袋 5/非法时回退 0。

using MixSceneNoArgFn = void (*)();

bool mix_view_installed = false;
int mix_view_bag = -1;
int mix_window_original_bag = -1;
uint32_t* mix_original_size_word = nullptr;
uint32_t mix_original_bag_size = 0;
void* mix_projected_item_root = nullptr;
std::array<void*, virtual_bag::kBagCount> mix_tab_buttons{};
std::array<void*, virtual_bag::kBagCount> mix_tab_bag_items{};
void* mix_tab_root = nullptr;
uint64_t mix_tab_generation = 0;
int mix_pending_tab = -1;
uint64_t mix_pending_tab_generation = 0;
bool mix_frame_active = false;

MixSceneNoArgFn mix_original_enter = nullptr;
MixSceneNoArgFn mix_original_f3 = nullptr;
uint8_t* mix_state_entry = nullptr;
uintptr_t mix_draw_patch_addr = 0;
void* mix_draw_thunk = nullptr;
uintptr_t mix_bag_draw_patch_addr = 0;
void* mix_bag_draw_thunk = nullptr;
bool mix_hooks_ready = false;

uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*);
void mix_install_module_view_locked(int bag);
void mix_restore_module_view_locked();

void* mix_bag_container_locked() {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_BAG_GROUP);
}

void* mix_item_root_locked() {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_ITEM_GROUP);
}

// 面板状态：0=配方菜单（原版不画背包列），1=背包/合成视图。扩展页签只在 state!=0
// 时挂载/绘制，避免在配方菜单阶段早于原版背包出现（UIMix_Draw 状态门控反汇编确认）。
int mix_state_locked() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STATE);
}

bool mix_hooks_installed() {
    return mix_hooks_ready;
}

int mix_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (mix_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void mix_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const uint8_t type = g_virtual_bag_state.types[index];
        void*& item = mix_tab_bag_items[index];
        if (type == 0) {
            if (item != nullptr) {
                if (defer_item_free_locked(item)) item = nullptr;
            }
            continue;
        }
        if (item != nullptr && fn_get_bit != nullptr) {
            const uint16_t flags = *reinterpret_cast<uint16_t*>(
                reinterpret_cast<uint8_t*>(item) + I_TYPE);
            if (fn_get_bit(flags, 15, 6) != static_cast<int>(type)) {
                if (defer_item_free_locked(item)) item = nullptr;
            }
        }
        if (item == nullptr && fn_create_item != nullptr) {
            item = fn_create_item(static_cast<int32_t>(type), 0, 0, 0);
            if (item != nullptr) {
                const uint32_t flags = *reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uint8_t*>(item) + I_COUNT);
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
                    stack_codec::write_native_bag_object_marker(flags);
            }
        }
    }
}

void mix_disable_tab_buttons_locked() {
    ++mix_tab_generation;
    mix_pending_tab = -1;
    mix_pending_tab_generation = 0;
    mix_tab_buttons.fill(nullptr);
    mix_tab_root = nullptr;
}

void mix_delete_tab_buttons_locked() {
    if (fn_touch_handle_delete_control != nullptr) {
        for (void*& button : mix_tab_buttons) {
            if (button != nullptr) fn_touch_handle_delete_control(button);
            button = nullptr;
        }
    } else {
        mix_tab_buttons.fill(nullptr);
    }
}

void mix_install_tab_buttons_locked() {
    if (g_base == 0 || g_uimix == nullptr || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr || fn_mem_malloc == nullptr ||
        fn_control_object_set_control_proc == nullptr ||
        fn_control_object_set_user_type == nullptr ||
        fn_touch_handle_unuse_control_event_move == nullptr ||
        fn_touch_handle_delete_control == nullptr || fn_ctrl_get_count == nullptr ||
        fn_ctrl_set_data == nullptr) {
        return;
    }
    void* bag_container = mix_bag_container_locked();
    if (bag_container == nullptr || fn_ctrl_get_count(bag_container) < 6) return;
    if (mix_tab_root == bag_container && mix_tab_buttons[0] != nullptr) return;

    if (mix_tab_root != nullptr) mix_delete_tab_buttons_locked();
    mix_refresh_tab_bag_items_locked();

    int64_t bag0_abs_x = 0;
    int64_t bag0_abs_y = 0;
    void* bag0 = valid_child_locked(bag_container, 0);
    if (bag0 == nullptr) return;
    extension_bag_control_abs_pos(bag0, &bag0_abs_x, &bag0_abs_y);
    const uint8_t* bag0_bytes = static_cast<const uint8_t*>(bag0);
    const int64_t bag0_width = *reinterpret_cast<const int64_t*>(bag0_bytes + CO_RECT_W);
    if (bag0_width <= 0) return;

    int64_t mount_abs_x = 0;
    int64_t mount_abs_y = 0;
    extension_bag_control_abs_pos(bag_container, &mount_abs_x, &mount_abs_y);
    // 沿用扩展页/商店的原版袋列右侧间距 68、下移 2；起点来自 UIMix 实时袋 0 rect。
    const int64_t tab_rel_x = bag0_abs_x + 68 - mount_abs_x;
    const int64_t tab_rel_y = bag0_abs_y + 2 - mount_abs_y;
    const uintptr_t add_raw = g_base + fn_resolve(
        "F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA", F_CONTROL_OBJECT_ADD_CONTROL_OBJECT_VMA);
    using AddControlFn = void* (*)(void*, void*, void*, uint32_t, void*);
    void* add_proc = reinterpret_cast<void*>(g_base + fn_resolve(
        "F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA", F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA));
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* control = reinterpret_cast<AddControlFn>(add_raw)(
            bag_container, nullptr, nullptr, 3, add_proc);
        if (control == nullptr) continue;
        fn_control_object_set_control_proc(control,
            reinterpret_cast<void*>(&mix_tab_item_proc));
        fn_control_object_set_user_type(control, 2);
        fn_touch_handle_unuse_control_event_move(control);
        uint8_t* control_bytes = static_cast<uint8_t*>(control);
        *reinterpret_cast<int64_t*>(control_bytes + CO_RECT_X) = tab_rel_x;
        *reinterpret_cast<int64_t*>(control_bytes + CO_RECT_Y) = tab_rel_y + index * 70;
        *reinterpret_cast<int64_t*>(control_bytes + CO_RECT_W) = 57;
        *reinterpret_cast<int64_t*>(control_bytes + CO_RECT_H) = 57;
        void* data = fn_mem_malloc(0x10);
        if (data == nullptr) continue;
        std::memset(data, 0, 0x10);
        *reinterpret_cast<void**>(data) = mix_tab_bag_items[index];
        fn_ctrl_set_data(control, data);
        mix_tab_buttons[index] = control;
    }
    mix_tab_root = bag_container;
    ++mix_tab_generation;
    VIRTBAG_LOG("mix tabs installed root=%p bag0=(%lld,%lld) mount=(%lld,%lld) generation=%llu",
                bag_container, static_cast<long long>(bag0_abs_x),
                static_cast<long long>(bag0_abs_y), static_cast<long long>(mount_abs_x),
                static_cast<long long>(mount_abs_y),
                static_cast<unsigned long long>(mix_tab_generation));
}

uint64_t mix_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const int index = mix_tab_index(ctrl);
        if (index >= 0 && mix_tab_generation != 0) {
            mix_pending_tab = index;
            mix_pending_tab_generation = mix_tab_generation;
        }
        return 1;
    }
    // 合成器宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}

void mix_commit_pending_tab_locked() {
    if (!virtual_bag::valid_index(mix_pending_tab) ||
        mix_pending_tab_generation != mix_tab_generation) {
        mix_pending_tab = -1;
        mix_pending_tab_generation = 0;
        return;
    }
    const int bag = mix_pending_tab;
    mix_pending_tab = -1;
    mix_pending_tab_generation = 0;
    if (mix_view_installed && mix_view_bag == bag) return;
    mix_install_module_view_locked(bag);
}

void mix_refresh_projection_locked() {
    if (!mix_view_installed || mix_projected_item_root == nullptr ||
        fn_control_object_get_data == nullptr || fn_control_item_set_item == nullptr ||
        !virtual_bag::valid_index(mix_view_bag)) return;
    const int capacity = g_virtual_bag_state.capacities[mix_view_bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(mix_projected_item_root, slot);
        if (control == nullptr) continue;
        void* expected = module_item_locked(mix_view_bag, slot);
        void* data = fn_control_object_get_data(control);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != expected) fn_control_item_set_item(control, expected);
    }
}

void mix_install_module_view_locked(int bag) {
    if (g_base == 0 || g_uimix == nullptr || !virtual_bag::valid_index(bag) ||
        g_virtual_bag_state.capacities[bag] == 0 ||
        fn_control_item_set_item == nullptr || fn_control_object_get_child == nullptr ||
        fn_ui_mix_refresh_inven_item == nullptr) return;
    // UIMix 与 UIEquip 共用当前袋 GOT；落在任务袋 5/非法时回退原版袋 0。
    int host = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(host)) host = 0;
    uint32_t* size_word = bag_size_word_locked(host);
    if (size_word == nullptr) {
        VIRTBAG_LOG("mix view rejected host bag=%d", host);
        return;
    }
    if (mix_view_installed) mix_restore_module_view_locked();

    mix_window_original_bag = host;
    mix_original_size_word = size_word;
    mix_original_bag_size = *size_word;
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (*size_word & ~kCapacityMask) |
                 (static_cast<uint32_t>(g_virtual_bag_state.capacities[bag]) & kCapacityMask);
    set_original_bag_locked(host);

    mix_projected_item_root = mix_item_root_locked();
    if (mix_projected_item_root == nullptr) {
        *size_word = (*size_word & ~kCapacityMask) | (mix_original_bag_size & kCapacityMask);
        mix_original_size_word = nullptr;
        mix_original_bag_size = 0;
        mix_window_original_bag = -1;
        return;
    }
    fn_ui_mix_refresh_inven_item();
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(mix_projected_item_root, slot);
        if (control == nullptr) continue;
        void* item = module_item_locked(bag, slot);
        fn_control_item_set_item(control, item);
        if (item != nullptr) {
            ownership::borrow_for_view(&g_ownership_ledger,
                                       g_module_object_handles[bag][slot]);
        }
    }
    mix_view_bag = bag;
    mix_view_installed = true;
    VIRTBAG_LOG("mix view installed bag=%d window=%d capacity=%d", bag, host,
                g_virtual_bag_state.capacities[bag]);
}

void mix_restore_module_view_locked() {
    if (!mix_view_installed || g_base == 0) return;
    if (virtual_bag::valid_index(mix_view_bag)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = g_module_object_handles[mix_view_bag][slot];
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
        }
    }
    if (mix_original_size_word != nullptr) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        *mix_original_size_word = (*mix_original_size_word & ~kCapacityMask) |
                                  (mix_original_bag_size & kCapacityMask);
    }
    if (fn_ui_mix_refresh_inven_item != nullptr) fn_ui_mix_refresh_inven_item();
    mix_original_size_word = nullptr;
    mix_original_bag_size = 0;
    mix_window_original_bag = -1;
    mix_projected_item_root = nullptr;
    mix_view_bag = -1;
    mix_view_installed = false;
}

void mix_enter_wrapper() {
    if (mix_original_enter != nullptr) mix_original_enter();
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    mix_teardown_locked();
    // Enter 时面板处于 state 0（配方菜单，原版尚未画背包列）；页签等 state!=0 的
    // draw_end 再挂载，避免早于原版背包出现。
    mix_frame_active = true;
}

void mix_f3_wrapper() {
    MixSceneNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        mix_teardown_locked();
        mix_frame_active = false;
        original = mix_original_f3;
    }
    if (original != nullptr) original();
}

void mix_draw_tab_buttons_locked() {
    if (fn_grpx_draw_part == nullptr || fn_imgsys_get_group == nullptr ||
        fn_imgsys_get_loc == nullptr) return;
    void* group = fn_imgsys_get_group(0xf);
    if (group == nullptr) return;
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = mix_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        void* background = fn_imgsys_get_loc(0xf, 0x14);
        if (background != nullptr) {
            fn_grpx_draw_part(group, static_cast<int32_t>(x - 5),
                              static_cast<int32_t>(y - 12), background, 0, 1, 0);
        }
    }
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = mix_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = mix_view_installed && mix_view_bag == index;
        const bool equipped = g_virtual_bag_state.capacities[index] != 0;
        if (selected) {
            void* icon = fn_imgsys_get_loc(0xf, 0x13);
            if (icon != nullptr) fn_grpx_draw_part(group, static_cast<int32_t>(x - 5),
                                                   static_cast<int32_t>(y - 12), icon, 0, 1, 0);
            void* frame = fn_imgsys_get_loc(0xf, 6);
            if (frame != nullptr) fn_grpx_draw_part(group, static_cast<int32_t>(x + 5),
                                                    static_cast<int32_t>(y + 5), frame, 0, 1, 0);
        } else if (equipped) {
            void* frame = fn_imgsys_get_loc(0xf, 6);
            if (frame != nullptr) fn_grpx_draw_part(group, static_cast<int32_t>(x + 5),
                                                    static_cast<int32_t>(y + 5), frame, 0, 1, 0x28);
        } else {
            void* icon = fn_imgsys_get_loc(0xf, 9);
            if (icon != nullptr) fn_grpx_draw_part(group, static_cast<int32_t>(x + 5),
                                                   static_cast<int32_t>(y + 5), icon, 0, 1, 0x28);
        }
    }
}

void mix_draw_end_wrapper() {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (mix_frame_active) {
            const bool interactive = g_virtual_bag_enabled.load() && mix_state_locked() != 0;
            if (interactive) {
                ensure_state_loaded_locked();
                mix_install_tab_buttons_locked();
                mix_commit_pending_tab_locked();
                // 点原版袋按钮 / 切配方类型会改写当前袋（UIMix_InvenBagControlEventProc /
                // UIMix_ButtonMenuListExe 置 0）；当前袋偏离投影宿主即退出扩展视图。
                if (mix_view_installed && original_bag_locked() != mix_window_original_bag) {
                    mix_restore_module_view_locked();
                }
                mix_refresh_projection_locked();
                mix_draw_tab_buttons_locked();
            } else {
                // state 0（配方菜单）/扩展禁用：卸载投影与页签，保留 frame_active 待
                // 选配方（state 1）后重新挂载。
                mix_restore_module_view_locked();
                if (mix_tab_buttons[0] != nullptr) {
                    mix_delete_tab_buttons_locked();
                    mix_disable_tab_buttons_locked();
                }
            }
        }
    }
    if (fn_grpx_end != nullptr) fn_grpx_end();
}

void mix_teardown_locked() {
    mix_restore_module_view_locked();
    mix_disable_tab_buttons_locked();
    for (void*& item : mix_tab_bag_items) {
        if (item != nullptr) {
            if (defer_item_free_locked(item)) item = nullptr;
        }
    }
    mix_frame_active = false;
}

bool mix_patch_bl_locked(uintptr_t call_addr, uint32_t original_instruction,
                         void* wrapper, void** thunk, uintptr_t* patched_addr) {
    if (*patched_addr != 0) return true;
    const int64_t direct_delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(wrapper)) -
                                 static_cast<int64_t>(call_addr);
    uintptr_t branch_target = reinterpret_cast<uintptr_t>(wrapper);
    if ((direct_delta & 0x3) != 0 || direct_delta <= -0x08000000LL ||
        direct_delta >= 0x08000000LL) {
        *thunk = allocate_draw_thunk(call_addr, reinterpret_cast<uintptr_t>(wrapper));
        if (*thunk == nullptr) return false;
        branch_target = reinterpret_cast<uintptr_t>(*thunk);
    }
    const int64_t delta = static_cast<int64_t>(branch_target) - static_cast<int64_t>(call_addr);
    if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) return false;
    const uint32_t replacement = 0x94000000u |
                                 (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    constexpr size_t kPageSize = 4096;
    const uintptr_t page = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    const uint32_t current = *reinterpret_cast<uint32_t*>(call_addr);
    if (current != original_instruction && current != replacement) return false;
    *reinterpret_cast<uint32_t*>(call_addr) = replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(call_addr),
                             reinterpret_cast<char*>(call_addr + sizeof(uint32_t)));
    *patched_addr = call_addr;
    return true;
}

// R-57 原版袋列高亮遮蔽：进入扩展视图时，画原版 UIMix_DrawInvenBagGroup 前把当前袋
// GOT 临时置 kNoOriginalBagSelected(6)，画完恢复 → 原版 6 袋不再高亮（与扩展页
// virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end 的
// mix_draw_tab_buttons_locked 在袋列绘制之后补画。
void mix_draw_inven_bag_group_wrapper() {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UIMIX_DRAW_INVEN_BAG_GROUP_VMA", F_UIMIX_DRAW_INVEN_BAG_GROUP_VMA)
        : 0;
    if (raw == 0) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool masked = false;
    if (mix_view_installed && current_bag != nullptr && *current_bag != nullptr &&
        **current_bag != kNoOriginalBagSelected) {
        saved_current = **current_bag;
        **current_bag = kNoOriginalBagSelected;
        masked = true;
    }
    reinterpret_cast<void (*)()>(raw)();
    if (masked) **current_bag = saved_current;
}

bool install_mix_hooks_locked() {
    if (mix_hooks_ready) return true;
    if (g_base == 0) return false;
    if (mix_state_entry == nullptr) {
        mix_state_entry = find_popup_state_entry(g_base + fn_resolve(
            "F_PANEL_CRAFT_ENTER", F_PANEL_CRAFT_ENTER));
        if (mix_state_entry == nullptr) return false;
        const uintptr_t expected_event = g_base + fn_resolve(
            "F_SCENE_EVENT_MIX_VMA", F_SCENE_EVENT_MIX_VMA);
        if (*reinterpret_cast<uintptr_t*>(mix_state_entry + POPUP_ENTRY_EVENT) != expected_event) {
            mix_state_entry = nullptr;
            VIRTBAG_LOG("mix state callbacks differ from expected symbols");
            return false;
        }
        mix_original_enter = reinterpret_cast<MixSceneNoArgFn>(
            *reinterpret_cast<uintptr_t*>(mix_state_entry + POPUP_ENTRY_ENTER));
        mix_original_f3 = reinterpret_cast<MixSceneNoArgFn>(
            *reinterpret_cast<uintptr_t*>(mix_state_entry + POPUP_ENTRY_F3));
        *reinterpret_cast<uintptr_t*>(mix_state_entry + POPUP_ENTRY_ENTER) =
            reinterpret_cast<uintptr_t>(&mix_enter_wrapper);
        *reinterpret_cast<uintptr_t*>(mix_state_entry + POPUP_ENTRY_F3) =
            reinterpret_cast<uintptr_t>(&mix_f3_wrapper);
    }

    const uintptr_t draw_call = g_base + fn_resolve(
        "F_SCENE_DRAW_MIX_GRPX_END_CALL_VMA", F_SCENE_DRAW_MIX_GRPX_END_CALL_VMA);
    if (!mix_patch_bl_locked(draw_call, 0x97fd0fa8,
                             reinterpret_cast<void*>(&mix_draw_end_wrapper),
                             &mix_draw_thunk, &mix_draw_patch_addr)) return false;

    // R-57 原版袋列高亮遮蔽：UIMix_Draw 内 bl UIMix_DrawInvenBagGroup 调用点。
    const uintptr_t bag_draw_call = g_base + fn_resolve(
        "F_UIMIX_DRAW_INVEN_BAG_GROUP_CALL_VMA", F_UIMIX_DRAW_INVEN_BAG_GROUP_CALL_VMA);
    if (!mix_patch_bl_locked(bag_draw_call, 0x97fffe5e,
                             reinterpret_cast<void*>(&mix_draw_inven_bag_group_wrapper),
                             &mix_bag_draw_thunk, &mix_bag_draw_patch_addr)) return false;

    mix_hooks_ready = true;
    VIRTBAG_LOG("mix host hooks installed state=%p", mix_state_entry);
    return true;
}
