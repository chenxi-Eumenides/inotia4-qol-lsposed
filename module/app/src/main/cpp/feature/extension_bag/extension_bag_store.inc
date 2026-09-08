// 商店宿主只管理 UIStore 自己的控件、投影和详情事务，不复用 UIEquip 的视图状态。

using StoreSceneEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using StoreSceneNoArgFn = void (*)();
using StorePopupCallbackFn = void (*)(void*);

bool store_view_installed = false;
int store_view_bag = -1;
int store_window_original_bag = -1;
uint32_t* store_original_size_word = nullptr;
uint32_t store_original_bag_size = 0;
void* store_projected_item_root = nullptr;
std::array<void*, virtual_bag::kBagCount> store_tab_buttons{};
std::array<void*, virtual_bag::kBagCount> store_tab_bag_items{};
void* store_tab_root = nullptr;
uint64_t store_tab_generation = 0;
int store_pending_tab = -1;
uint64_t store_pending_tab_generation = 0;
bool store_frame_active = false;
PtrHook store_desc_sell_hook{};
void* store_desc_item = nullptr;
int store_desc_item_bag = -1;
int store_desc_item_slot = -1;
StorePopupCallbackFn store_sell_original_ok = nullptr;
StorePopupCallbackFn store_sell_original_cancel = nullptr;

StoreSceneEventFn store_original_event = nullptr;
StoreSceneNoArgFn store_original_f3 = nullptr;
StoreSceneNoArgFn store_original_enter = nullptr;
uint8_t* store_state_entry = nullptr;
uintptr_t store_desc_open_patch_addr = 0;
uintptr_t store_draw_patch_addr = 0;
uintptr_t store_buy_gate_1_patch_addr = 0;
uintptr_t store_buy_gate_2_patch_addr = 0;
void* store_desc_open_thunk = nullptr;
void* store_draw_thunk = nullptr;
void* store_buy_gate_1_thunk = nullptr;
void* store_buy_gate_2_thunk = nullptr;
bool store_hooks_ready = false;

void store_sell_ok(void*);
void store_sell_cancel(void*);
void store_desc_sell_execute(void*);
uint64_t store_tab_item_proc(void*, uint64_t, void*, void*);

bool store_hooks_installed() {
    return store_hooks_ready;
}

void store_restore_popup_callbacks_locked() {
    if (g_base != 0) {
        auto* ok_slot = reinterpret_cast<StorePopupCallbackFn*>(g_base + G_POPUP_FPOK_VMA);
        auto* cancel_slot = reinterpret_cast<StorePopupCallbackFn*>(g_base + G_POPUP_FPCANCEL_VMA);
        if (*ok_slot == &store_sell_ok) *ok_slot = store_sell_original_ok;
        if (*cancel_slot == &store_sell_cancel) *cancel_slot = store_sell_original_cancel;
    }
    store_sell_original_ok = nullptr;
    store_sell_original_cancel = nullptr;
}

void store_reset_desc_hook_locked() {
    store_desc_sell_hook.uninstall();
    store_restore_popup_callbacks_locked();
    store_desc_item = nullptr;
    store_desc_item_bag = -1;
    store_desc_item_slot = -1;
}

void store_clear_desc_locked() {
    store_reset_desc_hook_locked();
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
}

int store_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (store_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

void store_refresh_tab_bag_items_locked() {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const uint8_t type = g_virtual_bag_state.types[index];
        void*& item = store_tab_bag_items[index];
        if (type == 0) {
            if (item != nullptr) {
                defer_item_free_locked(item);
                item = nullptr;
            }
            continue;
        }
        if (item != nullptr && fn_get_bit != nullptr) {
            const uint16_t flags = *reinterpret_cast<uint16_t*>(
                reinterpret_cast<uint8_t*>(item) + I_TYPE);
            if (fn_get_bit(flags, 15, 6) != static_cast<int>(type)) {
                defer_item_free_locked(item);
                item = nullptr;
            }
        }
        if (item == nullptr && fn_create_item != nullptr) {
            item = fn_create_item(static_cast<int32_t>(type), 0, 0, 0);
            if (item != nullptr) {
                const uint32_t flags = *reinterpret_cast<uint32_t*>(
                    reinterpret_cast<uint8_t*>(item) + I_COUNT);
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
                    stack_codec::write_count(flags, 1);
            }
        }
    }
}

