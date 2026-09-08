void draw_tab_buttons_in_frame_locked() {
    const bool can_draw_original_button = fn_grpx_draw_part != nullptr && fn_imgsys_get_group != nullptr &&
                                           fn_imgsys_get_loc != nullptr;
    void* group = can_draw_original_button ? fn_imgsys_get_group(0xf) : nullptr;
    // 标签挂袋容器（与原版 6 袋标签同父）：比对基准同步为袋容器句柄。
    void* current_bag_container =
        g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x50) : nullptr;
    // 容器就位重试：enter 时 install 可能因占位容器（child count<6）被跳过；
    // 每帧检测真容器就位且尚未挂载（root 变化）时重新 install。
    if (current_bag_container != nullptr &&
        fn_ctrl_get_count != nullptr && fn_ctrl_get_count(current_bag_container) >= 6 &&
        current_bag_container != g_extension_tab_root) {
        install_extension_tab_buttons_locked();
        current_bag_container =
            g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x50)
                        : nullptr;
    }
    const bool tabs_are_current =
        current_bag_container != nullptr && current_bag_container == g_extension_tab_root &&
        g_extension_tab_generation == g_inventory_generation;
    if (!tabs_are_current || !can_draw_original_button || group == nullptr) return;
    // 第一遍：底框 loc20（72x84），倒序 index 4→0（对齐原版 DrawInvenBackground 的 5→0）。
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        void* bg = fn_imgsys_get_loc(0xf, 0x14);
        if (bg != nullptr) {
            fn_grpx_draw_part(group, static_cast<int32_t>(ax - 5),
                              static_cast<int32_t>(ay - 12), bg, 0, 1, 0);
        }
    }
    // 第二遍：袋图标 loc6/loc9 + 选中高亮 loc19，正序 index 0→4（对齐原版 DrawInvenBag 的 0→5）。
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        const bool selected = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                              g_virtual_bag_state.selected == index;
        const bool equipped = g_virtual_bag_state.capacities[index] != 0;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        if (selected) {
            void* icon = fn_imgsys_get_loc(0xf, 0x13);
            if (icon != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax - 5),
                                  static_cast<int32_t>(ay - 12), icon, 0, 1, 0);
            }
            void* frame = fn_imgsys_get_loc(0xf, 6);
            if (frame != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax + 5),
                                  static_cast<int32_t>(ay + 5), frame, 0, 1, 0);
            }
        } else if (equipped) {
            void* frame = fn_imgsys_get_loc(0xf, 6);
            if (frame != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax + 5),
                                  static_cast<int32_t>(ay + 5), frame, 0, 1, 0x28);
            }
        } else {
            void* bag_icon = fn_imgsys_get_loc(0xf, 9);
            if (bag_icon != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(ax + 5),
                                  static_cast<int32_t>(ay + 5), bag_icon, 0, 1, 0x28);
            }
        }
    }
}

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = fn_grpx_draw_part != nullptr && fn_imgsys_get_group != nullptr &&
                                           fn_imgsys_get_loc != nullptr;
    void* group = can_draw_original_button ? fn_imgsys_get_group(0xf) : nullptr;
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected) || g_module_view_installed) {
        return;
    }

    const int bag = g_virtual_bag_state.selected;
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        const int column = slot % 4;
        const int row = slot / 4;
        const UiRect rect{kGridX + column * kGridStep, kGridY + row * kGridStep,
                          kGridCell, kGridCell};
        const bool occupied = slot < capacity &&
                              g_virtual_bag_state.items[bag][slot].category > 0 &&
                              g_virtual_bag_state.items[bag][slot].count > 0;
        const bool dragging_source = g_extension_drag.active &&
                                     g_extension_drag.bag == bag &&
                                     g_extension_drag.slot == slot;
        if (can_draw_original_button && group != nullptr) {
            void* slot_background = fn_imgsys_get_loc(0xf, 0x29);
            if (slot_background != nullptr) {
                fn_grpx_draw_part(group, static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                                  slot_background, 0, 1, 0);
            }
        }
        if (occupied && !dragging_source && fn_item_draw_porting != nullptr) {
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) {
                // ITEM_DrawPorting applies the original +5 inset, rarity
                // frame, icon mapping and stack count itself.
                fn_item_draw_porting(item, static_cast<int32_t>(rect.x),
                                     static_cast<int32_t>(rect.y), 0, 1);
            }
        }
        if (g_virtual_bag_state.inspected == slot && fn_grpx_fill_rect != nullptr) {
            constexpr int32_t kInspectBorder = 3;
            constexpr uint32_t kInspectColor = 0xff75d8ff;
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                              kGridCell, kInspectBorder, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x),
                              static_cast<int32_t>(rect.y + kGridCell - kInspectBorder),
                              kGridCell, kInspectBorder, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                              kInspectBorder, kGridCell, kInspectColor);
            fn_grpx_fill_rect(static_cast<int32_t>(rect.x + kGridCell - kInspectBorder),
                              static_cast<int32_t>(rect.y), kInspectBorder, kGridCell,
                              kInspectColor);
        }
    }
    if (g_extension_drag.active && g_extension_drag.bag == bag &&
        fn_item_draw_porting != nullptr) {
        void* item = module_item_locked(bag, g_extension_drag.slot);
        if (item != nullptr) {
            const int32_t ghost_x = static_cast<int32_t>(g_extension_drag.current_x - kGridCell / 2);
            const int32_t ghost_y = static_cast<int32_t>(g_extension_drag.current_y - kGridCell / 2);
            fn_item_draw_porting(item, ghost_x, ghost_y, 0, 1);
        }
    }
}

