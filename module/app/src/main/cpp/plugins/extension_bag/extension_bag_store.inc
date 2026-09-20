// 商店宿主只管理 UIStore 自己的控件、投影和详情事务，不复用 UIEquip 的视图状态。


using StoreSceneEventFn = uint64_t (*)(uint64_t, uint64_t, uint64_t);
using StoreSceneNoArgFn = void (*)();

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
// W1 商店扩展物品出售：UIStore_SellItem@0xd25f0 函数入口 hook（backup 存本全局）。
// 原版数量面板/确认/取消全部保留；模块物品在入口层按「弹窗载荷 / 单价」反推数量后
// 接管整堆删除或部分递减，原版物品一律 backup。
using StoreSellItemFn = int64_t (*)(int64_t);
void* g_backup_store_sell_item = nullptr;

StoreSceneEventFn store_original_event = nullptr;
StoreSceneNoArgFn store_original_f3 = nullptr;
StoreSceneNoArgFn store_original_enter = nullptr;
uint8_t* store_state_entry = nullptr;

uint64_t store_tab_item_proc(void*, uint64_t, void*, void*);

void store_clear_desc_locked() {
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
                    ::qol::features::extension_bag::write_native_bag_object_marker(flags);
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
    // 配置门控：扩展背包关闭时商店页签不得挂载（3 个宿主之一）。
    if (!g_virtual_bag_enabled.load()) return;
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
            store_delete_tab_buttons_locked();
            store_teardown_locked();
            store_frame_active = false;
        }
    }
    if (fn_grpx_end != nullptr) fn_grpx_end();
}

void store_make_desc_gate(void* ctrl, void* arg) {
    // 详情打开仍走原版 MakeDesc。W1 不再在此捕获详情身份、也不再替换出售按钮回调
    // （原全局 popup OK/Cancel 槽劫持与按钮 PtrHook 已删除）；扩展出售改为
    // UIStore_SellItem 函数入口按 cursor item 识别，原版面板/确认/取消全部保留。
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UISTORE_MAKE_DESC_VMA", F_UISTORE_MAKE_DESC_VMA) : 0;
    if (raw == 0 || ctrl == nullptr) return;
    reinterpret_cast<UiStoreMakeDescFn>(raw)(ctrl, arg);
}

void store_sell_popup(uint32_t text_id) {
    if (fn_popup_create_ok_from_textdata != nullptr) {
        fn_popup_create_ok_from_textdata(text_id, 0, 0, 0);
    }
}

// W1 部分出售：把模式视图数量从 view 递减 count（不调用原版 ITEMSYSTEM_Divide）。
// 写回经 item-count facade（R-47 决策 b：启用态拆段进位、关闭态只写 b 保留 a），
// 随后重序列化 payload 并把 descriptor.count 回写为 canonical（R-38/R-48）。
bool decrement_extension_item_view_locked(int bag, int slot, void* item, int count,
                                          const char* context) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        item == nullptr || count <= 0) {
        return false;
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    const int view = static_cast<int>(virtual_bag_effective_count(
        item, static_cast<uint32_t>(descriptor.count)));
    if (count >= view) return false;  // 整堆由调用方走删除分支
    const uint32_t old_field = *reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(item) + I_COUNT);
    if (!virtual_bag_write_effective(item, static_cast<uint32_t>(view - count))) return false;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = old_field;
        VIRTBAG_LOG("store sell reserialize failed context=%s bag=%d slot=%d", context, bag, slot);
        return false;
    }
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.count = static_cast<int>(virtual_bag_canonical_count(
        item, virtual_bag_canonical_from_field(old_field, static_cast<uint32_t>(descriptor.count))));
    g_module_object_categories[bag][slot] = descriptor.category;
    g_module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
    g_item_state_dirty = true;
    return true;
}