void store_disable_tab_buttons_locked() {
    ++store_tab_generation;
    store_pending_tab = -1;
    store_pending_tab_generation = 0;
    store_tab_buttons.fill(nullptr);
    store_tab_root = nullptr;
}

void store_delete_tab_buttons_locked() {
    if (fn_touch_handle_delete_control != nullptr) {
        for (void*& button : store_tab_buttons) {
            if (button != nullptr) fn_touch_handle_delete_control(button);
            button = nullptr;
        }
    } else {
        store_tab_buttons.fill(nullptr);
    }
}

void store_install_tab_buttons_locked() {
    if (g_base == 0 || fn_control_object_get_child == nullptr ||
        fn_control_item_set_item == nullptr || fn_mem_malloc == nullptr ||
        fn_control_object_set_control_proc == nullptr ||
        fn_control_object_set_user_type == nullptr ||
        fn_touch_handle_unuse_control_event_move == nullptr ||
        fn_touch_handle_delete_control == nullptr || fn_ctrl_get_count == nullptr ||
        fn_ctrl_set_data == nullptr) {
        return;
    }
    void* bag_container = *reinterpret_cast<void**>(g_base + G_UISTORE_BAG_CTRL_VMA);
    if (bag_container == nullptr || fn_ctrl_get_count(bag_container) < 6) return;
    if (store_tab_root == bag_container && store_tab_buttons[0] != nullptr) return;

    if (store_tab_root != nullptr) store_delete_tab_buttons_locked();
    store_refresh_tab_bag_items_locked();

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
    // 这里沿用原版袋列的右侧间距 68、下移 2；起点仍来自商店实时袋 0 rect。
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
            reinterpret_cast<void*>(&store_tab_item_proc));
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
        *reinterpret_cast<void**>(data) = store_tab_bag_items[index];
        fn_ctrl_set_data(control, data);
        store_tab_buttons[index] = control;
    }
    store_tab_root = bag_container;
    ++store_tab_generation;
    VIRTBAG_LOG("store tabs installed root=%p generation=%llu", bag_container,
                static_cast<unsigned long long>(store_tab_generation));
}

uint64_t store_tab_item_proc(void* ctrl, uint64_t event, void*, void*) {
    if (event == 0x02) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const int index = store_tab_index(ctrl);
        if (index >= 0 && store_tab_generation != 0) {
            store_pending_tab = index;
            store_pending_tab_generation = store_tab_generation;
        }
        return 1;
    }
    // 商店宿主不接管拖拽，避免扩展标签成为拖动源或跨袋目标。
    return 0;
}

void store_install_module_view_locked(int bag);
void store_restore_module_view_locked();

void store_commit_pending_tab_locked() {
    if (!virtual_bag::valid_index(store_pending_tab) ||
        store_pending_tab_generation != store_tab_generation) {
        store_pending_tab = -1;
        store_pending_tab_generation = 0;
        return;
    }
    const int bag = store_pending_tab;
    store_pending_tab = -1;
    store_pending_tab_generation = 0;
    if (store_view_installed && store_view_bag == bag) return;
    store_install_module_view_locked(bag);
}

void store_refresh_projection_locked() {
    if (!store_view_installed || store_projected_item_root == nullptr ||
        fn_control_object_get_data == nullptr || fn_control_item_set_item == nullptr ||
        !virtual_bag::valid_index(store_view_bag)) return;
    const int capacity = g_virtual_bag_state.capacities[store_view_bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(store_projected_item_root, slot);
        if (control == nullptr) continue;
        void* expected = module_item_locked(store_view_bag, slot);
        void* data = fn_control_object_get_data(control);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != expected) fn_control_item_set_item(control, expected);
    }
}