int original_bag_locked();

uint32_t* bag_size_word_locked(int bag) {
    if (g_base == 0) return nullptr;
    void*** table_slot = reinterpret_cast<void***>(g_base + G_BAG_TABLE_VMA);
    if (table_slot == nullptr || *table_slot == nullptr) return nullptr;
    if (bag < 0 || bag >= 6) return nullptr;
    void* bag_object = (*table_slot)[bag];
    if (bag_object == nullptr) return nullptr;
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bag_object) + 0x10);
}

uint32_t* original_bag_size_word_locked() {
    return bag_size_word_locked(original_bag_locked());
}

void refresh_projection_if_overwritten_locked();

void refresh_module_item_area_locked(int index) {
    if (fn_ui_equip_refresh_item_area == nullptr) return;
    uint32_t* size_word = g_original_bag_size_word;
    if (size_word == nullptr) {
        fn_ui_equip_refresh_item_area();
        refresh_projection_if_overwritten_locked();
        return;
    }
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (g_original_bag_size & ~kCapacityMask) |
                 (static_cast<uint32_t>(g_virtual_bag_state.capacities[index]) & kCapacityMask);
    fn_ui_equip_refresh_item_area();
    refresh_projection_if_overwritten_locked();
}

void virtual_bag_draw_original_bag_wrapper() {
    const OriginalDrawInvenBagFn original =
        reinterpret_cast<OriginalDrawInvenBagFn>(g_base + fn_resolve("F_UIEQUIP_DRAW_INVEN_BAG_VMA",
                                                                     F_UIEQUIP_DRAW_INVEN_BAG_VMA));
    if (original == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    const bool module_view = g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
                             virtual_bag::valid_index(g_virtual_bag_state.selected);
    static int last_module_view = -1;
    if (last_module_view != static_cast<int>(module_view)) {
        log_exit_trace_locked("draw_bag", 0, 0, 0);
        last_module_view = module_view ? 1 : 0;
    }
    // 进入扩展选中态（kModule）时，原版袋标签全部取消高亮（互斥）：
    // 画原版 DrawInvenBag 前把当前袋 GOT 临时设为 kNoOriginalBagSelected(6)，
    // 画完恢复。此前只在 !installed 分支屏蔽，installed 分支漏掉了。
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool masked = false;
    if (module_view && current_bag != nullptr && *current_bag != nullptr) {
        saved_current = **current_bag;
        **current_bag = kNoOriginalBagSelected;
        masked = true;
    }
    if (module_view && g_module_view_installed) {
        original();
        if (masked) **current_bag = saved_current;
        draw_tab_buttons_in_frame_locked();
        refresh_projection_if_overwritten_locked();
        return;
    }
    original();
    if (masked) **current_bag = saved_current;
    draw_tab_buttons_in_frame_locked();
}
void virtual_bag_draw_inven_item_wrapper() {
    const OriginalDrawInvenItemFn original =
        reinterpret_cast<OriginalDrawInvenItemFn>(g_base + fn_resolve("F_UIEQUIP_DRAW_INVEN_ITEM_VMA",
                                                                       F_UIEQUIP_DRAW_INVEN_ITEM_VMA));
    if (original == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        if (g_module_view_installed) {
            original();
            return;
        }
        // Extension items occupy the original inventory rectangle without
        // entering g_inven. Mask the original bag only for this draw call so
        // the underlying ControlItem visuals do not show through the overlay.
        uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
        uint8_t saved_current = 0;
        bool masked_for_draw = false;
        if (current_bag != nullptr && *current_bag != nullptr &&
            **current_bag != kNoOriginalBagSelected) {
            saved_current = **current_bag;
            **current_bag = kNoOriginalBagSelected;
            masked_for_draw = true;
        }
        original();
        if (masked_for_draw) **current_bag = saved_current;
        return;
    }
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    uint8_t saved_current = 0;
    bool restored_for_draw = false;
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule &&
        g_exit_display_bag < kNoOriginalBagSelected && current_bag != nullptr &&
        *current_bag != nullptr && **current_bag == kNoOriginalBagSelected) {
        saved_current = **current_bag;
        **current_bag = g_exit_display_bag;
        restored_for_draw = true;
    }
    original();
    if (restored_for_draw) **current_bag = saved_current;
}

