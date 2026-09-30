void draw_tab_buttons_in_frame_locked() {
    // 配置门控：扩展背包关闭时不得绘制/重装页签，并清除已挂载的页签控件。
    if (!g_virtual_bag_enabled.load()) {
        if (g_extension_tab_root != nullptr || g_extension_tab_buttons[0] != nullptr) {
            delete_extension_tab_buttons_locked();
            disable_extension_tab_buttons_locked();
        }
        return;
    }
    const bool can_draw_original_button = extension_bag_ui_can_draw();
    // 标签挂袋容器（与原版 6 袋标签同父）：比对基准同步为袋容器句柄。
    void* current_bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    // 容器就位重试：enter 时 install 可能因占位容器（child count<6）被跳过；
    // 每帧检测真容器就位且尚未挂载（root 变化）时重新 install。
    int child_count = 0;
    if (current_bag_container != nullptr && extension_bag_ui_child_count(current_bag_container, &child_count) &&
        child_count >= 6 && current_bag_container != g_extension_tab_root) {
        install_extension_tab_buttons_locked();
        current_bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    }
    const bool tabs_are_current =
        current_bag_container != nullptr && current_bag_container == g_extension_tab_root &&
        g_extension_tab_generation == g_inventory_generation;
    if (!tabs_are_current || !can_draw_original_button) return;
    // 第一遍：底框 loc20（72x84），倒序 index 4→0（对齐原版 DrawInvenBackground 的 5→0）。
    for (int index = virtual_bag::kBagCount - 1; index >= 0; --index) {
        void* button = g_extension_tab_buttons[index];
        if (button == nullptr) continue;
        int64_t ax = 0, ay = 0;
        extension_bag_control_abs_pos(button, &ax, &ay);
        (void)extension_bag_ui_draw_part(0xf, 0x14, static_cast<int32_t>(ax - 5),
                                         static_cast<int32_t>(ay - 12), 0, 1);
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
            (void)extension_bag_ui_draw_part(0xf, 0x13, static_cast<int32_t>(ax - 5),
                                             static_cast<int32_t>(ay - 12), 0, 1);
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 1);
        } else if (equipped) {
            (void)extension_bag_ui_draw_part(0xf, 6, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 0x28);
        } else {
            (void)extension_bag_ui_draw_part(0xf, 9, static_cast<int32_t>(ax + 5),
                                             static_cast<int32_t>(ay + 5), 0, 0x28);
        }
    }
}

void draw_cells_in_frame_locked() {
    const bool can_draw_original_button = extension_bag_ui_can_draw();
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
        if (can_draw_original_button) {
            (void)extension_bag_ui_draw_part(0xf, 0x29, static_cast<int32_t>(rect.x),
                                             static_cast<int32_t>(rect.y), 0, 1);
        }
        if (occupied && !dragging_source) {
            void* item = module_item_locked(bag, slot);
            if (item != nullptr) {
                // ITEM_DrawPorting applies the original +5 inset, rarity
                // frame, icon mapping and stack count itself.
                (void)extension_bag_ui_draw_item(item, static_cast<int32_t>(rect.x),
                                                 static_cast<int32_t>(rect.y), 0, 1);
            }
        }
        if (g_virtual_bag_state.inspected == slot) {
            constexpr int32_t kInspectBorder = 3;
            constexpr uint32_t kInspectColor = 0xff75d8ff;
            (void)extension_bag_ui_fill_rect({rect.x, rect.y, kGridCell, kInspectBorder}, kInspectColor);
            (void)extension_bag_ui_fill_rect(
                {rect.x, rect.y + kGridCell - kInspectBorder, kGridCell, kInspectBorder}, kInspectColor);
            (void)extension_bag_ui_fill_rect({rect.x, rect.y, kInspectBorder, kGridCell}, kInspectColor);
            (void)extension_bag_ui_fill_rect(
                {rect.x + kGridCell - kInspectBorder, rect.y, kInspectBorder, kGridCell}, kInspectColor);
        }
    }
    if (g_extension_drag.active && g_extension_drag.bag == bag) {
        void* item = module_item_locked(bag, g_extension_drag.slot);
        if (item != nullptr) {
            const int32_t ghost_x = static_cast<int32_t>(g_extension_drag.current_x - kGridCell / 2);
            const int32_t ghost_y = static_cast<int32_t>(g_extension_drag.current_y - kGridCell / 2);
            (void)extension_bag_ui_draw_item(item, ghost_x, ghost_y, 0, 1);
        }
    }
}