void store_install_module_view_locked(int bag) {
    if (g_base == 0 || !virtual_bag::valid_index(bag) ||
        g_virtual_bag_state.capacities[bag] == 0 ||
        fn_control_item_set_item == nullptr || fn_control_object_get_child == nullptr ||
        fn_ui_store_refresh_inven_item == nullptr) return;
    const int original_bag = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(original_bag)) {
        VIRTBAG_LOG("store view rejected original bag=%d", original_bag);
        return;
    }
    uint32_t* size_word = bag_size_word_locked(original_bag);
    if (size_word == nullptr) return;
    if (store_view_installed) store_restore_module_view_locked();
    store_clear_desc_locked();

    store_window_original_bag = original_bag;
    store_original_size_word = size_word;
    store_original_bag_size = *size_word;
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (*size_word & ~kCapacityMask) |
                 (static_cast<uint32_t>(g_virtual_bag_state.capacities[bag]) & kCapacityMask);

    store_projected_item_root = *reinterpret_cast<void**>(g_base + G_UISTORE_ITEM_CTRL_VMA);
    if (store_projected_item_root == nullptr) {
        *size_word = (*size_word & ~kCapacityMask) |
                     (store_original_bag_size & kCapacityMask);
        store_original_size_word = nullptr;
        store_original_bag_size = 0;
        store_window_original_bag = -1;
        return;
    }
    fn_ui_store_refresh_inven_item();
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* control = valid_child_locked(store_projected_item_root, slot);
        if (control == nullptr) continue;
        void* item = module_item_locked(bag, slot);
        fn_control_item_set_item(control, item);
        if (item != nullptr) {
            ownership::borrow_for_view(&g_ownership_ledger,
                                       g_module_object_handles[bag][slot]);
        }
    }
    store_view_bag = bag;
    store_view_installed = true;
    VIRTBAG_LOG("store view installed bag=%d window=%d capacity=%d", bag, original_bag,
                g_virtual_bag_state.capacities[bag]);
}

void store_restore_module_view_locked() {
    if (!store_view_installed || g_base == 0) return;
    if (virtual_bag::valid_index(store_view_bag)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = g_module_object_handles[store_view_bag][slot];
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
        }
    }
    store_clear_desc_locked();
    if (store_original_size_word != nullptr) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        *store_original_size_word = (*store_original_size_word & ~kCapacityMask) |
                                    (store_original_bag_size & kCapacityMask);
    }
    if (fn_ui_store_refresh_inven_item != nullptr) fn_ui_store_refresh_inven_item();
    store_original_size_word = nullptr;
    store_original_bag_size = 0;
    store_window_original_bag = -1;
    store_projected_item_root = nullptr;
    store_view_bag = -1;
    store_view_installed = false;
}

int store_original_bag_button_index(int64_t x, int64_t y) {
    if (g_base == 0 || fn_ctrl_get_count == nullptr) return -1;
    void* container = *reinterpret_cast<void**>(g_base + G_UISTORE_BAG_CTRL_VMA);
    if (container == nullptr) return -1;
    const int count = static_cast<int>(fn_ctrl_get_count(container));
    const int limit = count < 6 ? count : 6;
    for (int index = 0; index < limit; ++index) {
        void* control = valid_child_locked(container, index);
        if (control == nullptr) continue;
        int64_t absolute_x = 0;
        int64_t absolute_y = 0;
        extension_bag_control_abs_pos(control, &absolute_x, &absolute_y);
        const uint8_t* bytes = static_cast<const uint8_t*>(control);
        const int64_t width = *reinterpret_cast<const int64_t*>(bytes + CO_RECT_W);
        const int64_t height = *reinterpret_cast<const int64_t*>(bytes + CO_RECT_H);
        if (width > 0 && height > 0 && x >= absolute_x && x < absolute_x + width &&
            y >= absolute_y && y < absolute_y + height) return index;
    }
    return -1;
}