// W1：UIStore_SellItem@0xd25f0 入口接管。原版只收总价 x0（弹窗载荷），数量由函数内部
// 自行读取并走 INVEN_RemoveItem/ITEMSYSTEM_Divide；模块在入口层用同一「载荷/单价」
// 反推数量后接管结算，原版物品一律 backup。
int64_t store_sell_item_wrapper(int64_t displayed_price) {
    if (g_backup_store_sell_item == nullptr) return 0;
    if (!g_virtual_bag_enabled.load()) return reinterpret_cast<StoreSellItemFn>(g_backup_store_sell_item)(displayed_price);

    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int view = 0;
    bool item_encoded = false;
    bool module_item = false;
    bool reject = false;
    qol::store_sell::Plan plan{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (store_view_installed && g_base != 0 && fn_control_object_get_cursor != nullptr &&
            fn_control_item_get_item != nullptr && fn_item_get_sell_price != nullptr) {
            void* ctrl = *reinterpret_cast<void**>(g_base + G_UISTORE_ITEM_CTRL_VMA);
            void* cursor = ctrl != nullptr ? fn_control_object_get_cursor(ctrl) : nullptr;
            item = cursor != nullptr ? fn_control_item_get_item(cursor) : nullptr;
            if (item != nullptr && module_slot_of_item_locked(item, &bag, &slot)) {
                module_item = true;
                const bool identity_ok =
                    bag == store_view_bag && virtual_bag::valid_index(bag) && slot >= 0 &&
                    slot < virtual_bag::kSlotCount && g_module_objects[bag][slot] == item &&
                    module_object_release_allowed_locked(bag, slot, item, "store sell", false,
                                                         nullptr);
                const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
                const bool descriptor_ok =
                    descriptor.category > 0 && descriptor.count > 0 &&
                    g_module_object_categories[bag][slot] == descriptor.category &&
                    g_module_object_hashes[bag][slot] == virtual_bag::payload_hash(descriptor);
                if (!identity_ok || !descriptor_ok) {
                    reject = true;
                } else {
                    view = static_cast<int>(virtual_bag_effective_count(
                        item, static_cast<uint32_t>(descriptor.count)));
                    item_encoded = item_count_encoding(item) ==
                                   ::qol::contracts::QolItemCountEncoding::kEncoded;
                    const int64_t unit = static_cast<int64_t>(fn_item_get_sell_price(item));
                    plan = qol::store_sell::plan(item_encoded, view, unit, displayed_price);
                    if (plan.kind == qol::store_sell::PlanKind::kReject) reject = true;
                }
            }
        }
    }
    if (!module_item) return reinterpret_cast<StoreSellItemFn>(g_backup_store_sell_item)(displayed_price);
    if (reject) {
        VIRTBAG_LOG("store sell reject reason=identity_or_plan bag=%d slot=%d", bag, slot);
        store_sell_popup(0xc);
        return 0;
    }

    const int count = plan.kind == qol::store_sell::PlanKind::kWhole ? 1 : plan.count;
    const int64_t price = extension_bag_sell_price(item, count, false);
    if (price < 0 || price != displayed_price || fn_add_money == nullptr ||
        fn_minus_money == nullptr) {
        VIRTBAG_LOG("store sell reject reason=price_mismatch bag=%d slot=%d count=%d price=%lld displayed=%lld",
                    bag, slot, count, static_cast<long long>(price),
                    static_cast<long long>(displayed_price));
        store_sell_popup(0xc);
        return 0;
    }
    if (!fn_add_money(price)) {
        VIRTBAG_LOG("store sell credit failed bag=%d slot=%d price=%lld", bag, slot,
                    static_cast<long long>(price));
        store_sell_popup(0xc);
        return 0;
    }

    bool committed = false;
    bool whole = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const bool current =
            virtual_bag::valid_index(bag) && slot >= 0 && slot < virtual_bag::kSlotCount &&
            g_module_objects[bag][slot] == item &&
            module_object_release_allowed_locked(bag, slot, item, "store sell", false, nullptr) &&
            (!item_encoded ||
             static_cast<int>(virtual_bag_effective_count(
                 item, static_cast<uint32_t>(g_virtual_bag_state.items[bag][slot].count))) == view);
        if (current) {
            whole = plan.kind == qol::store_sell::PlanKind::kWhole || count >= view;
            if (whole) {
                const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
                g_virtual_bag_state.items[bag][slot] = {};
                g_item_state_dirty = true;
                if (!free_module_object_locked(bag, slot)) {
                    g_virtual_bag_state.items[bag][slot] = previous_item;
                    persist_state_locked();
                    VIRTBAG_LOG("store sell rejected object release bag=%d slot=%d", bag, slot);
                } else {
                    persist_state_locked();
                    committed = true;
                }
            } else if (decrement_extension_item_view_locked(bag, slot, item, count, "store sell")) {
                persist_state_locked();
                committed = true;
            }
        }
        if (committed && store_view_installed && store_view_bag == bag) {
            // R-12：只用商店投影刷新；调用原版 UIStore_RefreshInvenItem 会把整列刷回
            // 窗口原版袋内容，破坏扩展投影。
            store_refresh_projection_locked();
        }
    }
    if (!committed) {
        const bool refunded = fn_minus_money(price) != 0;
        VIRTBAG_LOG("store sell remove failed bag=%d slot=%d count=%d price=%lld refunded=%d",
                    bag, slot, count, static_cast<long long>(price), refunded ? 1 : 0);
        store_sell_popup(0xc);
        return 0;
    }
    store_sell_popup(0x1f);
    VIRTBAG_LOG("store sell committed bag=%d slot=%d count=%d whole=%d view=%d price=%lld",
                bag, slot, count, whole ? 1 : 0, view, static_cast<long long>(price));
    return 0;
}


void store_teardown_locked() {
    store_restore_module_view_locked();
    store_disable_tab_buttons_locked();
    for (void*& item : store_tab_bag_items) {
        if (item != nullptr) {
            if (defer_item_free_locked(item)) item = nullptr;
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


// R-57 原版袋列高亮遮蔽（商店）：进入扩展视图时，画原版 UIStore_DrawInvenBagGroup
// 前把当前袋 GOT 临时置 kNoOriginalBagSelected(6)，画完恢复 → 原版 6 袋不再高亮
// （与扩展页 virtual_bag_draw_original_bag_wrapper 同法）。扩展页签高亮由 draw_end
// 的 store_draw_tab_buttons_locked 在袋列绘制之后补画。
void store_draw_inven_bag_group_wrapper() {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA",
                              F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA)
        : 0;
    if (raw == 0) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool masked = false;
    if (store_view_installed && current_bag != nullptr && *current_bag != nullptr &&
        **current_bag != kNoOriginalBagSelected) {
        saved_current = **current_bag;
        **current_bag = kNoOriginalBagSelected;
        masked = true;
    }
    reinterpret_cast<void (*)()>(raw)();
    if (masked) **current_bag = saved_current;
}