int original_bag_locked();

bool bag_size_word_locked(int bag, uint32_t* out_word) {
    return g_game != nullptr && g_write_port->ui.bag_capacity_word != nullptr &&
           out_word != nullptr && bag >= 0 && bag < 6 &&
           qol::contracts::succeeded(
               g_write_port->ui.bag_capacity_word(bag, out_word));
}

bool set_bag_size_word_locked(int bag, uint32_t word) {
    return g_game != nullptr && g_write_port->ui.set_bag_capacity_word != nullptr &&
           bag >= 0 && bag < 6 &&
           qol::contracts::succeeded(
               g_write_port->ui.set_bag_capacity_word(bag, word));
}

void refresh_projection_if_overwritten_locked();

void refresh_module_item_area_locked(int index) {
    if (g_game == nullptr || g_write_port->ui.refresh_item_area == nullptr) return;
    if (g_original_bag_size_bag < 0) {
        virtual_bag_call_original_refresh_item_area_with_guard();
        return;
    }
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    (void)set_bag_size_word_locked(
        g_original_bag_size_bag,
        (g_original_bag_size & ~kCapacityMask) |
            (static_cast<uint32_t>(g_virtual_bag_state.capacities[index]) & kCapacityMask));
    virtual_bag_call_original_refresh_item_area_with_guard();
    refresh_projection_if_overwritten_locked();
}

void virtual_bag_draw_original_bag_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_INVEN_BAG_DRAW"});
    if (g_game == nullptr || g_write_port->ui.draw_original_bag == nullptr) return;
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
    const int saved_current = original_bag_locked();
    if (module_view && g_write_port->ui.set_current_bag != nullptr) {
        (void)g_write_port->ui.set_current_bag(kNoOriginalBagSelected);
    }
    if (module_view && g_module_view_installed) {
        (void)g_write_port->ui.draw_original_bag();
        if (module_view) (void)g_write_port->ui.set_current_bag(saved_current);
        draw_tab_buttons_in_frame_locked();
        return;
    }
    (void)g_write_port->ui.draw_original_bag();
    if (module_view) (void)g_write_port->ui.set_current_bag(saved_current);
    draw_tab_buttons_in_frame_locked();
}
void virtual_bag_draw_inven_item_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_INVEN_ITEM_DRAW"});
    if (g_game == nullptr || g_write_port->ui.draw_original_items == nullptr) return;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        if (g_module_view_installed) {
            (void)g_write_port->ui.draw_original_items();
            return;
        }
        // Extension items occupy the original inventory rectangle without
        // entering g_inven. Mask the original bag only for this draw call so
        // the underlying ControlItem visuals do not show through the overlay.
        const int saved_current = original_bag_locked();
        (void)g_write_port->ui.set_current_bag(kNoOriginalBagSelected);
        (void)g_write_port->ui.draw_original_items();
        (void)g_write_port->ui.set_current_bag(saved_current);
        return;
    }
    const int saved_current = original_bag_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule &&
        g_exit_display_bag < kNoOriginalBagSelected) {
        (void)g_write_port->ui.set_current_bag(g_exit_display_bag);
    }
    (void)g_write_port->ui.draw_original_items();
    (void)g_write_port->ui.set_current_bag(saved_current);
}

int original_bag_locked() {
    if (g_game == nullptr || g_write_port->ui.current_bag == nullptr) return 0;
    int32_t bag = 0;
    return qol::contracts::succeeded(g_write_port->ui.current_bag(&bag)) &&
                   bag >= 0 && bag < 6 ? bag : 0;
}

void set_original_bag_locked(int bag) {
    if (g_game == nullptr || g_write_port->ui.set_current_bag == nullptr ||
        bag < 0 || bag >= 6) return;
    (void)g_write_port->ui.set_current_bag(bag);
    log_exit_trace_locked("write_set", 0, 0, 0);
}