void store_restore_for_original_event_locked(int64_t x, int64_t y) {
    if (!store_view_installed) return;
    if (store_original_bag_button_index(x, y) < 0) return;
    store_restore_module_view_locked();
    // 注意：不得 disable 扩展标签——装备页宿主切回原版袋后扩展标签仍可点回
    // 扩展视图；disable 只会清空指针（控件仍在袋容器上），draw_end 再次 install
    // 时因 store_tab_root 已清空无法删除旧控件，会导致标签重复创建。
}

uint64_t store_event_wrapper(uint64_t event, uint64_t param, uint64_t param2) {
    int64_t x = 0;
    int64_t y = 0;
    if ((event == 0x17 || event == 0x18) && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_restore_for_original_event_locked(x, y);
    }
    return store_original_event != nullptr ? store_original_event(event, param, param2) : 0;
}
void store_enter_wrapper() {
    if (store_original_enter != nullptr) store_original_enter();
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    store_teardown_locked();
    set_original_bag_locked(0);
    store_frame_active = true;
    store_install_tab_buttons_locked();
    if (fn_ui_store_refresh_inven_item != nullptr) fn_ui_store_refresh_inven_item();
}

void store_f3_wrapper() {
    StoreSceneNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_teardown_locked();
        store_frame_active = false;
        original = store_original_f3;
    }
    if (original != nullptr) original();
}

void store_draw_tab_buttons_locked() {
    if (fn_grpx_draw_part == nullptr || fn_imgsys_get_group == nullptr ||
        fn_imgsys_get_loc == nullptr) return;
    void* group = fn_imgsys_get_group(0xf);
    if (group == nullptr) return;
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = store_tab_buttons[index];
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
        void* button = store_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t x = 0;
        int64_t y = 0;
        extension_bag_control_abs_pos(button, &x, &y);
        const bool selected = store_view_installed && store_view_bag == index;
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

void store_draw_end_wrapper() {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (store_frame_active && g_virtual_bag_enabled.load()) {
            ensure_state_loaded_locked();
            store_install_tab_buttons_locked();
            store_commit_pending_tab_locked();
            store_refresh_projection_locked();
            store_draw_tab_buttons_locked();
        } else if (store_frame_active) {
            store_teardown_locked();
            store_frame_active = false;
        }
    }
    if (fn_grpx_end != nullptr) fn_grpx_end();
}

void store_make_desc_gate(void* ctrl, void* arg) {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UISTORE_MAKE_DESC_VMA", F_UISTORE_MAKE_DESC_VMA) : 0;
    if (raw == 0 || ctrl == nullptr) return;
    reinterpret_cast<UiStoreMakeDescFn>(raw)(ctrl, arg);

    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    store_reset_desc_hook_locked();
    if (!store_view_installed || fn_control_object_get_data == nullptr ||
        !game_memory_accessible(ctrl, CO_DATA + sizeof(void*), 'r')) return;
    void* data = fn_control_object_get_data(ctrl);
    if (data == nullptr || !game_memory_accessible(data, sizeof(void*), 'r')) return;
    void* item = *reinterpret_cast<void**>(data);
    int bag = -1;
    int slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &bag, &slot) ||
        bag != store_view_bag) return;
    store_desc_item = item;
    store_desc_item_bag = bag;
    store_desc_item_slot = slot;

    void* button = *reinterpret_cast<void**>(g_base + G_UISTORE_SELL_BTN_VMA);
    const uintptr_t sell_raw = g_base + fn_resolve(
        "F_UISTORE_BUTTON_SELL_EXE_VMA", F_UISTORE_BUTTON_SELL_EXE_VMA);
    if (button == nullptr || !game_memory_accessible(button, CO_DATA + sizeof(void*), 'r')) return;
    data = fn_control_object_get_data(button);
    if (data == nullptr || !game_memory_accessible(data, CB_EXECUTE_PROC + sizeof(void*), 'r')) return;
    void** execute_slot = reinterpret_cast<void**>(static_cast<uint8_t*>(data) + CB_EXECUTE_PROC);
    if (!game_memory_accessible(execute_slot, sizeof(void*), 'w') ||
        *execute_slot != reinterpret_cast<void*>(sell_raw)) return;
    if (!store_desc_sell_hook.install_typed(execute_slot, &store_desc_sell_execute)) {
        store_desc_item = nullptr;
        store_desc_item_bag = -1;
        store_desc_item_slot = -1;
        VIRTBAG_LOG("store sell hook install failed button=%p", button);
    }
}

