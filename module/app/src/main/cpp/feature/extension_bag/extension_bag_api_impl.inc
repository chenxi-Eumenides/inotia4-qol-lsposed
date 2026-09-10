std::string extension_bag_api_status_json_impl() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!game_in_world()) {
        return "{\"enabled\":" + std::string(g_virtual_bag_enabled.load() ? "true" : "false") +
               ",\"injected\":false,\"extension_tab_button\":false," +
               "\"inventory_frame_active\":false,\"recovery_action\":\"none\",\"state\":{}}";
    }
    ensure_state_loaded_locked();
    return extension_bag_status_json_locked();
}

std::string extension_bag_api_enter_view_impl(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    // 面板门禁（P3 真机实证）：控件树仅在背包面板打开时有效，面板关闭时
    // enter_view 会投影到 stale 树（点击失效/重建后被 RefreshItemArea 冲掉）。
    if (!g_inventory_frame_active) return op_err("inventory panel not open");
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        return op_err("already in extension view");
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    handle_extension_tab_click_locked(internal_bag);
    const bool entered = g_virtual_bag_state.mode == virtual_bag::Mode::kModule;
    return extension_bag_view_result_json(entered, "enter extension view failed");
}

std::string extension_bag_api_exit_view_impl() {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kExitingModule) {
        return op_err("exit in progress");
    }
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule || g_virtual_bag_state.selected < 0) {
        return op_err("not in extension view");
    }
    handle_extension_tab_click_locked(g_virtual_bag_state.selected);
    const bool exited = g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal;
    return extension_bag_view_result_json(exited, "exit extension view failed");
}

std::string extension_bag_api_select_bag_impl(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (g_virtual_bag_state.selected == internal_bag) return op_err("bag already selected");
    handle_extension_tab_click_locked(internal_bag);
    const bool selected = g_virtual_bag_state.selected == internal_bag;
    return extension_bag_view_result_json(selected, "select bag failed");
}

std::string extension_bag_api_click_item_impl(int logical_bag, int slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return op_err("not in extension view");
    }
    if (g_virtual_bag_state.selected != internal_bag) return op_err("bag not selected");
    if (slot >= g_virtual_bag_state.capacities[internal_bag]) {
        return op_err("slot beyond derived capacity");
    }
    const virtual_bag::Item& item = g_virtual_bag_state.items[internal_bag][slot];
    if (item.category <= 0 || item.count <= 0) {
        g_virtual_bag_state.inspected = -1;
        persist_state_locked();
        return "{\"ok\":true,\"item\":null}";
    }
    g_virtual_bag_state.inspected = slot;
    persist_state_locked();
    return "{\"ok\":true,\"item\":{\"bag\":" + std::to_string(logical_bag) +
           ",\"slot\":" + std::to_string(slot) +
           ",\"category\":" + std::to_string(item.category) +
           ",\"count\":" + std::to_string(item.count) +
           ",\"payload\":\"" +
           virtual_bag::base64_encode(item.payload.data(), item.payload_size) + "\"}}";
}

// P3 袋解除（原版语义）：非空拒绝、无空位拒绝、成功 = 按类型重建袋物品
// 入库 + 装备清零 + 切到接收袋。HTTP 路径不弹窗，仅返回错误码。
std::string extension_bag_api_unequip_impl(int logical_bag) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int internal_bag = extension_internal_bag(logical_bag);
    if (internal_bag < 0) return op_err("bad extension bag (6-10)");
    ExtensionBagUnequipResult result = ExtensionBagUnequipResult::kNotEquipped;
    {
        std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
        result = unequip_extension_bag_locked(lock, internal_bag);
    }
    if (result == ExtensionBagUnequipResult::kOk) return op_ok();
    if (result == ExtensionBagUnequipResult::kNotEmpty) return op_err("bag not empty");
    if (result == ExtensionBagUnequipResult::kNoSpace) return op_err("no space");
    if (result == ExtensionBagUnequipResult::kPersistFailed) return op_err("persist failed");
    if (result == ExtensionBagUnequipResult::kFailed) return op_err("unequip failed");
    return op_err("bag not equipped");
}