bool original_got_bag_locked(int* bag) {
    if (bag == nullptr) return false;
    const int current = original_bag_locked();
    if (current < 0 || current >= kNoOriginalBagSelected) return false;
    *bag = current;
    return true;
}

void clear_original_bag_selection_locked() {
    if (g_game == nullptr || g_write_port->ui.set_current_bag == nullptr) return;
    (void)g_write_port->ui.set_current_bag(kNoOriginalBagSelected);
    log_exit_trace_locked("write_clear", 0, 0, 0);
}

void clear_original_desc_locked() {
    restore_extension_desc_popup_callbacks_locked();
    reset_extension_desc_unequip_hook_locked();
    reset_extension_desc_item_hooks_locked();
    g_extension_desc_item = nullptr;
    g_extension_desc_item_bag = -1;
    g_extension_desc_item_slot = -1;
    if (g_game == nullptr || g_write_port->ui.set_desc_type == nullptr) return;
    (void)g_write_port->ui.set_desc_type(0);
    if (g_write_port->ui.description_set_off != nullptr) {
        (void)g_write_port->ui.description_set_off();
    }
}

int original_bag_button_index(int64_t x, int64_t y) {
    // Original bag tabs are laid out by the game under the bag container. Read
    // their live child rects instead of assuming the 1408px reference layout;
    // this follows density, resolution, and runtime panel repositioning.
    void* bag_container = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipBagContainer);
    if (bag_container == nullptr) return -1;
    int child_count = 0;
    if (!extension_bag_ui_child_count(bag_container, &child_count)) return -1;
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
        UiRect rect{};
        if (extension_bag_ui_rect(control, &rect) && rect.w > 0 && rect.h > 0 &&
            extension_bag_ui_hit_test(control, x, y, rect)) {
            return index;
        }
    }
    return -1;
}

bool bind_original_exit_display_bag_locked(int bag) {
    if (g_game == nullptr || bag < 0 || bag >= 6 ||
        g_write_port->ui.refresh_item_area == nullptr) return false;
    const int saved_bag = original_bag_locked();
    set_original_bag_locked(bag);
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    uint32_t size_word = 0;
    const bool has_capacity = bag_size_word_locked(bag, &size_word) &&
                              (size_word & kCapacityMask) != 0;
    if (has_capacity) virtual_bag_call_original_refresh_item_area_with_guard();
    set_original_bag_locked(saved_bag);
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
            QOL_LOG_WARN(QolDomain::kExtensionBag, "module materialize rejected stale object bag=%d slot=%d item=%p",
                        bag, slot, item);
            return nullptr;
        }
        item = materialize_module_item_locked(descriptor);
        if (item == nullptr) return nullptr;
        if (!extension_bag_register_item_handle(item)) {
            (void)extension_free_item(item);
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "module materialize rejected item handle registration bag=%d slot=%d",
                          bag, slot);
            return nullptr;
        }
        uint32_t handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &handle) != ownership::Outcome::kOk) {
            release_temporary_item_locked(item);
            QOL_LOG_WARN(QolDomain::kExtensionBag, "module materialize rejected ownership ledger exhausted bag=%d slot=%d",
                        bag, slot);
            return nullptr;
        }
        g_module_object_handles[bag][slot] = handle;  // module-owned（G-13）
        if (!module_object_generation_advance_locked(bag, slot, "materialize replacement")) {
            g_module_object_handles[bag][slot] = 0;
            ownership::release(&g_ownership_ledger, handle);
            release_temporary_item_locked(item);
            QOL_LOG_WARN(QolDomain::kExtensionBag, "module materialize replacement rejected bag=%d slot=%d", bag, slot);
            return nullptr;
        }
        g_module_objects[bag][slot] = item;
        g_module_object_categories[bag][slot] = descriptor.category;
        g_module_object_hashes[bag][slot] = descriptor_hash;
    }
    return item;
}