void store_sell_cancel(void*) {
    StorePopupCallbackFn original_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        original_cancel = store_sell_original_cancel;
        store_reset_desc_hook_locked();
    }
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
    if (original_cancel != nullptr) original_cancel(nullptr);
}

void store_sell_ok(void*) {
    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int count = 0;
    int64_t price = -1;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_restore_popup_callbacks_locked();
        item = store_desc_item;
        bag = store_desc_item_bag;
        slot = store_desc_item_slot;
        if (item != nullptr && virtual_bag::valid_index(bag) &&
            slot >= 0 && slot < virtual_bag::kSlotCount &&
            g_module_objects[bag][slot] == item) {
            count = g_virtual_bag_state.items[bag][slot].count;
            price = extension_bag_sell_price(item, count, false);
        }
    }
    if (price < 0 || fn_add_money == nullptr || fn_minus_money == nullptr ||
        fn_item_is_no_sell == nullptr || !fn_add_money(price)) {
        VIRTBAG_LOG("store sell credit failed bag=%d slot=%d price=%lld", bag, slot,
                    static_cast<long long>(price));
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_reset_desc_hook_locked();
        if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
        return;
    }

    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const bool current = item != nullptr && virtual_bag::valid_index(bag) &&
                             slot >= 0 && slot < virtual_bag::kSlotCount &&
                             g_module_objects[bag][slot] == item &&
                             g_virtual_bag_state.items[bag][slot].count == count &&
                             module_object_release_allowed_locked(
                                 bag, slot, item, "store sell", false, nullptr);
        if (current) {
            const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
            g_virtual_bag_state.items[bag][slot] = {};
            g_item_state_dirty = true;
            if (!free_module_object_locked(bag, slot)) {
                g_virtual_bag_state.items[bag][slot] = previous_item;
                persist_state_locked();
                VIRTBAG_LOG("store sell rejected object release bag=%d slot=%d", bag, slot);
            } else {
                persist_state_locked();
                // 只用商店投影刷新：调用原版 UIStore_RefreshInvenItem 会把整列控件
                // 刷回窗口原版袋的 INVEN 内容，破坏扩展投影（equip 宿主等价用
                // refresh_module_item_area_locked）。store_refresh_projection_locked
                // 会把已删除槽置 null、其余槽保持扩展物品。
                if (store_view_installed && store_view_bag == bag) {
                    store_refresh_projection_locked();
                }
                removed = true;
            }
        }
        store_reset_desc_hook_locked();
        if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
    }
    if (!removed) {
        const bool refunded = fn_minus_money(price) != 0;
        VIRTBAG_LOG("store sell remove failed bag=%d slot=%d price=%lld refunded=%d", bag, slot,
                    static_cast<long long>(price), refunded ? 1 : 0);
        return;
    }
    VIRTBAG_LOG("store sell committed bag=%d slot=%d price=%lld", bag, slot,
                static_cast<long long>(price));
}