int original_bag_locked() {
    if (g_base == 0) return 0;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr && **current_bag < 6) {
        return **current_bag;
    }
    const uint8_t direct = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    if (direct < 6) return direct;
    return 0;
}

void set_original_bag_locked(int bag) {
    if (g_base == 0 || bag < 0 || bag >= 6) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = static_cast<uint8_t>(bag);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = static_cast<uint8_t>(bag);
    log_exit_trace_locked("write_set", 0, 0, 0);
}

bool original_got_bag_locked(int* bag) {
    if (bag == nullptr || g_base == 0) return false;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag == nullptr || *current_bag == nullptr || **current_bag >= kNoOriginalBagSelected) {
        return false;
    }
    *bag = **current_bag;
    return true;
}

void clear_original_bag_selection_locked() {
    if (g_base == 0) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA) = kNoOriginalBagSelected;
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag != nullptr && *current_bag != nullptr) **current_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("write_clear", 0, 0, 0);
}

void clear_original_desc_locked() {
    restore_extension_desc_popup_callbacks_locked();
    reset_extension_desc_unequip_hook_locked();
    reset_extension_desc_item_hooks_locked();
    g_extension_desc_item = nullptr;
    g_extension_desc_item_bag = -1;
    g_extension_desc_item_slot = -1;
    if (g_base == 0) return;
    *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_DESC_TYPE_VMA) = 0;
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
}

int original_bag_button_index(int64_t x, int64_t y) {
    // Original bag tabs are laid out by the game under the bag container. Read
    // their live child rects instead of assuming the 1408px reference layout;
    // this follows density, resolution, and runtime panel repositioning.
    if (g_base == 0 || fn_control_object_get_child == nullptr || fn_ctrl_get_count == nullptr) {
        return -1;
    }
    void* bag_container = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x50);
    if (bag_container == nullptr) return -1;
    const int child_count = static_cast<int>(fn_ctrl_get_count(bag_container));
    // 原版标签包含扩展事务袋之外的任务袋（索引 5）；不能使用
    // virtual_bag::kBagCount（5）作为原版控件扫描上限。
    const int original_tab_count = static_cast<int>(kNoOriginalBagSelected);
    const int limit = child_count < original_tab_count ? child_count : original_tab_count;
    for (int index = 0; index < limit; ++index) {
        void* control = valid_child_locked(bag_container, index);
        if (control == nullptr || extension_tab_index(control) >= 0) continue;
        int64_t absolute_x = 0;
        int64_t absolute_y = 0;
        extension_bag_control_abs_pos(control, &absolute_x, &absolute_y);
        uint8_t* data = static_cast<uint8_t*>(control);
        const int64_t width = *reinterpret_cast<int64_t*>(data + CO_RECT_W);
        const int64_t height = *reinterpret_cast<int64_t*>(data + CO_RECT_H);
        if (width > 0 && height > 0 && x >= absolute_x && x < absolute_x + width &&
            y >= absolute_y && y < absolute_y + height) {
            return index;
        }
    }
    return -1;
}

bool bind_original_exit_display_bag_locked(int bag) {
    if (g_base == 0 || bag < 0 || bag >= 6 || fn_ui_equip_refresh_item_area == nullptr) return false;
    uint8_t* direct_bag = reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
    uint8_t** current_bag = reinterpret_cast<uint8_t**>(g_base + G_UIEQUIP_CUR_BAG_GOT_VMA);
    if (current_bag == nullptr || *current_bag == nullptr) return false;
    const uint8_t saved_direct = *direct_bag;
    const uint8_t saved_got = **current_bag;
    *direct_bag = static_cast<uint8_t>(bag);
    **current_bag = static_cast<uint8_t>(bag);
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    const uint32_t* size_word = bag_size_word_locked(bag);
    const bool has_capacity = size_word != nullptr && (*size_word & kCapacityMask) != 0;
    if (has_capacity) fn_ui_equip_refresh_item_area();
    *direct_bag = saved_direct;
    **current_bag = saved_got;
    return has_capacity;
}