// 保存门禁（P3 事故修复）：SAVE_SaveInventory 被游戏内部直调（存档面板/自动存档），
// 投影安装期间执行会把 INVEN 投影态写进存档（污染事故根因）。
// v0.6.x：由「改写 SAVE_Save 内唯一 bl 调用点」改为「按符号 hook SAVE_SaveInventory
// 函数入口」——调用方无关（原版 SAVE_Save 或 monster mod 的包装器都经过此函数），
// 且不受 monster 改指调用目标影响。
typedef uint32_t (*SaveInventoryRawFn)(uint8_t* cursor);
uint32_t save_inventory_wrapper(uint8_t* cursor) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"hook:SAVE_SaveInventory"});
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_view_installed) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag, "save gate: restoring projected view before SAVE_SaveInventory");
            restore_module_view_locked();
        }
    }
    // 函数入口 hook：必须走框架 backup（原函数 trampoline）；按符号地址直调会重入自身。
    if (g_backup_save_inventory == nullptr) return 0;
    return reinterpret_cast<SaveInventoryRawFn>(g_backup_save_inventory)(cursor);
}

// G-6 终极绘制门禁：ControlItem_Draw 的 bl ITEM_DrawPorting（0xaaf28）替换——
// 投影期间任何控件 data[0] 悬空（desc 残留/重建窗口/释放时序）都不再崩游戏：
// 物品指针必须命中已知集合（模块缓存/隔离区/INVEN_pItem）才放行绘制。
bool item_pointer_known_locked(void* item);

uint32_t item_draw_porting_gate(void* item, int32_t x, int32_t y, int32_t a, int32_t b, int32_t c) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_DRAW_GATE"});
    if (g_module_view_installed && item != nullptr) {
        // 保留原版区域绘制以维护格子/Active 状态，但扩展视图下不绘制
        // INVEN 中的原版物品；否则投影控件重绑的间隙会露出原版背包一帧。
        // 扩展对象不在 g_inven 中，仍由下面的合法性门禁正常绘制。
        for (int idx = 0; idx < 6 * virtual_bag::kSlotCount; ++idx) {
            void* physical = nullptr;
            if (inventory_slot_locked(idx / virtual_bag::kSlotCount,
                                      idx % virtual_bag::kSlotCount, &physical) &&
                physical == item) return 0;
        }
    }
    if (g_module_view_installed && !item_pointer_known_locked(item)) {
        QOL_LOG_DEBUG(QolDomain::kExtensionBag, "draw gate: suppressed unknown item=%p", item);
        return 0;
    }
    (void)c;
    return extension_bag_ui_draw_item(item, x, y, a, b) ? 1u : 0u;
}

// G-6/G-7 drop 门禁 → G-8 路由：bag proc event 0x04 落袋写入（b8cc0 bl）——
// 投影期间拖动投影物品松手到这里：路由到 ext→orig 事务（真实移动），借出对象
// 不经原版 SaveItemOnEmpty 写 INVEN。非投影拖动（installed=false）直通原版。
int32_t save_item_on_empty_gate(void* item, int32_t bag) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_DROP_GATE"});
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
                QOL_LOG_DEBUG(QolDomain::kExtensionBag, "drop gate: conflicting deferred target old=%d/%d new=%d",
                            static_cast<int>(g_extension_drag_session.target_kind),
                            g_extension_drag_session.target_bag, bag);
                return 0;
            }
            g_extension_drag_session.target_kind = virtual_bag::DragTargetKind::kOriginalBag;
            g_extension_drag_session.target_bag = bag;
            g_extension_drag_session.target_slot = -1;
            QOL_LOG_DEBUG(QolDomain::kExtensionBag, "drop deferred ext->orig source=%d/%d bag=%d",
                        src_bag, src_slot, bag);
            // 返回 0（而非伪成功 1）：反汇编证实（b8cc8 cbz）原版仅在返回 0 时
            // 跳过「清 INVEN[当前袋][控件槽号] + 控件重绑」——返回 1 会让游戏
            // 在事务尚未运行前毁掉同槽号的真实原版物品。真实移动完全由 0x18
            // 之后的 route_deferred_extension_to_original_locked 执行。
            return 0;
        }
        if (g_module_view_installed) {
            if (!module_item) {
                if (g_game == nullptr || g_write_port->inventory.save_item_on_empty == nullptr) {
                    return 0;
                }
                int32_t result = 0;
                return qol::contracts::succeeded(
                           g_write_port->inventory.save_item_on_empty(item, bag, &result))
                           ? result : 0;
            }
            QOL_LOG_DEBUG(QolDomain::kExtensionBag, "drop gate: source/session mismatch src=%d/%d bag=%d phase=%d",
                        src_bag, src_slot, bag,
                        static_cast<int>(g_extension_drag_session.phase));
            return 0;
        }
    }
    if (g_game == nullptr || g_write_port->inventory.save_item_on_empty == nullptr) {
        return 0;
    }
    int32_t result = 0;
    return qol::contracts::succeeded(
               g_write_port->inventory.save_item_on_empty(item, bag, &result))
               ? result : 0;
}

