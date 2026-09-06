enum class ExtensionBagEquipResult {
    kOk,
    kReject,         // 前置不满足：非背包物品 / 扩展位已装备 / 不在原版库存
    kFailed,         // 移除或回滚失败（状态可能不一致，强日志）
    kPersistFailed,  // persist 失败且已完整回滚
};

ExtensionBagEquipResult equip_extension_bag_item_locked(int internal_bag, void* item) {
    ensure_state_loaded_locked();
    if (!virtual_bag::valid_index(internal_bag) || item == nullptr) {
        return ExtensionBagEquipResult::kReject;
    }
    if (g_virtual_bag_state.types[internal_bag] != 0) {
        return ExtensionBagEquipResult::kReject;
    }
    int category = 0;
    int src_bag = -1;
    int src_slot = -1;
    for (int bag = 0; bag < virtual_bag::kOriginalTransactionBagCount && src_bag < 0; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            void* current = nullptr;
            if (inventory_slot_locked(bag, slot, &current) && current == item) {
                src_bag = bag;
                src_slot = slot;
                break;
            }
        }
    }
    const bool module_source = src_bag < 0 && module_slot_of_item_locked(item, &src_bag, &src_slot);
    if (module_source) {
        category = g_virtual_bag_state.items[src_bag][src_slot].category;
    } else {
        if (src_bag < 0 || fn_get_bit == nullptr) return ExtensionBagEquipResult::kReject;
        const uint16_t flags =
            *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        category = fn_get_bit(flags, 15, 6);
    }
    if (category < 1 || category > 4) return ExtensionBagEquipResult::kReject;
    if (!module_source &&
        (fn_remove_item_direct == nullptr || fn_inven_save_item_on_empty == nullptr ||
         fn_save_save_item == nullptr || fn_save_load_item == nullptr || fn_itempool_free == nullptr)) {
        return ExtensionBagEquipResult::kFailed;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (module_source) {
        const virtual_bag::Item& source = g_virtual_bag_state.items[src_bag][src_slot];
        payload = source.payload;
        payload_size = source.payload_size;
    } else if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        VIRTBAG_LOG("extension equip reject reason=payload_serialize_failed src=%d/%d", src_bag,
                    src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    if (module_source) {
        const virtual_bag::Item source = g_virtual_bag_state.items[src_bag][src_slot];
        if (!virtual_bag::equip_bag(&g_virtual_bag_state, internal_bag, category)) {
            return ExtensionBagEquipResult::kReject;
        }
        g_virtual_bag_state.items[src_bag][src_slot] = {};
        g_item_state_dirty = true;
        if (persist_state_locked()) {
            free_module_object_locked(src_bag, src_slot);
            VIRTBAG_LOG("extension equip ok source=extension bag=%d category=%d src=%d/%d",
                        internal_bag, category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        g_virtual_bag_state.types[internal_bag] = 0;
        g_virtual_bag_state.items[src_bag][src_slot] = source;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
        persist_state_locked();
        return ExtensionBagEquipResult::kPersistFailed;
    }

    // P4.3：移除前先分配保管 handle（失败时物品仍在原版库存，无丢失风险）。
    uint32_t custody_handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &custody_handle) != ownership::Outcome::kOk) {
        VIRTBAG_LOG("extension equip ledger exhausted src=%d/%d", src_bag, src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    fn_remove_item_direct(src_bag, src_slot);
    void* after = item;
    if (inventory_slot_locked(src_bag, src_slot, &after) && after != nullptr) {
        VIRTBAG_LOG("extension equip remove verify failed bag=%d slot=%d", src_bag, src_slot);
        // 物品仍被 INVEN 持有（remove 未生效）→ 不得释放，记 inventory 终态。
        handover_tracked_item_locked(custody_handle, item,
                                     "equip remove verify failed; retained in inventory");
        return ExtensionBagEquipResult::kFailed;
    }
    // 移除已确认：此后任何失败（state 拒绝 / persist 失败）都必须回插物品，
    // 否则真实丢失。equip_bag 前置已查 types==0，此处拒绝仅剩理论可能。
    const bool state_equipped =
        virtual_bag::equip_bag(&g_virtual_bag_state, internal_bag, category);
    if (state_equipped) {
        g_item_state_dirty = true;
        if (persist_state_locked()) {
            // P4.3：装备对象已被状态 payload 取代（原指针无人引用），终止保管；
            // drop 路径处于触摸窗口内 → 隔离区，API 路径窗口外 → 真释放。
            retire_custody_item_locked(custody_handle, item, "equip custody retired");
            VIRTBAG_LOG("extension equip ok bag=%d category=%d src=%d/%d", internal_bag,
                        category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        g_virtual_bag_state.types[internal_bag] = 0;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
    } else {
        VIRTBAG_LOG("extension equip state reject bag=%d category=%d", internal_bag, category);
    }
    bool restored = false;
    uint32_t rebuilt_handle = 0;
    void* rebuilt = load_item_payload_tracked_locked(payload.data(), payload_size,
                                                     "equip_rollback", &rebuilt_handle);
    if (rebuilt != nullptr) {
        if (fn_inven_save_item_on_empty(rebuilt, src_bag)) {
            handover_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback handover");
            restored = true;
        } else {
            release_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback insert failed");
        }
    }
    retire_custody_item_locked(custody_handle, item, "equip rollback custody retired");
    persist_state_locked();
    VIRTBAG_LOG("extension equip persist failed bag=%d restored=%d", internal_bag,
                restored ? 1 : 0);
    return restored ? ExtensionBagEquipResult::kPersistFailed : ExtensionBagEquipResult::kFailed;
}

bool equip_module_item_on_character_locked(void* item, int source_bag, int source_slot) {
    if (item == nullptr || !virtual_bag::valid_index(source_bag) ||
        source_slot < 0 || source_slot >= virtual_bag::kSlotCount ||
        g_module_objects[source_bag][source_slot] != item || fn_can_equip == nullptr ||
        fn_find_equip_slot == nullptr || fn_equip_item_from_inven_to_slot == nullptr ||
        !g_module_view_installed || g_module_view_index != source_bag ||
        g_module_window_original_bag < 0 || g_module_window_original_bag >= 5) {
        return false;
    }
    void* character = member_or_null(0);
    if (character == nullptr || !fn_can_equip(character, item)) return false;

    const int target = fn_find_equip_slot(character, item);
    if (target < 0 || target >= C_EQUIP_SLOTS) return false;
    const int original_bag = g_module_window_original_bag;
    if (g_inven == nullptr) return false;

    // Projection deliberately leaves the real inventory untouched. The original
    // exchange entry point reads INVEN[bag][slot], so expose the projected item
    // only for the duration of that call and restore the real slot afterwards.
    auto* inventory_slot = reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_inven) + original_bag * 0x80 + source_slot * 8);
    void* original_inventory_item = *inventory_slot;
    void* previous_equipped_item = fn_get_equip_item != nullptr
        ? fn_get_equip_item(character, target) : nullptr;
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> previous_equipped_payload{};
    int previous_equipped_payload_size = 0;
    if (previous_equipped_item != nullptr &&
        !serialize_item_payload_locked(previous_equipped_item, &previous_equipped_payload,
                                       &previous_equipped_payload_size)) {
        return false;
    }
    *inventory_slot = item;

    // 这是原版 UI 的真实交换入口：它从 INVEN[bag][slot] 取新装备，
    // 目标槽已有装备时把旧装备写回同一库存槽，再把新装备放入角色槽。
    // 先不调用 CHAR_EquipItem/CHAR_Unequip，后者会跳过交换协议。
    extension_bag_begin_internal_equip();
    const int equip_result =
        fn_equip_item_from_inven_to_slot(character, original_bag, source_slot, target);
    extension_bag_end_internal_equip();
    if (equip_result == 0) {
        *inventory_slot = original_inventory_item;
        return false;
    }
    if (target >= 0 && target < C_EQUIP_SLOTS && fn_get_equip_item != nullptr &&
        fn_get_equip_item(character, target) != item) {
        *inventory_slot = original_inventory_item;
        return false;
    }

    *inventory_slot = original_inventory_item;
    // CHAR_EquipItemFromInvenToSlot 已把旧装备换入临时投影槽；该槽随后仍由
    // 扩展状态表示，不能让同一个旧指针同时继续留在原版 INVEN 中。
    // 角色装备接管对象所有权，不能走 free_module_object_locked；否则下一帧
    // 会把刚装备到角色槽的对象释放掉。先结束视图借用，再转为游戏侧持有。
    const uint32_t handle = g_module_object_handles[source_bag][source_slot];
    if (ownership::live_state(g_ownership_ledger, handle,
                              ownership::State::kBorrowedForView)) {
        ownership::return_from_view(&g_ownership_ledger, handle);
    }
    if (ownership::live_state(g_ownership_ledger, handle,
                              ownership::State::kModuleOwned)) {
        if (ownership::handover_to_inventory(&g_ownership_ledger, handle) !=
            ownership::Outcome::kOk) {
            VIRTBAG_LOG("extension character equip ownership handover failed bag=%d slot=%d",
                        source_bag, source_slot);
        }
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[source_bag][source_slot];
    if (previous_equipped_item != nullptr) {
        descriptor.payload = previous_equipped_payload;
        descriptor.payload_size = previous_equipped_payload_size;
        descriptor.category = fn_get_bit != nullptr
            ? fn_get_bit(*reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(
                              previous_equipped_item) + I_TYPE), 15, 6)
            : 0;
        descriptor.count = fn_get_cumulate_count != nullptr
            ? fn_get_cumulate_count(previous_equipped_item) : 1;
        // 旧装备对象被原版 UI（装备槽 desc/控件缓存）持续引用，绝不能释放：
        // 直接收编为扩展槽的物化对象，投影与详情都继续指向同一存活对象。
        g_module_objects[source_bag][source_slot] = previous_equipped_item;
        g_module_object_categories[source_bag][source_slot] = descriptor.category;
        g_module_object_hashes[source_bag][source_slot] = virtual_bag::payload_hash(descriptor);
        uint32_t adopted_handle = 0;
        if (ownership::allocate(&g_ownership_ledger, &adopted_handle) == ownership::Outcome::kOk) {
            g_module_object_handles[source_bag][source_slot] = adopted_handle;
        } else {
            g_module_object_handles[source_bag][source_slot] = 0;
            VIRTBAG_LOG("extension character equip adopt ledger exhausted bag=%d slot=%d",
                        source_bag, source_slot);
        }
    } else {
        descriptor = {};
        g_module_objects[source_bag][source_slot] = nullptr;
        g_module_object_categories[source_bag][source_slot] = 0;
        g_module_object_hashes[source_bag][source_slot] = 0;
        g_module_object_handles[source_bag][source_slot] = 0;
    }
    g_item_state_dirty = true;
    persist_state_locked();
    VIRTBAG_LOG("extension character equip ok bag=%d slot=%d target=%d", source_bag,
                source_slot, target);
    return true;
}

// 装备成功后的 UI 收尾（游戏线程、持锁；先例：draw_end/enter 路径的锁内
// UI 调用）：标签物品对象物化 + 控件 data[0] 更新（绘制每帧读 capacities，
// 其余自愈）+ 物品区刷新（清消耗源槽）+ 复位拖动态。音效复用模块袋切换
// 音 0x11——原版按装备位查未注册全局（0x2f3418/0x2f5b60）从简。
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control) {
    refresh_tab_bag_items_locked();
    void* tab = g_extension_tab_buttons[internal_bag];
    if (tab != nullptr && fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(tab);
        if (data != nullptr) {
            *reinterpret_cast<void**>(data) = g_tab_bag_items[internal_bag];
        }
    }
    if (g_module_view_installed) {
        refresh_module_item_area_locked(internal_bag);
        refresh_projection_if_overwritten_locked();
    } else if (fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();
    }
    if (fn_ui_equip_update_char_equip != nullptr) fn_ui_equip_update_char_equip();
    if (moving_control != nullptr) reset_drag_state_locked(moving_control);
    play_extension_switch_sound();
}

bool try_equip_on_extension_tab_drop_locked(int index, void* moving_control) {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return false;
    if (fn_control_object_get_data == nullptr) return false;
    void* data = fn_control_object_get_data(moving_control);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    if (item == nullptr) return false;
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(index, item);
    if (result != ExtensionBagEquipResult::kOk) {
        VIRTBAG_LOG("extension tab drop equip result=%d index=%d", static_cast<int>(result),
                    index);
        return false;
    }
    finish_extension_equip_ui_locked(index, moving_control);
    VIRTBAG_LOG("extension tab drop equipped index=%d", index);
    return true;
}

bool extension_source_should_equip_locked(int target_bag) {
    const auto& session = g_extension_drag_session;
    if (!virtual_bag::valid_index(target_bag) ||
        !virtual_bag::valid_index(session.source_bag) ||
        session.source_slot < 0 || session.source_slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const int category = g_virtual_bag_state.items[session.source_bag][session.source_slot].category;
    return g_virtual_bag_state.types[target_bag] == 0 && category >= 1 && category <= 4;
}

bool equip_extension_source_on_tab_locked(int target_bag, int source_bag, int source_slot) {
    void* source_item = module_item_locked(source_bag, source_slot);
    if (source_item == nullptr) return false;
    const ExtensionBagEquipResult result =
        equip_extension_bag_item_locked(target_bag, source_item);
    if (result != ExtensionBagEquipResult::kOk) {
        VIRTBAG_LOG("extension source tab equip result=%d target=%d source=%d/%d",
                    static_cast<int>(result), target_bag, source_bag, source_slot);
        return false;
    }
    finish_extension_equip_ui_locked(target_bag, nullptr);
    return true;
}

// ---- Path A：装备按钮（原版 ButtonEquipExe 背包分支的扩展对应物）----
// 挂法：BL hook 0xb9188（物品 desc 打开调 MakeDesc）→ 调原函数后 PtrHook
// 装备按钮（panel+0x78，SetDescMenu desc_type=2 装备分支 b8668 创建）的
// execute proc；用户点击装备按钮即明确选择扩展背包，因此只要扩展袋有空位
// 就接管，不再因原版背包有空位而闪回原版背包。
void extension_desc_equip_execute(void* button);

void reset_extension_desc_equip_hook_locked() {
    // 详情按钮由游戏所有：若当前 panel 仍指向被 hook 的按钮，先恢复原回调；
    // 若按钮已被游戏销毁，则不能再写入旧 slot，只丢弃模块记录。
    if (g_extension_desc_equip_hook.installed() && g_base != 0) {
        void* button = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x78);
        void* data = nullptr;
        if (button != nullptr &&
            game_memory_accessible(button, CO_DATA + sizeof(void*), 'r')) {
            data = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + CO_DATA);
        }
        void* execute_proc = nullptr;
        if (data != nullptr &&
            game_memory_accessible(data, CB_EXECUTE_PROC + sizeof(void*), 'r')) {
            execute_proc = reinterpret_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
        }
        if (execute_proc == g_extension_desc_equip_hook.slot) {
            g_extension_desc_equip_hook.uninstall();
        }
    }
    g_extension_desc_equip_hook = {};
    g_extension_desc_equip_item = nullptr;
}

void install_extension_desc_equip_hook_locked(void* item) {
    reset_extension_desc_equip_hook_locked();
    if (g_base == 0 || item == nullptr) return;
    void* button = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x78);
    if (button == nullptr) return;
    if (!game_memory_accessible(button, CO_DATA + sizeof(void*), 'r')) {
        VIRTBAG_LOG("extension desc equip hook skip invalid button=%p", button);
        return;
    }
    void* data = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr || !game_memory_accessible(data, CB_EXECUTE_PROC + sizeof(void*), 'r')) {
        VIRTBAG_LOG("extension desc equip hook skip invalid data button=%p data=%p", button, data);
        return;
    }
    void* execute_proc = reinterpret_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
    if (!game_memory_accessible(execute_proc, sizeof(void*), 'w') ||
        *reinterpret_cast<void**>(execute_proc) == nullptr ||
        !game_memory_accessible(*reinterpret_cast<void**>(execute_proc), 1, 'x')) {
        VIRTBAG_LOG("extension desc equip hook skip invalid execute button=%p", button);
        return;
    }
    const uintptr_t equip_raw = g_base +
        fn_resolve("F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA);
    if (equip_raw == 0 || *reinterpret_cast<void**>(execute_proc) !=
        reinterpret_cast<void*>(equip_raw)) {
        VIRTBAG_LOG("extension desc equip hook skip non-equip proc button=%p proc=%p expected=%p",
                    button, *reinterpret_cast<void**>(execute_proc),
                    reinterpret_cast<void*>(equip_raw));
        return;
    }
    if (!g_extension_desc_equip_hook.install_typed(execute_proc,
                                                   &extension_desc_equip_execute)) {
        VIRTBAG_LOG("extension desc equip hook install failed");
        return;
    }
    g_extension_desc_equip_item = item;
}