void* module_item_locked(int bag, int slot) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount) return nullptr;
    const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    if (descriptor.category <= 0 || descriptor.count <= 0) return nullptr;
    const uint32_t descriptor_hash = virtual_bag::payload_hash(descriptor);
    void* item = g_module_objects[bag][slot];
    if (item == nullptr || g_module_object_categories[bag][slot] != descriptor.category ||
        g_module_object_hashes[bag][slot] != descriptor_hash) {
        if (!free_module_object_locked(bag, slot)) {
            VIRTBAG_LOG("module materialize rejected stale object bag=%d slot=%d item=%p",
                        bag, slot, item);
            return nullptr;
        }
        item = materialize_module_item_locked(descriptor);
        if (item == nullptr) return nullptr;
        uint32_t handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &handle) == ownership::Outcome::kOk) {
            g_module_object_handles[bag][slot] = handle;  // module-owned（G-13）
        }
        if (!module_object_generation_advance_locked(bag, slot, "materialize replacement")) {
            VIRTBAG_LOG("module materialize replacement rejected bag=%d slot=%d", bag, slot);
            return nullptr;
        }
        g_module_objects[bag][slot] = item;
        g_module_object_categories[bag][slot] = descriptor.category;
        g_module_object_hashes[bag][slot] = descriptor_hash;
    }
    return item;
}

// 保存门禁（P3 事故修复）：SAVE_SaveInventory 被游戏内部直调（存档面板/自动存档），
// 投影安装期间执行会把 INVEN 投影态写进存档（E-2026-08-29-02 污染事故根因）。
// 通过替换 SAVE_Save 内唯一 bl 调用点，保存前强制把投影写回快照。
typedef uint32_t (*SaveInventoryRawFn)(uint8_t* cursor);
uint32_t save_inventory_wrapper(uint8_t* cursor) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_view_installed) {
            VIRTBAG_LOG("save gate: restoring projected view before SAVE_SaveInventory");
            restore_module_view_locked();
        }
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_SAVE_SAVE_INVENTORY_VMA",
                                                    F_SAVE_SAVE_INVENTORY_VMA)
                              : 0;
    if (raw == 0) return 0;
    return reinterpret_cast<SaveInventoryRawFn>(raw)(cursor);
}

// G-6 终极绘制门禁：ControlItem_Draw 的 bl ITEM_DrawPorting（0xaaf28）替换——
// 投影期间任何控件 data[0] 悬空（desc 残留/重建窗口/释放时序）都不再崩游戏：
// 物品指针必须命中已知集合（模块缓存/隔离区/INVEN_pItem）才放行绘制。
bool item_pointer_known_locked(void* item);

uint32_t item_draw_porting_gate(void* item, int32_t x, int32_t y, int32_t a, int32_t b, int32_t c) {
    if (g_module_view_installed && item != nullptr && g_inven != nullptr) {
        // 保留原版区域绘制以维护格子/Active 状态，但扩展视图下不绘制
        // INVEN 中的原版物品；否则投影控件重绑的间隙会露出原版背包一帧。
        // 扩展对象不在 g_inven 中，仍由下面的合法性门禁正常绘制。
        void** inventory = static_cast<void**>(g_inven);
        for (int idx = 0; idx < 6 * virtual_bag::kSlotCount; ++idx) {
            if (inventory[idx] == item) return 0;
        }
    }
    if (g_module_view_installed && !item_pointer_known_locked(item)) {
        VIRTBAG_LOG("draw gate: suppressed unknown item=%p", item);
        return 0;
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_ITEM_DRAW_PORTING_VMA",
                                                    F_ITEM_DRAW_PORTING_VMA)
                              : 0;
    if (raw == 0) return 0;
    typedef uint32_t (*DrawPortingFn)(void*, int32_t, int32_t, int32_t, int32_t, int32_t);
    return reinterpret_cast<DrawPortingFn>(raw)(item, x, y, a, b, c);
}