std::string extension_bag_api_move_item_impl(int from_bag, int from_slot, int to_bag, int to_slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    if (g_module_view_installed) return op_err("extension view open; movement disabled (P2)");
    if (from_bag == virtual_bag::kOriginalTaskBag ||
        to_bag == virtual_bag::kOriginalTaskBag) {
        return op_err("task bag excluded");
    }
    if (from_slot < 0 || from_slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    const bool from_original = virtual_bag::valid_original_transaction_bag(from_bag);
    const bool to_original = virtual_bag::valid_original_transaction_bag(to_bag);
    const int from_internal = extension_internal_bag(from_bag);
    const int to_internal = extension_internal_bag(to_bag);
    const bool from_extension = from_internal >= 0;
    const bool to_extension = to_internal >= 0;
    if (!from_original && !from_extension) return op_err("bad from bag");
    if (!to_original && !to_extension) return op_err("bad to bag");
    if (from_original && to_original) return op_err("use /api/item/inventory/move_item");
    std::unique_lock<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    bool moved = false;
    if (from_original && to_extension) {
        moved = move_original_to_extension_slot_locked(lock, from_bag, from_slot, to_internal);
    } else if (from_extension && to_original) {
        moved = move_extension_to_original_locked(lock, from_internal, from_slot, to_bag);
    } else {
        moved = move_extension_to_extension_locked(from_internal, from_slot, to_internal, to_slot);
    }
    if (!moved) return op_err("move failed");
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
}

namespace {
void refresh_extension_item_operation_locked(int bag) {
    if (g_module_view_installed && g_module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
}

}  // namespace

std::string extension_bag_api_use_item_impl(int logical_bag, int slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int bag = extension_internal_bag(logical_bag);
    if (bag < 0) return op_err("bad extension bag (6-10)");
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return op_err("bad slot");

    bool used = false;
    void* item = nullptr;
    void* leader = nullptr;
    bool closes_description = false;
    ModuleUseToken use_token{};
    ModuleUseToken* previous_use_token = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (slot >= g_virtual_bag_state.capacities[bag]) return op_err("slot beyond derived capacity");
        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) return op_err("slot empty");
        if (fn_is_use == nullptr || fn_char_use_item_ex == nullptr) {
            return op_err("symbol not resolved");
        }
        if (!fn_is_use(descriptor.category)) return op_err("item not usable");

        item = module_item_locked(bag, slot);
        leader = member_or_null(0);
        if (item == nullptr) return op_err("extension item unavailable");
        if (leader == nullptr) return op_err("no leader");
        closes_description = g_extension_desc_item == item;
        if (!module_use_begin_locked(bag, slot, item, &use_token)) {
            return op_err("extension item already in use");
        }
    }
    if (closes_description && fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
    previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    used = fn_char_use_item_ex(leader, item, 0) != 0;
    g_active_module_use_token = previous_use_token;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG("ERROR API use token mismatch bag=%d slot=%d item=%p marker_cleared=%d",
                        bag, slot, item, isolated ? 1 : 0);
            return op_err("extension item changed during native operation");
        }
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            return op_err("extension item release failed");
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            return op_err("extension item release finalize failed");
        }
    }
    if (!used) return op_err("on cooldown");
    if (closes_description) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        reset_extension_desc_item_hooks_locked();
        g_extension_desc_item = nullptr;
        g_extension_desc_item_bag = -1;
        g_extension_desc_item_slot = -1;
    }
    if (used && tutorial_state() == 6) tutorial_cancel();
    return op_ok();
}

std::string extension_bag_api_put_jewel_impl(int role, int logical_bag, int slot,
                                             int equip_slot) {
    if (!extension_bag_ready_locked()) return extension_bag_not_ready_error_locked();
    const int bag = extension_internal_bag(logical_bag);
    if (bag < 0) return op_err("bad extension bag (6-10)");
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return op_err("bad slot");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad equip slot");

    void* jewel = nullptr;
    void* equip = nullptr;
    int before_count = 0;
    ModuleUseToken use_token{};
    ModuleUseToken* previous_use_token = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (slot >= g_virtual_bag_state.capacities[bag]) return op_err("slot beyond derived capacity");
        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) return op_err("slot empty");
        if (fn_put_jewel == nullptr || fn_is_jewel == nullptr) {
            return op_err("symbol not resolved");
        }
        if (!fn_is_jewel(descriptor.category)) return op_err("not jewel");

        void* ch = member_or_null(role);
        if (ch == nullptr) return op_err("role not found");
        jewel = module_item_locked(bag, slot);
        equip = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) +
                                          C_EQUIP + equip_slot * sizeof(void*));
        if (jewel == nullptr) return op_err("extension jewel unavailable");
        if (equip == nullptr) return op_err("equip slot empty");

        before_count = descriptor.count;
        if (!module_use_begin_locked(bag, slot, jewel, &use_token)) {
            return op_err("extension jewel already in use");
        }
    }

    previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    const int result = fn_put_jewel(equip, jewel);
    g_active_module_use_token = previous_use_token;

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG("ERROR API put jewel token mismatch bag=%d slot=%d item=%p marker_cleared=%d",
                        bag, slot, jewel, isolated ? 1 : 0);
            return op_err("extension jewel changed during native operation");
        }
        if (result != 0) return result == 2 ? op_err("no socket") : op_err("not jewel");
        ensure_state_loaded_locked();
        if (!consumed) {
            int current_bag = -1;
            int current_slot = -1;
            if (!module_slot_of_item_locked(jewel, &current_bag, &current_slot) ||
                current_bag != bag || current_slot != slot ||
                g_module_objects[bag][slot] != jewel) {
                return op_err("extension jewel changed during native operation");
            }
            if (!consume_extension_item_after_native_locked(
                    bag, slot, jewel, before_count, before_count, "api_put_jewel", nullptr)) {
                return op_err("extension jewel state update failed");
            }
        }
        g_item_state_dirty = true;
        refresh_extension_item_operation_locked(bag);
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            return op_err("extension jewel release failed");
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            return op_err("extension jewel release finalize failed");
        }
    }
    return op_ok();
}

std::string virtual_bag_inventory_bags_json() {
    if (!g_virtual_bag_enabled.load() || !game_in_world()) return "";
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    std::string out;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        int filled = 0;
        std::string items;
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = g_virtual_bag_state.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            if (filled > 0) items += ",";
            items += "{\"slot\":" + std::to_string(slot) +
                     ",\"category\":" + std::to_string(item.category) +
                     ",\"count\":" + std::to_string(item.count) + "}";
            ++filled;
        }
        out += ",{\"bag\":" + std::to_string(bag + 6) +
               ",\"items\":[" + items + "]" +
               ",\"capacity\":" + std::to_string(capacity) +
               ",\"slot_count\":" + std::to_string(filled) + "}";
    }
    return out;
}