void extension_desc_equip_execute(void* button) {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA)
        : 0;
    bool takeover = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        void* item = g_extension_desc_equip_item;
        void* hooked_button =
            g_base != 0 ? *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x78)
                        : nullptr;
        VIRTBAG_LOG("extension equip execute button=%p hooked=%p item=%p", button,
                    hooked_button, g_extension_desc_equip_item);
        reset_extension_desc_equip_hook_locked();
            const bool current_button = button != nullptr && button == hooked_button;
        if (raw != 0 && current_button && g_virtual_bag_enabled.load() && game_in_world() &&
            item != nullptr) {
            const uint16_t flags = *reinterpret_cast<uint16_t*>(
                reinterpret_cast<uint8_t*>(item) + I_TYPE);
            const int category = fn_get_bit != nullptr ? fn_get_bit(flags, 15, 6) : -1;
            const bool jewel = category >= 0 && fn_is_jewel != nullptr &&
                               fn_is_jewel(category) != 0;
            if (!jewel && item_is_equip(item)) {
                int source_bag = -1;
                int source_slot = -1;
                if (module_slot_of_item_locked(item, &source_bag, &source_slot)) {
                    takeover = equip_module_item_on_character_locked(item, source_bag,
                                                                       source_slot);
                    if (takeover) {
                        clear_original_desc_locked();
                        finish_extension_equip_ui_locked(source_bag, nullptr);
                    } else {
                        VIRTBAG_LOG("extension character equip button failed bag=%d slot=%d",
                                    source_bag, source_slot);
                    }
                }
            }
        }
    }
    if (!takeover && raw != 0) {
        reinterpret_cast<void (*)(void*)>(raw)(button);
    }
}