// G-6/G-7 drop 门禁 → G-8 路由：bag proc event 0x04 落袋写入（b8cc0 bl）——
// 投影期间拖动投影物品松手到这里：路由到 ext→orig 事务（真实移动），借出对象
// 不经原版 SaveItemOnEmpty 写 INVEN。非投影拖动（installed=false）直通原版。
// G-8 源识别：drop 事件的物品指针与模块缓存比对，反查扩展袋/槽。
// 事件参数自带物品指针（SaveItemOnEmpty 的 item / 0x02 的 src 控件 data），
// 无需模块维护拖动状态机——TouchHandle 全权处理拖动建立与拖影。
bool module_slot_of_item_locked(void* item, int* out_bag, int* out_slot) {
    if (item == nullptr) return false;
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

int32_t save_item_on_empty_gate(void* item, int32_t bag) {
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        int src_bag = -1, src_slot = -1;
        const bool module_item = module_slot_of_item_locked(item, &src_bag, &src_slot);
        const bool session_source_match =
            module_item && g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle &&
            g_extension_drag_session.source_bag == src_bag &&
            g_extension_drag_session.source_slot == src_slot &&
            virtual_bag::drag_session_generation_current(
                g_extension_drag_session, g_extension_tab_generation);
        if (session_source_match && virtual_bag::valid_original_transaction_bag(bag)) {
            if (g_extension_drag_session.target_kind != virtual_bag::DragTargetKind::kInvalid &&
                (g_extension_drag_session.target_kind !=
                     virtual_bag::DragTargetKind::kOriginalBag ||
                 g_extension_drag_session.target_bag != bag)) {
                VIRTBAG_LOG("drop gate: conflicting deferred target old=%d/%d new=%d",
                            static_cast<int>(g_extension_drag_session.target_kind),
                            g_extension_drag_session.target_bag, bag);
                return 0;
            }
            g_extension_drag_session.target_kind = virtual_bag::DragTargetKind::kOriginalBag;
            g_extension_drag_session.target_bag = bag;
            g_extension_drag_session.target_slot = -1;
            VIRTBAG_LOG("drop deferred ext->orig source=%d/%d bag=%d",
                        src_bag, src_slot, bag);
            // 返回 0（而非伪成功 1）：反汇编证实（b8cc8 cbz）原版仅在返回 0 时
            // 跳过「清 INVEN[当前袋][控件槽号] + 控件重绑」——返回 1 会让游戏
            // 在事务尚未运行前毁掉同槽号的真实原版物品。真实移动完全由 0x18
            // 之后的 route_deferred_extension_to_original_locked 执行。
            return 0;
        }
        if (g_module_view_installed) {
            if (!module_item) {
                return reinterpret_cast<InvenSaveItemOnEmptyFn>(
                    g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA",
                                        F_INVEN_SAVE_ITEM_ON_EMPTY_VMA))(item, bag);
            }
            VIRTBAG_LOG("drop gate: source/session mismatch src=%d/%d bag=%d phase=%d",
                        src_bag, src_slot, bag,
                        static_cast<int>(g_extension_drag_session.phase));
            return 0;
        }
    }
    const uintptr_t raw = g_base != 0
                              ? g_base + fn_resolve("F_INVEN_SAVE_ITEM_ON_EMPTY_VMA",
                                                    F_INVEN_SAVE_ITEM_ON_EMPTY_VMA)
                              : 0;
    if (raw == 0) return 0;
    return reinterpret_cast<InvenSaveItemOnEmptyFn>(raw)(item, bag);
}

