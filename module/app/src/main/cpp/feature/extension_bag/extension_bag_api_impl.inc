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
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        result = unequip_extension_bag_locked(internal_bag);
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
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    bool moved = false;
    if (from_original && to_extension) {
        moved = move_original_to_extension_slot_locked(from_bag, from_slot, to_internal);
    } else if (from_extension && to_original) {
        moved = move_extension_to_original_locked(from_internal, from_slot, to_bag);
    } else {
        moved = move_extension_to_extension_locked(from_internal, from_slot, to_internal, to_slot);
    }
    if (!moved) return op_err("move failed");
    return "{\"ok\":true,\"state\":" + extension_bag_status_json_locked() + "}";
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