void reset_extension_desc_item_hooks_locked() {
    for (PtrHook& hook : g_extension_desc_item_hooks) hook.uninstall();
}

void extension_destroy_ok(void*);
void extension_sell_ok(void*);
void extension_destroy_cancel(void*);
void extension_sell_cancel(void*);
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context);

void restore_extension_desc_popup_callbacks_locked() {
    if (g_base != 0) {
        auto* ok_slot = reinterpret_cast<ExtensionDestroyOkFn*>(g_base + G_POPUP_FPOK_VMA);
        auto* cancel_slot = reinterpret_cast<ExtensionDestroyOkFn*>(g_base + G_POPUP_FPCANCEL_VMA);
        if (*ok_slot == &extension_destroy_ok) *ok_slot = g_extension_destroy_original_ok;
        if (*ok_slot == &extension_sell_ok) *ok_slot = g_extension_sell_original_ok;
        if (*cancel_slot == &extension_destroy_cancel) *cancel_slot = g_extension_destroy_original_cancel;
        if (*cancel_slot == &extension_sell_cancel) *cancel_slot = g_extension_sell_original_cancel;
    }
    g_extension_destroy_original_ok = nullptr;
    g_extension_sell_original_ok = nullptr;
    g_extension_destroy_original_cancel = nullptr;
    g_extension_sell_original_cancel = nullptr;
    g_extension_sell_apply_variant_discount = true;
}