// 正式窗口投影（P3 方案 C，控件级）：只写控件不写 INVEN——
//   容量字（袋对象 +0x10）临时写扩展容量驱动 RefreshItemArea/DrawInvenBag 的容量语义；
//   RefreshItemArea 按容量字 SetActive/SetShow 容量外控件（隐藏）；
//   容量内控件 ControlItem_SetItem(借出对象) 显示扩展物品；
//   INVEN_pItem 全程真实（API/存档/捡取读数无污染），restore 仅写回容量字 + RefreshItemArea。
bool install_module_view_locked(int bag) {
    if (g_base == 0 || !virtual_bag::valid_index(bag)) return false;
    if (g_virtual_bag_state.capacities[bag] == 0) return false;
    if (fn_control_item_set_item == nullptr || fn_control_object_get_child == nullptr) return false;
    const int original_bag = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(original_bag)) {
        VIRTBAG_LOG("module view reject install: original window bag=%d (task bag reserved)",
                    original_bag);
        return false;
    }
    uint32_t* size_word = bag_size_word_locked(original_bag);
    if (size_word == nullptr) return false;

    if (g_module_view_installed) restore_module_view_locked();
    clear_original_desc_locked();  // 原版详情面板不跨视图残留

    g_original_bag_size_word = size_word;
    g_original_bag_size = *size_word;
    g_module_window_original_bag = original_bag;
    const int capacity = g_virtual_bag_state.capacities[bag];
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    *size_word = (*size_word & ~kCapacityMask) |
                 (static_cast<uint32_t>(capacity) & kCapacityMask);

    void* root = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
    g_projected_item_root = root;
    if (root != nullptr && fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();  // 按新容量禁用容量外控件 + 刷 INVEN 原版物品
        for (int slot = 0; slot < capacity; ++slot) {
            void* ctrl = valid_child_locked(root, slot);
            if (ctrl == nullptr) continue;
            void* item = module_item_locked(bag, slot);
            // 空槽也必须 SetItem(nullptr)：RefreshItemArea 刚把 INVEN 原版物品刷进控件，
            // 扩展袋空位不覆盖的话会残留原版物品显示。
            fn_control_item_set_item(ctrl, item);
            if (item != nullptr) {
                ownership::borrow_for_view(&g_ownership_ledger,
                                           g_module_object_handles[bag][slot]);
            }
        }
    }
    g_module_view_installed = true;
    g_module_view_index = bag;
    VIRTBAG_LOG("module view installed bag=%d window=%d capacity=%d", bag, original_bag, capacity);
    return true;
}

// 帧级自愈：RefreshItemArea 被游戏逻辑触发时会把控件刷回 INVEN 原版物品，
// installed 状态下每帧检测并重投影（draw wrapper 已持锁）。
void refresh_projection_if_overwritten_locked() {
    if (!g_module_view_installed || g_projected_item_root == nullptr ||
        fn_control_object_get_data == nullptr || fn_control_item_set_item == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) {
        return;
    }
    const int bag = g_module_view_index;
    const int capacity = g_virtual_bag_state.capacities[bag];
    static int heal_count = 0;
    for (int slot = 0; slot < capacity; ++slot) {
        void* item = g_module_objects[bag][slot];  // 空槽为 nullptr，同样需要清空控件
        void* ctrl = valid_child_locked(g_projected_item_root, slot);
        if (ctrl == nullptr) continue;
        void* data = fn_control_object_get_data(ctrl);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != item) {
            ++heal_count;
            fn_control_item_set_item(ctrl, item);
            VIRTBAG_LOG("projection heal #%d slot=%d current=%p item=%p",
                        heal_count, slot, current, item);
        }
    }
}

void refresh_projected_module_view_after_move_locked() {
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index)) return;
    // 原版 UIEquip_InvenItemControlEventProc 在移动成功后先刷新区域：该调用除了
    // SetItem，还会按当前容量更新每个槽的 Active/Show 状态；随后再覆盖回扩展对象。
    refresh_module_item_area_locked(g_module_view_index);
    refresh_projection_if_overwritten_locked();
}

void restore_module_view_locked() {
    if (!g_module_view_installed || g_base == 0) return;
    // P4.3：视图借出逐槽归还（安装时借出的槽位必须成对归还，窗口外借用为零）。
    if (virtual_bag::valid_index(g_module_view_index)) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const uint32_t handle = g_module_object_handles[g_module_view_index][slot];
            if (ownership::live_state(g_ownership_ledger, handle,
                                      ownership::State::kBorrowedForView)) {
                ownership::return_from_view(&g_ownership_ledger, handle);
            }
        }
        log_ownership_audit_locked("restore_module_view");
    }
    clear_original_desc_locked();  // 关闭原版详情面板（二次点击 MakeDesc 打开）
    if (g_original_bag_size_word != nullptr) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        *g_original_bag_size_word =
            (*g_original_bag_size_word & ~kCapacityMask) |
            (g_original_bag_size & kCapacityMask);
    }
    if (fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();  // 容量字已还原：原版逻辑自动恢复控件（INVEN 全程真实）
    }
    g_original_bag_size_word = nullptr;
    g_original_bag_size = 0;
    g_module_window_original_bag = -1;
    g_projected_item_root = nullptr;
    g_module_view_installed = false;
    g_module_view_index = -1;
    log_exit_trace_locked("write_restore", 0, 0, 0);
}

