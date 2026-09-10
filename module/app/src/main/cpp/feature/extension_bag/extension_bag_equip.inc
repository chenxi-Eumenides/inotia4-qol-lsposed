enum class ExtensionBagEquipResult {
    kOk,
    kReject,         // 前置不满足：非背包物品 / 扩展位已装备 / 不在原版库存
    kFailed,         // 移除或回滚失败（状态可能不一致，强日志）
    kPersistFailed,  // persist 失败且已完整回滚
};

void* extension_menu_character() {
    if (g_base == 0) return nullptr;
    const uintptr_t raw = g_base + fn_resolve(
        "F_GET_MENU_CHARACTER_VMA", F_GET_MENU_CHARACTER_VMA);
    if (raw == 0 || !game_memory_accessible(reinterpret_cast<void*>(raw), 4, 'x')) {
        return nullptr;
    }
    const GetMenuCharacterFn get_menu_character =
        reinterpret_cast<GetMenuCharacterFn>(raw);
    return get_menu_character();
}

ExtensionBagEquipResult equip_extension_bag_item_locked(std::unique_lock<std::mutex>& lock,
                                                        int internal_bag, void* item) {
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
    if (!is_backpack_category(category)) return ExtensionBagEquipResult::kReject;
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
        const uint8_t previous_type = g_virtual_bag_state.types[internal_bag];
        if (!virtual_bag::equip_bag(&g_virtual_bag_state, internal_bag, category)) {
            return ExtensionBagEquipResult::kReject;
        }
        g_virtual_bag_state.items[src_bag][src_slot] = {};
        g_item_state_dirty = true;
        if (persist_state_locked()) {
            if (!free_module_object_locked(src_bag, src_slot)) {
                g_virtual_bag_state.types[internal_bag] = previous_type;
                g_virtual_bag_state.items[src_bag][src_slot] = source;
                g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
                virtual_bag::normalize(&g_virtual_bag_state);
                persist_state_locked();
                VIRTBAG_LOG("extension equip rejected source release bag=%d slot=%d",
                            src_bag, src_slot);
                return ExtensionBagEquipResult::kFailed;
            }
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

    if (!remove_item_direct_unlocked(lock, src_bag, src_slot, item, "extension equip")) {
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

bool equip_module_item_on_character_locked(void* character, void* item, int source_bag,
                                           int source_slot) {
    if (character == nullptr || item == nullptr || !virtual_bag::valid_index(source_bag) ||
        source_slot < 0 || source_slot >= virtual_bag::kSlotCount ||
        g_module_objects[source_bag][source_slot] != item || fn_can_equip == nullptr ||
        fn_find_equip_slot == nullptr || fn_equip_item_from_inven_to_slot == nullptr ||
        !g_module_view_installed || g_module_view_index != source_bag ||
        g_module_window_original_bag < 0 || g_module_window_original_bag >= 5) {
        return false;
    }
    if (!module_object_replacement_allowed_locked(source_bag, source_slot,
                                                  "character equip")) {
        return false;
    }
    if (!fn_can_equip(character, item)) return false;

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
        if (!module_object_generation_advance_locked(
                source_bag, source_slot, "character equip replacement")) {
            return false;
        }
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
        if (!module_object_generation_advance_locked(
                source_bag, source_slot, "character equip clear")) {
            return false;
        }
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
// UI 调用）：internal_bag 始终是扩展逻辑袋 0..4。若当前已在另一个扩展视图，
// 先复用 handle_extension_tab_click_locked 切到目标袋，使容量字和投影使用同一
// g_module_view_index，再刷新物品区；随后更新标签对象、复位拖动态并播放音效。
void finish_extension_equip_ui_locked(int internal_bag, void* moving_control) {
    bool switched_view = false;
    refresh_tab_bag_items_locked();
    void* tab = g_extension_tab_buttons[internal_bag];
    if (tab != nullptr && fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(tab);
        if (data != nullptr) {
            *reinterpret_cast<void**>(data) = g_tab_bag_items[internal_bag];
        }
    }
    if (g_module_view_installed && g_virtual_bag_state.mode == virtual_bag::Mode::kModule &&
        g_module_view_index != internal_bag) {
        handle_extension_tab_click_locked(internal_bag);
        switched_view = g_module_view_index == internal_bag;
    }
    if (g_module_view_installed && g_module_view_index == internal_bag) {
        refresh_module_item_area_locked(internal_bag);
    } else if (fn_ui_equip_refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    if (fn_ui_equip_update_char_equip != nullptr) fn_ui_equip_update_char_equip();
    if (moving_control != nullptr) reset_drag_state_locked(moving_control);
    if (!switched_view) play_extension_switch_sound();
}

bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                            int index, void* moving_control) {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return false;
    if (fn_control_object_get_data == nullptr) return false;
    void* data = fn_control_object_get_data(moving_control);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    if (item == nullptr) return false;
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(lock, index, item);
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
    return g_virtual_bag_state.types[target_bag] == 0 && is_backpack_category(category);
}

int first_empty_extension_bag_locked() {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        if (g_virtual_bag_state.types[bag] == 0) return bag;
    }
    return -1;
}

bool equip_extension_source_on_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag,
                                          int source_bag, int source_slot) {
    void* source_item = module_item_locked(source_bag, source_slot);
    if (source_item == nullptr) return false;
    const ExtensionBagEquipResult result =
        equip_extension_bag_item_locked(lock, target_bag, source_item);
    if (result != ExtensionBagEquipResult::kOk) {
        VIRTBAG_LOG("extension source tab equip result=%d target=%d source=%d/%d",
                    static_cast<int>(result), target_bag, source_bag, source_slot);
        return false;
    }
    finish_extension_equip_ui_locked(target_bag, nullptr);
    return true;
}

// ---- Path A：装备按钮语义（已函数级 hook 化，无按钮 PtrHook）----
// 详情"装备"按钮 execute proc 恒为 UIEquip_ButtonEquipExe（0xb7c18），因此
// 其全部扩展语义由 native 第 10 hook（button_equip_exe_wrapper →
// virtual_bag_handle_backpack_button_equip）承担：背包物品（装原版/扩展袋）
// 与扩展槽装备物品（装角色）在原函数内联删源之前分流；其余交还原函数。
// 历史遗留：早期曾用按钮 PtrHook（改 execute_slot 指针）接管，与函数级
// hook 构成同点双层钩子，且受 SetDescMenu 按钮生命周期/槽位漂移困扰，
// 已整体移除。

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
                                  g_module_objects[bag][slot] == item &&
                                  module_object_release_allowed_locked(
                                      bag, slot, item, "extension sell", false, nullptr);
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
        const int64_t unit_price = static_cast<int64_t>(fn_item_get_sell_price(item));
        price = extension_bag_sell_price(item, count, apply_variant_discount);
        if (price < 0) {
            VIRTBAG_LOG(
                "sell price reject reason=price_bounds source=equip category=%d count=%d unit=%lld variant=%d payload=%u gen=%llu",
                category, count, static_cast<long long>(unit_price),
                apply_variant_discount ? 70 : 100,
                virtual_bag::payload_hash(g_virtual_bag_state.items[bag][slot]),
                static_cast<unsigned long long>(g_module_object_use[bag][slot].generation));
            clear_original_desc_locked();
            return;
        }
        VIRTBAG_LOG(
            "sell price source=equip category=%d count=%d unit=%lld variant=%d final=%lld payload=%u gen=%llu",
            category,
            static_cast<int>(stack_codec::clamp_count(static_cast<uint32_t>(count),
                                                      stack_limit_enabled())),
            static_cast<long long>(unit_price), apply_variant_discount ? 70 : 100,
            static_cast<long long>(price),
            virtual_bag::payload_hash(g_virtual_bag_state.items[bag][slot]),
            static_cast<unsigned long long>(g_module_object_use[bag][slot].generation));
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
                                  g_virtual_bag_state.items[bag][slot].count == count &&
                                  module_object_release_allowed_locked(
                                      bag, slot, item, "extension sell commit", false, nullptr);
        if (current_item) {
            const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
            g_virtual_bag_state.items[bag][slot] = {};
            g_item_state_dirty = true;
            if (!free_module_object_locked(bag, slot)) {
                g_virtual_bag_state.items[bag][slot] = previous_item;
                persist_state_locked();
                VIRTBAG_LOG("extension sell rejected object release bag=%d slot=%d", bag, slot);
            } else {
                persist_state_locked();
                if (g_module_view_installed && g_module_view_index == bag) {
                    refresh_module_item_area_locked(bag);
                }
                removed = true;
            }
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
            g_module_objects[bag][slot] == g_extension_desc_item &&
            module_object_release_allowed_locked(
                bag, slot, g_extension_desc_item, "extension destroy", false, nullptr)) {
            const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
            g_virtual_bag_state.items[bag][slot] = {};
            g_item_state_dirty = true;
            if (!free_module_object_locked(bag, slot)) {
                g_virtual_bag_state.items[bag][slot] = previous_item;
                persist_state_locked();
                VIRTBAG_LOG("extension destroy rejected object release bag=%d slot=%d", bag, slot);
            } else {
                persist_state_locked();
                committed_bag = bag;
                committed_slot = slot;
                committed = true;
            }
            reset_extension_desc_item_hooks_locked();
            g_extension_desc_item = nullptr;
            g_extension_desc_item_bag = -1;
            g_extension_desc_item_slot = -1;
            clear_original_desc_locked();
        } else {
            reset_extension_desc_item_hooks_locked();
            g_extension_desc_item = nullptr;
            g_extension_desc_item_bag = -1;
            g_extension_desc_item_slot = -1;
            VIRTBAG_LOG("extension destroy rejected stale detail bag=%d slot=%d", bag, slot);
        }
    }
    if (committed && fn_ui_equip_refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    if (committed) {
        VIRTBAG_LOG("extension destroy committed bag=%d slot=%d", committed_bag, committed_slot);
    }
}

bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* raw_use_token) {
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
        const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
        g_virtual_bag_state.items[bag][slot] = {};
        if (!free_module_object_locked(bag, slot, raw_use_token)) {
            g_virtual_bag_state.items[bag][slot] = previous_item;
            VIRTBAG_LOG("extension consume rejected object release context=%s bag=%d slot=%d",
                        context, bag, slot);
            return false;
        }
        return true;
    }
    const stack_codec::CountEncoding count_encoding = item_count_encoding(item);
    if (count_encoding != stack_codec::CountEncoding::kEncoded) {
        VIRTBAG_LOG("extension consume rejected count encoding=%d context=%s bag=%d slot=%d",
                    static_cast<int>(count_encoding), context, bag, slot);
        return false;
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
    // ApplyStuff consumes a stacked material through H-04. Keep the token's
    // consumed bit even when the native object remains live, otherwise the
    // caller treats a successful jewel/scroll apply as an unconsumed failure.
    if (raw_use_token != nullptr) {
        ModuleUseToken* token = static_cast<ModuleUseToken*>(raw_use_token);
        ModuleUseState& use = g_module_object_use[bag][slot];
        if (use.active && use.item == item && use.generation == token->generation) {
            use.consumed = true;
        }
    }
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
        // 装备/卸下按钮由函数级 hook（ButtonEquipExe/ButtonUnequipExe）接管，
        // 其槽位 execute proc 即这两个函数地址；观察 hook 不应对其包装
        // （避免点击先进观察 wrapper 的详情操作同步逻辑）。
        const uintptr_t equip_proc = g_base + fn_resolve(
            "F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA);
        const uintptr_t unequip_proc = g_base + fn_resolve(
            "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA", F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA);
        void* proc_value = *reinterpret_cast<void**>(execute_slot);
        if (proc_value == reinterpret_cast<void*>(equip_proc) ||
            proc_value == reinterpret_cast<void*>(unequip_proc)) {
            VIRTBAG_LOG("extension desc hook skip equip/unequip proc offset=0x%zx slot=%p",
                        kDescButtonOffsets[index], execute_slot);
            continue;
        }
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

// IsUseAfterConfirm 类扩展物品（如卓越灵药）的确认使用回调分流：
// 原版回调会先 INVEN_FindItemSlot（扩展物品不在 INVEN 被拦），此处直接
// CHAR_UseItemEx 后清理 hooks/描述。身份不一致时返回 false，由 Native Hook
// wrapper 调用原版 backup。
bool extension_confirm_use_item(void* item) {
    bool used = false;
    int bag = -1;
    int slot = -1;
    ModuleUseReleasePlan release_plan{};
    {
        std::unique_lock<std::mutex> lock(g_virtual_bag_mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            VIRTBAG_LOG("extension confirm-use lock busy item=%p", item);
            return false;
        }
        if (!extension_bag_enabled() || item == nullptr || item != g_extension_desc_item) {
            return false;
        }
        bag = g_extension_desc_item_bag;
        slot = g_extension_desc_item_slot;
        int module_bag = -1;
        int module_slot = -1;
        if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            g_virtual_bag_state.items[bag][slot].category <= 0 ||
            g_virtual_bag_state.items[bag][slot].count <= 0 ||
            g_module_objects[bag][slot] != item ||
            !module_slot_of_item_locked(item, &module_bag, &module_slot) ||
            module_bag != bag || module_slot != slot) {
            return false;
        }
        void* character = extension_menu_character();
        if (character == nullptr || fn_char_use_item_ex == nullptr) {
            VIRTBAG_LOG("extension confirm-use unavailable bag=%d slot=%d character=%p fn=%p",
                        bag, slot, character, reinterpret_cast<void*>(fn_char_use_item_ex));
            return false;
        }
        ModuleUseToken use_token{};
        if (!module_use_begin_locked(bag, slot, item, &use_token)) {
            VIRTBAG_LOG("extension confirm-use already active bag=%d slot=%d item=%p",
                        bag, slot, item);
            return false;
        }
        // CHAR_UseItemEx 会同步进入 INVEN_ConsumeItem；不得持有背包锁调用。
        ModuleUseToken* previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        lock.unlock();
        used = fn_char_use_item_ex(character, item, 0) != 0;
        lock.lock();
        g_active_module_use_token = previous_use_token;
        bool consumed = false;
        const bool use_token_valid = module_use_finish_locked(use_token, &consumed, &release_plan);
        const bool same_item = g_module_objects[bag][slot] == item;
        if (!use_token_valid) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG("ERROR extension confirm-use token lost after use bag=%d slot=%d item=%p marker_cleared=%d; treated handled",
                        bag, slot, item, isolated ? 1 : 0);
            if (isolated) {
                reset_extension_desc_item_hooks_locked();
                g_extension_desc_item = nullptr;
                g_extension_desc_item_bag = -1;
                g_extension_desc_item_slot = -1;
            } else {
                VIRTBAG_LOG("ERROR extension confirm-use abort failed; slot may remain active "
                            "bag=%d slot=%d item=%p",
                            bag, slot, item);
            }
            return true;
        }
        if (!used) {
            VIRTBAG_LOG("extension confirm-use executed failed bag=%d slot=%d", bag, slot);
            return true;
        }
        if (!same_item && !consumed) {
            VIRTBAG_LOG("extension confirm-use identity changed unexpectedly bag=%d slot=%d item=%p",
                        bag, slot, item);
            return true;
        }
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            VIRTBAG_LOG("ERROR extension confirm-use native release failed bag=%d slot=%d",
                        bag, slot);
            return true;
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            VIRTBAG_LOG("ERROR extension confirm-use release finalize failed bag=%d slot=%d",
                        bag, slot);
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        // native 释放和对象摘除完成后再刷新并清理详情控件，避免悬挂绑定。
        refresh_module_item_area_locked(bag);
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
    }
    VIRTBAG_LOG("extension confirm-use handled bag=%d slot=%d used=1", bag, slot);
    return true;
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
    const uintptr_t use_after_confirm_raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_BUTTON_USE_AFTER_CONFIRM_EXE_VMA",
                              F_UIEQUIP_BUTTON_USE_AFTER_CONFIRM_EXE_VMA)
        : 0;
    const bool direct_use = use_raw != 0 &&
                            reinterpret_cast<uintptr_t>(hook->orig) == use_raw;
    const bool confirm_use = use_after_confirm_raw != 0 &&
                             reinterpret_cast<uintptr_t>(hook->orig) == use_after_confirm_raw;
    if (direct_use || confirm_use) {
        bool used = false;
        bool current_item = false;
        void* leader = nullptr;
        bool closes_description = false;
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            current_item = item != nullptr && virtual_bag::valid_index(item_bag) &&
                           item_slot >= 0 && item_slot < virtual_bag::kSlotCount &&
                           g_module_objects[item_bag][item_slot] == item;
            leader = current_item ? extension_menu_character() : nullptr;
            closes_description = current_item;
        }
        if (leader == nullptr || fn_char_use_item_ex == nullptr || !current_item) {
            // 扩展侧没有完整接管前置条件时，direct/confirm 都必须回退原版按钮，
            // 不能清状态或吞掉原版确认弹窗及其 OK 回调。
            hook->call_orig<void>(button);
            return;
        }
        if (current_item && leader != nullptr && fn_char_use_item_ex != nullptr) {
            if (confirm_use) {
                // IsUseAfterConfirm 类物品（如卓越灵药）：原版按钮会弹确认
                // YesNo，其 OK 回调（UIEquip_OKConfrimUseItem）在调
                // CHAR_UseItemEx 前先 INVEN_FindItemSlot 定位——扩展物品不在
                // INVEN 会被拦截而无效果。确认使用函数入口由常驻 LSPosed
                // Native Hook 接管，按回调传入的物品身份分流；此处只让原版
                // 按钮创建确认弹窗。弹窗创建/关闭不持有背包锁。
                if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
                hook->call_orig<void>(button);
                return;
            }
            used = fn_char_use_item_ex(leader, item, 0) != 0;
        }
        {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            if (closes_description) {
                if (used) refresh_module_item_area_locked(item_bag);
                reset_extension_desc_item_hooks_locked();
                g_extension_desc_item = nullptr;
                g_extension_desc_item_bag = -1;
                g_extension_desc_item_slot = -1;
            }
        }
        VIRTBAG_LOG("extension use execute bag=%d slot=%d confirm=%d used=%d", item_bag, item_slot,
                    confirm_use ? 1 : 0, used ? 1 : 0);
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
                                      g_module_objects[bag][slot] == item &&
                                      module_object_release_allowed_locked(
                                          bag, slot, item, "extension sell popup", false, nullptr);
            int category = -1;
            int count = 0;
            int64_t unit_price = -1;
            uint32_t payload = 0;
            uint64_t generation = 0;
            if (current_item && fn_item_get_sell_price != nullptr &&
                fn_item_is_no_sell != nullptr && fn_get_bit != nullptr) {
                count = g_virtual_bag_state.items[bag][slot].count;
                const uint16_t flags = *reinterpret_cast<uint16_t*>(
                    reinterpret_cast<uint8_t*>(item) + I_TYPE);
                category = fn_get_bit(flags, 15, 6);
                if (fn_item_is_no_sell(category) == 0 && count > 0) {
                    g_extension_sell_apply_variant_discount = modified_destroy_sell;
                    unit_price = static_cast<int64_t>(fn_item_get_sell_price(item));
                    price = extension_bag_sell_price(item, count, modified_destroy_sell);
                    if (price >= 0) {
                        valid = true;
                        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
                        payload = virtual_bag::payload_hash(descriptor);
                        generation = g_module_object_use[bag][slot].generation;
                        const int legal_count = static_cast<int>(stack_codec::clamp_count(
                            static_cast<uint32_t>(count), stack_limit_enabled()));
                        VIRTBAG_LOG(
                            "sell price source=equip category=%d count=%d unit=%lld variant=%d final=%lld payload=%u gen=%llu",
                            category, legal_count, static_cast<long long>(unit_price),
                            modified_destroy_sell ? 70 : 100, static_cast<long long>(price), payload,
                            static_cast<unsigned long long>(generation));
                    } else {
                        VIRTBAG_LOG(
                            "sell price reject reason=price_bounds source=equip category=%d count=%d unit=%lld variant=%d payload=%u gen=%llu",
                            category, count, static_cast<long long>(unit_price),
                            modified_destroy_sell ? 70 : 100, payload,
                            static_cast<unsigned long long>(g_module_object_use[bag][slot].generation));
                    }
                } else {
                    VIRTBAG_LOG(
                        "sell price reject reason=no_sell_or_count source=equip category=%d count=%d variant=%d",
                        category, count, modified_destroy_sell ? 70 : 100);
                }
            } else if (!current_item) {
                VIRTBAG_LOG("sell price reject reason=stale_or_release_guard source=equip bag=%d slot=%d",
                            bag, slot);
            } else {
                VIRTBAG_LOG("sell price reject reason=symbols source=equip bag=%d slot=%d",
                            bag, slot);
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
            VIRTBAG_LOG("sell price reject reason=popup_unavailable source=equip valid=%d",
                        valid ? 1 : 0);
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
    const virtual_bag::Item previous_descriptor = descriptor;
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    if (fn_get_bit != nullptr) {
        const uint16_t flags = *reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(item) + I_TYPE);
        descriptor.category = fn_get_bit(flags, 15, 6);
    }
    descriptor.count = fn_get_cumulate_count != nullptr
        ? fn_get_cumulate_count(item) : descriptor.count;
    if (descriptor.category <= 0 || descriptor.count <= 0) {
        descriptor = {};
        if (!free_module_object_locked(item_bag, item_slot)) {
            descriptor = previous_descriptor;
            VIRTBAG_LOG("extension desc sync rejected object release bag=%d slot=%d",
                        item_bag, item_slot);
            reset_extension_desc_item_hooks_locked();
            return;
        }
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
                // 装备/卸下按钮无 PtrHook：装备语义由函数级 hook
                // （ButtonEquipExe 第 10）承担，观察 hook 只覆盖其他详情按钮。
                install_extension_desc_item_hooks_locked(item);
                VIRTBAG_LOG("extension desc item captured bag=%d slot=%d item=%p", item_bag,
                            item_slot, item);
            }
        }
    }
}

// ---- Path 卸下按钮：已函数级 hook 化，无按钮 PtrHook ----
// 卸下按钮 execute proc 恒为 UIEquip_ButtonUnequipExe（0xb7e14），扩展语义由
// native 第 11 hook（button_unequip_exe_wrapper →
// virtual_bag_handle_original_bag_unequip）承担：扩展视图内识别扩展袋 desc
// 解除扩展袋；原版视图解除原版袋并在背包满时放回其他袋行/收编扩展空位。
// g_extension_desc_unequip_bag 仍由扩展袋 desc 打开时记录（供函数 hook 识别），
// 但不再安装按钮 execute-slot PtrHook。

// virtual_bag_handle_backpack_button_equip 定义在 game_ui_virtbag.cpp 的
// 匿名 namespace 之外（本文件整体位于匿名 namespace 内，internal linkage
// 无法被 extension_bag_port.cpp 跨 TU 调用），依赖的本 TU 函数在匿名
// namespace 内仍可见。