void extension_sell_cancel(void*) {
    ExtensionDestroyOkFn original_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        original_cancel = g_extension_sell_original_cancel;
        restore_extension_desc_popup_callbacks_locked();
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
    }
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
    if (original_cancel != nullptr) original_cancel(nullptr);
}

void extension_destroy_cancel(void*) {
    ExtensionDestroyOkFn original_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        original_cancel = g_extension_destroy_original_cancel;
        restore_extension_desc_popup_callbacks_locked();
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
    }
    if (original_cancel != nullptr) original_cancel(nullptr);
}

void extension_sell_ok(void*) {
    VIRTBAG_LOG("extension sell confirmation entered");
    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int count = 0;
    int64_t price = 0;
    bool apply_variant_discount = true;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        restore_extension_desc_popup_callbacks_locked();
        item = g_extension_desc_item;
        bag = g_extension_desc_item_bag;
        slot = g_extension_desc_item_slot;
        apply_variant_discount = g_extension_sell_apply_variant_discount;
        const bool current_item = item != nullptr && virtual_bag::valid_index(bag) &&
                                  slot >= 0 && slot < virtual_bag::kSlotCount &&
                                  g_module_objects[bag][slot] == item;
        if (!current_item) {
            VIRTBAG_LOG("extension sell rejected stale detail bag=%d slot=%d item=%p", bag,
                        slot, item);
            reset_extension_desc_item_hooks_locked();
            g_extension_desc_item = nullptr;
            g_extension_desc_item_bag = -1;
            g_extension_desc_item_slot = -1;
            clear_original_desc_locked();
            return;
        }
        if (fn_item_get_sell_price == nullptr || fn_add_money == nullptr ||
            fn_minus_money == nullptr || fn_item_is_no_sell == nullptr) {
            VIRTBAG_LOG("extension sell rejected symbols unavailable bag=%d slot=%d", bag, slot);
            clear_original_desc_locked();
            return;
        }
        count = g_virtual_bag_state.items[bag][slot].count;
        const uint16_t flags = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(item) + I_TYPE);
        const int category = fn_get_bit != nullptr ? fn_get_bit(flags, 15, 6) : -1;
        if (category < 0 || fn_item_is_no_sell(category) != 0) {
            VIRTBAG_LOG("extension sell rejected no-sell item bag=%d slot=%d category=%d",
                        bag, slot, category);
            clear_original_desc_locked();
            return;
        }
        price = extension_bag_sell_price(item, count, apply_variant_discount);
        if (price < 0) {
            VIRTBAG_LOG("extension sell rejected invalid price/count bag=%d slot=%d price=%lld count=%d",
                        bag, slot, static_cast<long long>(price), count);
            clear_original_desc_locked();
            return;
        }
    }

    if (price < 0 || !fn_add_money(price)) {
        VIRTBAG_LOG("extension sell money credit failed bag=%d slot=%d price=%lld", bag, slot,
                    static_cast<long long>(price));
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        clear_original_desc_locked();
        return;
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const bool current_item = g_extension_desc_item == item &&
                                  g_extension_desc_item_bag == bag &&
                                  g_extension_desc_item_slot == slot &&
                                  g_module_objects[bag][slot] == item &&
                                  g_virtual_bag_state.items[bag][slot].count == count;
        if (current_item) {
            g_virtual_bag_state.items[bag][slot] = {};
            g_item_state_dirty = true;
            free_module_object_locked(bag, slot);
            persist_state_locked();
            if (g_module_view_installed && g_module_view_index == bag) {
                refresh_module_item_area_locked(bag);
            }
            removed = true;
        }
    }
    if (!removed) {
        const bool refunded = fn_minus_money(price) != 0;
        VIRTBAG_LOG("extension sell remove failed bag=%d slot=%d price=%lld refunded=%d", bag,
                    slot, static_cast<long long>(price), refunded ? 1 : 0);
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        clear_original_desc_locked();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
        clear_original_desc_locked();
    }
    VIRTBAG_LOG("extension sell committed bag=%d slot=%d price=%lld", bag, slot,
                static_cast<long long>(price));
}