bool complete_original_exit_locked() {
    int selected_original_bag = 0;
    if (!original_got_bag_locked(&selected_original_bag)) {
        log_exit_trace_locked("complete_invalid_got", 0, 0, 0);
        return false;
    }
    set_original_bag_locked(selected_original_bag);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    virtual_bag::enter_original(&g_virtual_bag_state, selected_original_bag);
    persist_state_locked();
    g_exit_display_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("complete_success", 0, 0, 0);
    return true;
}

void cancel_original_exit_locked() {
    const int original_bag = g_original_current_got < kNoOriginalBagSelected ?
                             g_original_current_got : g_original_current_direct;
    set_original_bag_locked(original_bag);
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
    virtual_bag::enter_original(&g_virtual_bag_state, original_bag);
    persist_state_locked();
    g_exit_display_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("complete_cancel", 0, 0, 0);
}

void virtual_bag_f3_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        const bool was_exiting_module = g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule;
        if (g_module_view_installed) restore_module_view_locked();
        if (!was_exiting_module && g_virtual_bag_state.mode != virtual_bag::Mode::kOriginal) {
            virtual_bag::enter_original(&g_virtual_bag_state, original_bag_locked());
        }
        if (was_exiting_module) {
            cancel_original_exit_locked();
        }
        g_exit_display_bag = kNoOriginalBagSelected;
        g_inventory_frame_active = false;
        g_extension_touch_capture = false;
        g_extension_drag = {};
        cancel_projected_drag_session_locked("f3");
        disable_extension_tab_buttons_locked();
        log_exit_trace_locked("f3", 0, 0, 0);
        original = g_orig_f3;
    }
    if (original != nullptr) original();
}

void virtual_bag_inventory_enter_wrapper() {
    PopupNoArgFn original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        clear_original_desc_locked();
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
        g_extension_touch_capture = false;
        g_extension_drag = {};
        cancel_projected_drag_session_locked("inventory-enter");
        disable_extension_tab_buttons_locked();
        persist_state_locked();
        original = g_orig_enter;
    }
    if (original != nullptr) original();
    if (!g_virtual_bag_enabled.load()) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    install_extension_tab_buttons_locked();
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
}

void virtual_bag_save_panel_enter_wrapper() {
    VIRTBAG_LOG("save panel enter wrapper invoked world=%d hook=%d",
                game_in_world() ? 1 : 0, g_save_panel_hook_installed ? 1 : 0);
    if (game_in_world()) {
        module_save_game();
    }
    if (g_orig_save_enter != nullptr) g_orig_save_enter();
}

void virtual_bag_draw_end_wrapper() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_state != nullptr && *reinterpret_cast<uint16_t*>(g_state) != 5) {
        if (!prepare_main_menu_locked()) {
            g_main_menu_cleanup_dirty.store(true, std::memory_order_release);
            VIRTBAG_LOG("draw-end main-menu cleanup failed; retry scheduled");
        } else {
            g_main_menu_cleanup_dirty.store(false, std::memory_order_release);
        }
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    if (g_gamestate != nullptr && *reinterpret_cast<uint32_t*>(g_gamestate) != 0) {
        restore_module_view_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    if (!g_inventory_frame_active) {
        restore_module_view_locked();
        set_original_bag_locked(0);
        virtual_bag::enter_original(&g_virtual_bag_state, 0);
        g_inventory_frame_active = true;
    }
    if (!g_virtual_bag_enabled.load()) {
        restore_module_view_locked();
        if (fn_grpx_end != nullptr) fn_grpx_end();
        return;
    }
    ensure_state_loaded_locked();
    commit_pending_extension_tab_locked();
    // 投影常驻（P2/P3）：module 视图有效期间保持安装；仅在状态不再指向
    // 模块视图（退出/切档/异常）时兜底恢复，防止遗留投影污染原版窗口。
    const bool module_view_expected =
        g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected);
    if (g_module_view_installed && !module_view_expected) restore_module_view_locked();
    if (g_module_view_installed) {
        // TouchHandle owns the native drag shadow. Keep a verified projected control
        // alive across frames; all other stale moving controls are still cleared.
        if (g_base != 0) {
            uint8_t* touch_state = reinterpret_cast<uint8_t*>(g_base + G_TOUCH_STATE_VMA);
            void* moving = *reinterpret_cast<void**>(touch_state + TOUCH_STATE_MOVING_CTRL);
            bool projected_moving = false;
            if (moving != nullptr && virtual_bag::valid_index(g_module_view_index) &&
                g_projected_item_root != nullptr) {
                const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
                for (int slot = 0; slot < capacity; ++slot) {
                    if (valid_child_locked(g_projected_item_root, slot) == moving) {
                        projected_moving = true;
                        break;
                    }
                }
            }
            if (!projected_moving) {
                *reinterpret_cast<void**>(touch_state + TOUCH_STATE_MOVING_CTRL) = nullptr;
            }
        }
        refresh_projection_if_overwritten_locked();
    }
    if (g_module_view_installed) {
        refresh_projection_if_overwritten_locked();
    }
    static int last_mode = -1;
    static int last_selected = -2;
    static int last_overlay = -1;
    static int last_overlay_index = -2;
    if (last_mode != static_cast<int>(g_virtual_bag_state.mode) ||
        last_selected != g_virtual_bag_state.selected ||
        last_overlay != static_cast<int>(g_module_view_installed) ||
        last_overlay_index != g_module_view_index) {
        log_exit_trace_locked("draw_end", 0, 0, 0);
        last_mode = static_cast<int>(g_virtual_bag_state.mode);
        last_selected = g_virtual_bag_state.selected;
        last_overlay = g_module_view_installed ? 1 : 0;
        last_overlay_index = g_module_view_index;
    }
    draw_cells_in_frame_locked();
    if (fn_grpx_end != nullptr) fn_grpx_end();
}