void store_desc_sell_execute(void* button) {
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    int64_t price = -1;
    bool valid = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const bool current = store_desc_item != nullptr &&
                             virtual_bag::valid_index(store_desc_item_bag) &&
                             store_desc_item_slot >= 0 &&
                             store_desc_item_slot < virtual_bag::kSlotCount &&
                             g_module_objects[store_desc_item_bag][store_desc_item_slot] ==
                                 store_desc_item;
        if (current) {
            item = store_desc_item;
            bag = store_desc_item_bag;
            slot = store_desc_item_slot;
            const int count = g_virtual_bag_state.items[bag][slot].count;
            if (fn_item_is_no_sell != nullptr && fn_get_bit != nullptr && count > 0) {
                const uint16_t flags = *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(item) + I_TYPE);
                const int category = fn_get_bit(flags, 15, 6);
                if (fn_item_is_no_sell(category) == 0) {
                    price = extension_bag_sell_price(item, count, false);
                    valid = price >= 0;
                }
            }
        }
    }
    const uintptr_t original_ok = g_base != 0
        ? g_base + fn_resolve("F_UISTORE_SELL_ITEM_VMA", F_UISTORE_SELL_ITEM_VMA) : 0;
    const bool popup_created = valid && price <= UINT32_MAX && original_ok != 0 &&
                               fn_popup_create_yesno_from_textdata != nullptr;
    if (popup_created) {
        if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
        // 弹窗先登记原版 UIStore_SellItem，再替换全局回调实现扩展袋事务。
        fn_popup_create_yesno_from_textdata(
            9, 0, 2, reinterpret_cast<void*>(original_ok), nullptr,
            reinterpret_cast<void*>(static_cast<uintptr_t>(price)));
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        auto* ok_slot = reinterpret_cast<StorePopupCallbackFn*>(g_base + G_POPUP_FPOK_VMA);
        auto* cancel_slot = reinterpret_cast<StorePopupCallbackFn*>(g_base + G_POPUP_FPCANCEL_VMA);
        store_sell_original_ok = *ok_slot;
        store_sell_original_cancel = *cancel_slot;
        *ok_slot = &store_sell_ok;
        *cancel_slot = &store_sell_cancel;
        store_desc_sell_hook.uninstall();
    } else {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        store_reset_desc_hook_locked();
        if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
        VIRTBAG_LOG("store sell confirmation unavailable bag=%d slot=%d valid=%d", bag, slot,
                    valid ? 1 : 0);
    }
    (void)button;
}

void store_teardown_locked() {
    store_restore_module_view_locked();
    store_disable_tab_buttons_locked();
    store_reset_desc_hook_locked();
    for (void*& item : store_tab_bag_items) {
        if (item != nullptr) {
            defer_item_free_locked(item);
            item = nullptr;
        }
    }
    store_frame_active = false;
}

// 买入预检 gate：替代 UIStore_BuyItem 内两处 bl INVEN_FindSaveSlot。
// 先走原版 FindSaveSlot（真实容量），若原版有槽直接返回槽号；原版无槽
// （返回 0）但扩展袋有空位时返回 1（非 0 槽号语义仅被上层当"有空间"
// 判断），让流程继续调用 INVEN_SaveItem → save_item_wrapper 触发扩展
// adopt 收编，购买不再误报"背包已满"。该函数不持锁调用原版指针。
uint8_t store_buy_find_slot_gate(void* item, void* out_slot) {
    uint8_t result = 0;
    if (fn_inven_find_save_slot != nullptr && item != nullptr) {
        result = static_cast<uint8_t>(fn_inven_find_save_slot(item, nullptr));
    }
    if (result != 0) return result;
    if (!g_virtual_bag_enabled.load()) return 0;
    if (extension_bag_has_empty_slots(1, 0)) return 1;
    return 0;
}