void extension_destroy_ok(void*) {
    int committed_bag = -1;
    int committed_slot = -1;
    bool committed = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        restore_extension_desc_popup_callbacks_locked();

        const int bag = g_extension_desc_item_bag;
        const int slot = g_extension_desc_item_slot;
        if (g_extension_desc_item != nullptr && virtual_bag::valid_index(bag) &&
            slot >= 0 && slot < virtual_bag::kSlotCount &&
            g_module_objects[bag][slot] == g_extension_desc_item) {
            g_virtual_bag_state.items[bag][slot] = {};
            g_item_state_dirty = true;
            free_module_object_locked(bag, slot);
            persist_state_locked();
            reset_extension_desc_item_hooks_locked();
            g_extension_desc_item = nullptr;
            g_extension_desc_item_bag = -1;
            g_extension_desc_item_slot = -1;
            clear_original_desc_locked();
            committed_bag = bag;
            committed_slot = slot;
            committed = true;
        } else {
            reset_extension_desc_item_hooks_locked();
            g_extension_desc_item = nullptr;
            g_extension_desc_item_bag = -1;
            g_extension_desc_item_slot = -1;
            VIRTBAG_LOG("extension destroy rejected stale detail bag=%d slot=%d", bag, slot);
        }
    }
    if (committed && fn_ui_equip_refresh_item_area != nullptr) {
        fn_ui_equip_refresh_item_area();
    }
    if (committed) {
        VIRTBAG_LOG("extension destroy committed bag=%d slot=%d", committed_bag, committed_slot);
    }
}

bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        item == nullptr || before_count <= 0) {
        return false;
    }
    int count = fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : before_count - 1;
    if (count == observed_before) --count;
    if (count >= before_count) count = before_count - 1;
    if (count < 0) count = 0;
    const uint32_t old_count_flags = *reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(item) + I_COUNT);
    if (count == 0) {
        g_virtual_bag_state.items[bag][slot] = {};
        free_module_object_locked(bag, slot);
        return true;
    }
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
        stack_codec::write_count(old_count_flags, static_cast<uint32_t>(count));
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) = old_count_flags;
        VIRTBAG_LOG("extension consume reserialize failed context=%s bag=%d slot=%d",
                    context, bag, slot);
        return false;
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    descriptor.count = count;
    g_module_object_categories[bag][slot] = descriptor.category;
    g_module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
    return true;
}

void install_extension_desc_item_hooks_locked(void* item) {
    if (item == nullptr || g_base == 0 || fn_control_object_get_data == nullptr) return;
    constexpr std::array<size_t, 11> kDescButtonOffsets = {
        0x68, 0x70, 0x78, 0x80, 0x88, 0x90,
        0x98, 0xa0, 0xa8, 0xb0, 0xb8,
    };
    for (size_t index = 0; index < kDescButtonOffsets.size(); ++index) {
        if (kDescButtonOffsets[index] == 0x78) continue;
        void* button = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA +
                                                  kDescButtonOffsets[index]);
        if (button == nullptr) continue;
        if (!game_memory_accessible(button, CO_DATA + sizeof(void*), 'r')) {
            VIRTBAG_LOG("extension desc hook skip invalid button index=%d button=%p",
                        static_cast<int>(index), button);
            continue;
        }
        void* data = fn_control_object_get_data(button);
        if (data == nullptr || !game_memory_accessible(data, CB_EXECUTE_PROC + sizeof(void*), 'r')) {
            VIRTBAG_LOG("extension desc hook skip invalid data index=%d button=%p data=%p",
                        static_cast<int>(index), button, data);
            continue;
        }
        void** execute_slot = reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(data) + CB_EXECUTE_PROC);
        if (!game_memory_accessible(execute_slot, sizeof(void*), 'w')) continue;
        bool already_hooked = false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (g_extension_desc_item_hooks[previous].slot == execute_slot) {
                already_hooked = true;
                break;
            }
        }
        if (already_hooked) {
            VIRTBAG_LOG("extension desc hook skip duplicate slot index=%d offset=0x%zx slot=%p",
                        static_cast<int>(index), kDescButtonOffsets[index], execute_slot);
            continue;
        }
        void* original = *execute_slot;
        if (original == nullptr || !game_memory_accessible(original, 1, 'x')) continue;
        if (!g_extension_desc_item_hooks[index].install_typed(
                execute_slot, &extension_desc_item_execute)) {
            VIRTBAG_LOG("extension desc hook install failed index=%d offset=0x%zx button=%p data=%p",
                        static_cast<int>(index), kDescButtonOffsets[index], button, data);
            continue;
        }
        VIRTBAG_LOG("extension desc hook installed index=%d offset=0x%zx button=%p data=%p orig=%p",
                    static_cast<int>(index), kDescButtonOffsets[index], button, data, original);
    }
}