// 正式窗口投影（P3 方案 C，控件级）：只写控件不写 INVEN——
//   容量字（袋对象 +0x10）临时写扩展容量驱动 RefreshItemArea/DrawInvenBag 的容量语义；
//   RefreshItemArea 按容量字 SetActive/SetShow 容量外控件（隐藏）；
//   容量内控件 ControlItem_SetItem(借出对象) 显示扩展物品；
//   INVEN_pItem 全程真实（API/存档/捡取读数无污染），restore 仅写回容量字 + RefreshItemArea。
bool install_module_view_locked(int bag) {
    if (g_game == nullptr || !virtual_bag::valid_index(bag)) return false;
    if (g_virtual_bag_state.capacities[bag] == 0) return false;
    const int original_bag = original_bag_locked();
    if (!virtual_bag::valid_original_transaction_bag(original_bag)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "module view reject install: original window bag=%d (task bag reserved)",
                    original_bag);
        return false;
    }
    uint32_t size_word = 0;
    if (!bag_size_word_locked(original_bag, &size_word)) return false;

    if (g_module_view_installed) restore_module_view_locked();
    clear_original_desc_locked();  // 原版详情面板不跨视图残留

    g_original_bag_size_bag = original_bag;
    g_original_bag_size = size_word;
    g_module_window_original_bag = original_bag;
    const int capacity = g_virtual_bag_state.capacities[bag];
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    (void)set_bag_size_word_locked(
        original_bag, (size_word & ~kCapacityMask) |
                          (static_cast<uint32_t>(capacity) & kCapacityMask));

    void* root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    g_projected_item_root = root;
    if (root != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();  // 按新容量禁用容量外控件 + 刷 INVEN 原版物品
        for (int slot = 0; slot < capacity; ++slot) {
            void* ctrl = valid_child_locked(root, slot);
            if (ctrl == nullptr) continue;
            void* item = module_item_locked(bag, slot);
            // 空槽也必须 SetItem(nullptr)：RefreshItemArea 刚把 INVEN 原版物品刷进控件，
            // 扩展袋空位不覆盖的话会残留原版物品显示。
            (void)extension_bag_ui_set_control_item(ctrl, item);
            if (item != nullptr) {
                ownership::borrow_for_view(&g_ownership_ledger,
                                           g_module_object_handles[bag][slot]);
            }
        }
    }
    g_module_view_installed = true;
    g_module_view_index = bag;
    QOL_LOG_INFO(QolDomain::kExtensionBag, "module view installed bag=%d window=%d capacity=%d", bag, original_bag, capacity);
    return true;
}

// H-16 post-projection：一次原版 RefreshItemArea 返回后，按当前逻辑槽重绑投影。
// 该函数不再由 draw wrapper 每帧调用；无刷新事务的调用方必须使用定点同步。
void refresh_projection_if_overwritten_locked() {
    if (!g_module_view_installed || g_projected_item_root == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) {
        return;
    }
    const int bag = g_module_view_index;
    const int capacity = g_virtual_bag_state.capacities[bag];
    for (int slot = 0; slot < capacity; ++slot) {
        void* item = g_module_objects[bag][slot];  // 空槽为 nullptr，同样需要清空控件
        void* ctrl = valid_child_locked(g_projected_item_root, slot);
        if (ctrl == nullptr) continue;
        void* data = extension_bag_ui_data(ctrl);
        void* current = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
        if (current != item) {
            (void)extension_bag_ui_set_control_item(ctrl, item);
            QOL_LOG_INFO(QolDomain::kExtensionBag, "projection post-refresh slot=%d current=%p item=%p",
                        slot, current, item);
        }
    }
}