bool store_patch_bl_locked(uintptr_t call_addr, uint32_t original_instruction,
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

bool install_store_hooks_locked() {
    if (store_hooks_ready) return true;
    if (g_base == 0) return false;
    if (store_state_entry == nullptr) {
        store_state_entry = find_popup_state_entry(g_base + fn_resolve(
            "F_PANEL_SHOP_ENTER", F_PANEL_SHOP_ENTER));
        if (store_state_entry == nullptr) return false;
        const uintptr_t expected_event = g_base + fn_resolve(
            "F_SCENE_EVENT_STORE_VMA", F_SCENE_EVENT_STORE_VMA);
        if (*reinterpret_cast<uintptr_t*>(store_state_entry + 0x38) != expected_event) {
            store_state_entry = nullptr;
            VIRTBAG_LOG("store state callbacks differ from expected symbols");
            return false;
        }
        store_original_enter = reinterpret_cast<StoreSceneNoArgFn>(
            *reinterpret_cast<uintptr_t*>(store_state_entry + 0x10));
        store_original_f3 = reinterpret_cast<StoreSceneNoArgFn>(
            *reinterpret_cast<uintptr_t*>(store_state_entry + 0x28));
        store_original_event = reinterpret_cast<StoreSceneEventFn>(
            *reinterpret_cast<uintptr_t*>(store_state_entry + 0x38));
        *reinterpret_cast<uintptr_t*>(store_state_entry + 0x10) =
            reinterpret_cast<uintptr_t>(&store_enter_wrapper);
        *reinterpret_cast<uintptr_t*>(store_state_entry + 0x28) =
            reinterpret_cast<uintptr_t>(&store_f3_wrapper);
        *reinterpret_cast<uintptr_t*>(store_state_entry + 0x38) =
            reinterpret_cast<uintptr_t>(&store_event_wrapper);
    }

    const uintptr_t desc_call = g_base + fn_resolve(
        "F_UISTORE_DESC_MAKE_DESC_CALL_VMA", F_UISTORE_DESC_MAKE_DESC_CALL_VMA);
    if (!store_patch_bl_locked(desc_call, 0x97ffffc9,
                               reinterpret_cast<void*>(&store_make_desc_gate),
                               &store_desc_open_thunk, &store_desc_open_patch_addr)) return false;
    const uintptr_t draw_call = g_base + fn_resolve(
        "F_SCENE_DRAW_STORE_GRPX_END_CALL_VMA", F_SCENE_DRAW_STORE_GRPX_END_CALL_VMA);
    if (!store_patch_bl_locked(draw_call, 0x97fcff36,
                               reinterpret_cast<void*>(&store_draw_end_wrapper),
                               &store_draw_thunk, &store_draw_patch_addr)) return false;

    // 买入预检 gate：UIStore_BuyItem 内两处 bl INVEN_FindSaveSlot 改写为
    // store_buy_find_slot_gate。原版袋无空位但扩展袋有空位时返回非 0，
    // 让流程继续调 INVEN_SaveItem → save_item_wrapper 触发扩展 adopt。
    const uintptr_t buy_gate_1 = g_base + fn_resolve(
        "F_UISTORE_BUY_FIND_SLOT_CALL_1_VMA", F_UISTORE_BUY_FIND_SLOT_CALL_1_VMA);
    if (!store_patch_bl_locked(buy_gate_1, 0x9400c530,
                               reinterpret_cast<void*>(&store_buy_find_slot_gate),
                               &store_buy_gate_1_thunk, &store_buy_gate_1_patch_addr)) return false;
    const uintptr_t buy_gate_2 = g_base + fn_resolve(
        "F_UISTORE_BUY_FIND_SLOT_CALL_2_VMA", F_UISTORE_BUY_FIND_SLOT_CALL_2_VMA);
    if (!store_patch_bl_locked(buy_gate_2, 0x9400c508,
                               reinterpret_cast<void*>(&store_buy_find_slot_gate),
                               &store_buy_gate_2_thunk, &store_buy_gate_2_patch_addr)) return false;

    store_hooks_ready = true;
    VIRTBAG_LOG("store host hooks installed state=%p", store_state_entry);
    return true;
}