void extension_desc_item_execute(void* button) {
    constexpr std::array<size_t, 11> kDescButtonOffsets = {
        0x68, 0x70, 0x78, 0x80, 0x88, 0x90,
        0x98, 0xa0, 0xa8, 0xb0, 0xb8,
    };
    int hook_index = -1;
    void* item = nullptr;
    int item_bag = -1;
    int item_slot = -1;
    PtrHook* hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        for (size_t index = 0; index < kDescButtonOffsets.size(); ++index) {
            if (!g_extension_desc_item_hooks[index].installed()) continue;
            void* current = *reinterpret_cast<void**>(
                g_base + G_UIEQUIP_PANEL_VMA + kDescButtonOffsets[index]);
            if (current == button) {
                hook_index = static_cast<int>(index);
                hook = &g_extension_desc_item_hooks[index];
                break;
            }
        }
        item = g_extension_desc_item;
        item_bag = g_extension_desc_item_bag;
        item_slot = g_extension_desc_item_slot;
    }
    if (hook_index < 0 || hook == nullptr) return;
    VIRTBAG_LOG("extension desc execute index=%d offset=0x%zx orig=%p bag=%d slot=%d",
                hook_index, kDescButtonOffsets[hook_index], hook->orig, item_bag, item_slot);

    const uintptr_t use_raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_BUTTON_USE_EXE_VMA", F_UIEQUIP_BUTTON_USE_EXE_VMA)
        : 0;
    if (use_raw != 0 && reinterpret_cast<uintptr_t>(hook->orig) == use_raw) {
        bool used = false;
        bool current_item = false;
        void* leader = nullptr;
        bool closes_description = false;
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            current_item = item != nullptr && virtual_bag::valid_index(item_bag) &&
                           item_slot >= 0 && item_slot < virtual_bag::kSlotCount &&
                           g_module_objects[item_bag][item_slot] == item;
            leader = current_item ? member_or_null(0) : nullptr;
            closes_description = current_item;
        }
        if (current_item && leader != nullptr && fn_char_use_item_ex != nullptr) {
            // CHAR_UseItemEx 会同步进入 INVEN_ConsumeItem；不得持有背包锁调用。
            if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
            used = fn_char_use_item_ex(leader, item, 0) != 0;
        }
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            if (closes_description) {
                reset_extension_desc_item_hooks_locked();
                g_extension_desc_item = nullptr;
                g_extension_desc_item_bag = -1;
                g_extension_desc_item_slot = -1;
            }
        }
        VIRTBAG_LOG("extension use execute bag=%d slot=%d used=%d", item_bag, item_slot,
                    used ? 1 : 0);
        return;
    }

    const uintptr_t destroy_raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_BUTTON_DESTROY_EXE_VMA",
                              F_UIEQUIP_BUTTON_DESTROY_EXE_VMA)
        : 0;
    const uintptr_t sell_button_raw = g_base != 0
        ? g_base + fn_resolve("F_UISTORE_BUTTON_SELL_EXE_VMA", F_UISTORE_BUTTON_SELL_EXE_VMA)
        : 0;
    const bool direct_sell = sell_button_raw != 0 &&
                             reinterpret_cast<uintptr_t>(hook->orig) == sell_button_raw;
    const bool modified_destroy_sell = destroy_raw != 0 &&
                                       reinterpret_cast<uintptr_t>(hook->orig) == destroy_raw;
    if (direct_sell || modified_destroy_sell) {
        VIRTBAG_LOG("extension sell button matched source=%s bag=%d slot=%d item=%p",
                    modified_destroy_sell ? "destroy" : "store",
                    g_extension_desc_item_bag, g_extension_desc_item_slot,
                    g_extension_desc_item);
        int64_t price = 0;
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            const int bag = g_extension_desc_item_bag;
            const int slot = g_extension_desc_item_slot;
            void* item = g_extension_desc_item;
            const bool current_item = item != nullptr && virtual_bag::valid_index(bag) &&
                                      slot >= 0 && slot < virtual_bag::kSlotCount &&
                                      g_module_objects[bag][slot] == item;
        if (current_item && fn_item_get_sell_price != nullptr &&
                fn_item_is_no_sell != nullptr && fn_get_bit != nullptr) {
                const int count = g_virtual_bag_state.items[bag][slot].count;
                const uint16_t flags = *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(item) + I_TYPE);
                const int category = fn_get_bit(flags, 15, 6);
                if (fn_item_is_no_sell(category) == 0 && count > 0) {
                    g_extension_sell_apply_variant_discount = modified_destroy_sell;
                    price = extension_bag_sell_price(item, count, modified_destroy_sell);
                    if (price >= 0) valid = true;
                }
            }
            g_extension_sell_original_ok = nullptr;
            g_extension_sell_original_cancel = nullptr;
        }
        const uintptr_t original_ok_raw = g_base != 0
            ? g_base + fn_resolve(
                modified_destroy_sell ? "F_UIEQUIP_OK_DESTROY_ITEM_VMA"
                                      : "F_UISTORE_SELL_ITEM_VMA",
                modified_destroy_sell ? F_UIEQUIP_OK_DESTROY_ITEM_VMA
                                      : F_UISTORE_SELL_ITEM_VMA)
            : 0;
        const bool popup_created = valid && price <= UINT32_MAX && original_ok_raw != 0 &&
                                   fn_popup_create_yesno_from_textdata != nullptr;
        if (popup_created) {
            // 原版按钮先关闭详情再创建确认弹窗；保持同样的 UI 生命周期，且不持有背包锁调用游戏函数。
            if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
            // 先沿用原版回调创建控件。CreateYesNoFromTextData 内部会把 x3
            // 暂时带入原版文本构造链，不能直接传模块函数指针；x5 是弹窗 i32Param。
            fn_popup_create_yesno_from_textdata(
                9, 0, 2,
                reinterpret_cast<void*>(original_ok_raw), nullptr,
                reinterpret_cast<void*>(static_cast<uintptr_t>(price)));
            auto* ok_slot = reinterpret_cast<ExtensionDestroyOkFn*>(g_base + G_POPUP_FPOK_VMA);
            auto* cancel_slot = reinterpret_cast<ExtensionDestroyOkFn*>(g_base + G_POPUP_FPCANCEL_VMA);
            g_extension_sell_original_ok = *ok_slot;
            g_extension_sell_original_cancel = *cancel_slot;
            *ok_slot = &extension_sell_ok;
            *cancel_slot = &extension_sell_cancel;
            VIRTBAG_LOG("extension sell confirmation created price=%lld", static_cast<long long>(price));
        } else {
            VIRTBAG_LOG("extension sell confirmation unavailable valid=%d", valid ? 1 : 0);
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (popup_created) {
            reset_extension_desc_item_hooks_locked();
        } else {
            clear_original_desc_locked();
        }
        return;
    }
    hook->call_orig<void>(button);

    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (item == nullptr || !virtual_bag::valid_index(item_bag) ||
        item_slot < 0 || item_slot >= virtual_bag::kSlotCount ||
        g_module_objects[item_bag][item_slot] != item) {
        reset_extension_desc_item_hooks_locked();
        return;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        VIRTBAG_LOG("extension desc operation reserialize failed bag=%d slot=%d", item_bag,
                    item_slot);
        reset_extension_desc_item_hooks_locked();
        return;
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[item_bag][item_slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    if (fn_get_bit != nullptr) {
        const uint16_t flags = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(item) + I_TYPE);
        descriptor.category = fn_get_bit(flags, 15, 6);
    }
    descriptor.count = static_cast<int>(stack_codec::read_count(
        virtual_bag::payload_count(descriptor)));
    if (descriptor.category <= 0 || descriptor.count <= 0) {
        descriptor = {};
        free_module_object_locked(item_bag, item_slot);
    } else {
        g_module_object_categories[item_bag][item_slot] = descriptor.category;
        g_module_object_hashes[item_bag][item_slot] = virtual_bag::payload_hash(descriptor);
    }
    g_item_state_dirty = true;
    reset_extension_desc_item_hooks_locked();
    VIRTBAG_LOG("extension desc operation synced bag=%d slot=%d category=%d count=%d",
                item_bag, item_slot, descriptor.category, descriptor.count);
}