bool sync_projected_slot_locked(int bag, int slot) {
    if (!g_module_view_installed || bag != g_module_view_index ||
        !virtual_bag::valid_index(bag) || g_projected_item_root == nullptr || slot < 0 ||
        slot >= g_virtual_bag_state.capacities[bag]) {
        return false;
    }
    void* ctrl = valid_child_locked(g_projected_item_root, slot);
    if (ctrl == nullptr) return false;
    return extension_bag_ui_set_control_item(ctrl, g_module_objects[bag][slot]);
}

void refresh_projected_module_view_after_move_locked() {
    if (!g_module_view_installed || !virtual_bag::valid_index(g_module_view_index)) return;
    // 原版 UIEquip_InvenItemControlEventProc 在移动成功后先刷新区域：该调用除了
    // SetItem，还会按当前容量更新每个槽的 Active/Show 状态；随后再覆盖回扩展对象。
    refresh_module_item_area_locked(g_module_view_index);
}

struct RefreshRestoreSuppressScope {
    bool previous;

    RefreshRestoreSuppressScope() : previous(g_refresh_restore_suppress) {
        g_refresh_restore_suppress = true;
    }

    ~RefreshRestoreSuppressScope() {
        g_refresh_restore_suppress = previous;
    }
};

void restore_module_view_locked() {
    if (!g_module_view_installed) return;
    RefreshRestoreSuppressScope suppress_refresh;
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
    if (g_original_bag_size_bag >= 0) {
        constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
        (void)set_bag_size_word_locked(
            g_original_bag_size_bag,
            (g_original_bag_size & kCapacityMask) |
                (g_original_bag_size & ~kCapacityMask));
    }
    if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();  // 容量字已还原：原版逻辑自动恢复控件（INVEN 全程真实）
    }
    g_original_bag_size_bag = -1;
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
    if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
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
    if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    virtual_bag::enter_original(&g_virtual_bag_state, original_bag);
    persist_state_locked();
    g_exit_display_bag = kNoOriginalBagSelected;
    log_exit_trace_locked("complete_cancel", 0, 0, 0);
}

void virtual_bag_f3_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"own:popup.inventory"});
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
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"own:popup.inventory"});
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
    if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
}

void virtual_bag_save_panel_enter_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"own:popup.save_panel"});
    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "save panel enter wrapper invoked world=%d hook=%d",
                read_world_in_world() ? 1 : 0, g_save_panel_hook_installed ? 1 : 0);
    if (read_world_in_world()) {
        if (g_sdk != nullptr && g_sdk->store.save_game != nullptr) {
            (void)g_sdk->store.save_game();
        }
    }
    if (g_orig_save_enter != nullptr) g_orig_save_enter();
}

void virtual_bag_draw_end_wrapper() {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_INVEN_DRAW_END"});
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    const int inventory_state = virtual_bag_inventory_state_id();
    if (inventory_state >= 0 && inventory_state != 5) {
        if (!prepare_main_menu_locked()) {
            g_main_menu_cleanup_dirty.store(true, std::memory_order_release);
            QOL_LOG_DEBUG(QolDomain::kExtensionBag, "draw-end main-menu cleanup failed; retry scheduled");
        } else {
            g_main_menu_cleanup_dirty.store(false, std::memory_order_release);
        }
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
            (void)g_sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
        return;
    }
    int32_t game_state_open = 1;
    const auto* world_facts = qol::features::extension_bag::world_facts();
    if (world_facts != nullptr && world_facts->game_state_gate != nullptr &&
        qol::contracts::succeeded(world_facts->game_state_gate(&game_state_open)) &&
        game_state_open == 0) {
        restore_module_view_locked();
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
            (void)g_sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
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
        if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
            (void)g_sdk->ui->end_frame();
        }
        drain_deferred_frees_locked();
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
        clear_stale_projected_moving_locked();
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
    if (g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->end_frame != nullptr) {
        (void)g_sdk->ui->end_frame();
    }
    drain_deferred_frees_locked();
}