// thunk 页登记：每页 4096B 承载 28B thunk，剩余空间可复用（页必在 libgame
// 调用点 ±128MB 内，多挂钩共享免再扫地址空间；粗扫粒度下零星单页空洞会
// 耗尽——真机实证：第 7 个挂钩因分配失败拖垮整个注入链）。
std::array<void*, 8> g_thunk_pages{};
size_t g_thunk_page_count = 0;
size_t g_thunk_page_used = 0;  // 当前页已用槽位数（每槽 32B 对齐）

void* allocate_draw_thunk(uintptr_t call_addr, uintptr_t wrapper) {
#ifndef MAP_FIXED_NOREPLACE
    (void)call_addr;
    (void)wrapper;
    return nullptr;
#else
    constexpr size_t kPageSize = 4096;
    constexpr size_t kThunkSlot = 32;
    const auto emit_thunk = [](void* slot, uintptr_t wrapper_addr) {
        uint32_t code[] = {
            0xa9bf7bf0, // stp x16, x30, [sp, #-16]!
            0x58000090, // ldr x16, #16 (literal at thunk+20)
            0xd63f0200, // blr x16
            0xa8c17bf0, // ldp x16, x30, [sp], #16
            0xd65f03c0, // ret
        };
        memcpy(slot, code, sizeof(code));
        *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(slot) + 20) = wrapper_addr;
        __builtin___clear_cache(reinterpret_cast<char*>(slot),
                                reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(slot) + kThunkSlot));
    };
    // 1) 已有页 carving（校验对本调用点的 BL 可达性：页可能在别的调用点
    // ±128MB 边缘，libgame 跨度 ~20MB 时存在不可达组合）。
    if (g_thunk_page_count > 0) {
        void* page = g_thunk_pages[g_thunk_page_count - 1];
        void* slot = reinterpret_cast<uint8_t*>(page) + g_thunk_page_used * kThunkSlot;
        const int64_t reach = static_cast<int64_t>(reinterpret_cast<uintptr_t>(slot)) -
                              static_cast<int64_t>(call_addr);
        if ((g_thunk_page_used + 1) * kThunkSlot <= kPageSize && reach > -0x08000000LL &&
            reach < 0x08000000LL) {
            ++g_thunk_page_used;
            emit_thunk(slot, wrapper);
            return slot;
        }
    }
    // 2) 两级扫描新页：先 64KB 粗扫（快），失败后 4KB 细扫兜底。
    for (const int64_t coarse : {1, 0}) {
        const int64_t kStep = coarse ? 0x00010000 : 0x1000;
        const uintptr_t base = call_addr & ~(static_cast<uintptr_t>(kPageSize) - 1);
        for (int64_t distance = kStep; distance < 0x08000000; distance += kStep) {
            for (int sign : {1, -1}) {
                const int64_t candidate_signed = static_cast<int64_t>(base) + sign * distance;
                if (candidate_signed <= 0) continue;
                void* region = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate_signed)), kPageSize,
                                    PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (region == MAP_FAILED) continue;
                emit_thunk(region, wrapper);
                if (g_thunk_page_count < g_thunk_pages.size()) {
                    g_thunk_pages[g_thunk_page_count++] = region;
                    g_thunk_page_used = 1;
                }
                return region;
            }
        }
    }
    return nullptr;
#endif
}