// BL 门禁（0xb9188）：物品 desc 打开 → 调原 MakeDesc（同步生成含装备按钮的
// 菜单）→ 锁内重装装备按钮 hook。物品从触发控件 data[0] 取（MakeDesc 同源）。
void make_desc_equip_gate(void* ctrl, void* arg) {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_MAKE_DESC_VMA", F_UIEQUIP_MAKE_DESC_VMA)
        : 0;
    if (raw == 0 || ctrl == nullptr) return;
    // UIEquip_RefreshItemArea may have restored the original inventory object
    // immediately before the original event reaches MakeDesc. Rebind only
    // projected controls so the original MakeDesc reads the extension item.
    virtual_bag_sync_projected_item_control(ctrl);
    void* item = nullptr;
    if (fn_control_object_get_data != nullptr) {
        if (game_memory_accessible(ctrl, CO_DATA + sizeof(void*), 'r')) {
            void* data = fn_control_object_get_data(ctrl);
            if (data != nullptr && game_memory_accessible(data, sizeof(void*), 'r')) {
                item = *reinterpret_cast<void**>(data);
            }
        }
    }
    reinterpret_cast<UiEquipMakeDescFn>(raw)(ctrl, arg);
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
        if (item != nullptr) {
            int item_bag = -1;
            int item_slot = -1;
            if (module_slot_of_item_locked(item, &item_bag, &item_slot)) {
                g_extension_desc_item = item;
                g_extension_desc_item_bag = item_bag;
                g_extension_desc_item_slot = item_slot;
                install_extension_desc_item_hooks_locked(item);
                VIRTBAG_LOG("extension desc item captured bag=%d slot=%d item=%p", item_bag,
                            item_slot, item);
            }
        }
        bool real_equip = false;
        if (item != nullptr && fn_get_bit != nullptr && fn_is_jewel != nullptr) {
            const uint16_t flags = *reinterpret_cast<uint16_t*>(
                reinterpret_cast<uint8_t*>(item) + I_TYPE);
            const int category = fn_get_bit(flags, 15, 6);
            real_equip = fn_is_jewel(category) == 0 && item_is_equip(item);
        }
        if (g_virtual_bag_enabled.load() && game_in_world() && real_equip) {
            install_extension_desc_equip_hook_locked(item);
        } else {
            reset_extension_desc_equip_hook_locked();
        }
    }
}

void extension_desc_unequip_execute(void*) {
    ExtensionBagUnequipResult result = ExtensionBagUnequipResult::kNotEquipped;
    bool show_not_empty = false;
    bool show_no_space = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        const int internal_bag = g_extension_desc_unequip_bag;
        const bool owns_current_desc =
            virtual_bag::valid_index(internal_bag) &&
            g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
            g_virtual_bag_state.selected == internal_bag &&
            g_virtual_bag_state.info_bag == internal_bag;
        if (owns_current_desc) {
            result = unequip_extension_bag_locked(internal_bag);
            show_not_empty = result == ExtensionBagUnequipResult::kNotEmpty;
            show_no_space = result == ExtensionBagUnequipResult::kNoSpace;
            VIRTBAG_LOG("extension desc unequip bag=%d result=%d", internal_bag,
                        static_cast<int>(result));
        } else {
            VIRTBAG_LOG("extension desc unequip rejected: stale desc bag=%d", internal_bag);
        }
        reset_extension_desc_unequip_hook_locked();
    }

    // 锁内仅置标志，弹窗统一在解锁后调用（锁内零 UI 不变量）；两条弹窗均为
    // 原版 UIEquip_ButtonUnequipExe 对应分支的精确复刻（非空=7/b8084、
    // 无空位=6/b809c），且不在按钮回调栈内做 UIDesc_SetOff。
    if (show_not_empty) {
        show_extension_bag_not_empty_popup();
        return;
    }
    if (show_no_space) {
        show_extension_bag_no_space_popup();
        return;
    }
    // Successful removal is restored by virtual_bag_draw_end_wrapper on the
    // next game frame. Do not invoke original UI refresh from this callback or
    // from the HTTP/JNI operation path.
}
