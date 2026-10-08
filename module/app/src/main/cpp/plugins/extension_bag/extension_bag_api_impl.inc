std::string extension_bag_api_status_json_impl() {
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!read_world_in_world()) {
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
           ",\"count\":" +
           std::to_string(static_cast<int>(virtual_bag_effective_count(g_module_objects[internal_bag][slot], static_cast<uint32_t>(item.count)))) +
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
    // 锁内复核当前 epoch 准入：readiness 与真正执行之间可能发生 detach/epoch 变化。
    if (!extension_bag_session_admitted_locked()) return extension_bag_not_ready_error_locked();
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
    bool is_seal = false;
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
        // R-63 佣兵徽章（category 42..50 / 928..933）：ITEMDATA_IsUse 不认（+2=0x1d），
        // 效果只在原版 MERCENARYSYSTEM_MakeMercenary，CHAR_UseItemEx 不实现，故豁免
        // fn_is_use 并按原版按钮顺序 SAVE_IsOK→IsEmptyManagerSlot→MakeMercenary。
        is_seal = read_item_is_mercenary_seal(descriptor.category);
        if (is_seal) {
            const auto* save_facts = qol::features::extension_bag::save_facts();
            if (save_facts == nullptr || save_facts->is_ok == nullptr ||
                g_character_extras->read.has_empty_mercenary_slot == nullptr ||
                extension_bag_write_api() == nullptr ||
                extension_bag_write_api()->equipment.make_mercenary == nullptr) {
                return op_err("symbol not resolved");
            }
        } else if (g_game == nullptr || g_game->read_only.item.is_usable == nullptr ||
                   extension_bag_write_api() == nullptr ||
                   extension_bag_write_api()->equipment.use_item == nullptr) {
            return op_err("symbol not resolved");
        }
        if (!is_seal && !read_item_is_usable(descriptor.category)) {
            return op_err("item not usable");
        }

        item = module_item_locked(bag, slot);
        leader = nullptr;
        if (g_game != nullptr && g_character_extras->read.party_member != nullptr) {
            (void)g_character_extras->read.party_member(0, &leader);
        }
        if (item == nullptr) return op_err("extension item unavailable");
        if (!is_seal && leader == nullptr) return op_err("no leader");
        closes_description = g_extension_desc_item == item;
        if (!module_use_begin_locked(bag, slot, item, &use_token)) {
            return op_err("extension item already in use");
        }
    }
    if (closes_description) extension_description_set_off();
    if (is_seal) {
        // SAVE_IsOK/空槽检查不得持背包锁调用；失败先 abort token 再返回明确错误。
        const bool save_ok = read_save_is_ok();
        const bool slot_ok = save_ok && read_has_empty_mercenary_slot();
        if (!save_ok || !slot_ok) {
            std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
            module_use_abort_locked(use_token);
            return op_err(!save_ok ? "save busy" : "mercenary slots full");
        }
        previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        void* mercenary = nullptr;
        used = extension_make_mercenary(item, &mercenary);
        g_active_module_use_token = previous_use_token;
    } else {
        previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        int use_result = 0;
        used = extension_use_item(leader, item, 0, &use_result) && use_result != 0;
        g_active_module_use_token = previous_use_token;
    }
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG_ERROR("ERROR API use token mismatch bag=%d slot=%d item=%p marker_cleared=%d",
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
    int32_t current_tutorial_state = -1;
    const auto* world_facts = qol::features::extension_bag::world_facts();
    if (used && world_facts != nullptr && world_facts->tutorial_state != nullptr &&
        g_game->world_action.cancel_tutorial != nullptr &&
        qol::contracts::succeeded(world_facts->tutorial_state(&current_tutorial_state)) &&
        current_tutorial_state == 6) {
        (void)g_game->world_action.cancel_tutorial();
    }
    return op_ok();
}

QolResult extension_put_jewel_error(const char* message) {
    return qol::contracts::error(kQolErrInternal, message,
                                 static_cast<uint32_t>(std::strlen(message)));
}

QolResult extension_bag_api_put_jewel_typed(int role, int logical_bag, int slot,
                                            int equip_slot) {
    if (!extension_bag_ready_locked()) {
        return extension_put_jewel_error(!g_virtual_bag_enabled.load()
                                             ? "extension bag disabled"
                                             : "not in game");
    }
    const int bag = extension_internal_bag(logical_bag);
    if (bag < 0) return extension_put_jewel_error("bad extension bag (6-10)");
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return extension_put_jewel_error("bad slot");
    if (equip_slot < 0 || equip_slot >= kEquipSlotCount) {
        return extension_put_jewel_error("bad equip slot");
    }

    void* jewel = nullptr;
    void* equip = nullptr;
    int before_count = 0;
    ModuleUseToken use_token{};
    ModuleUseToken* previous_use_token = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        ensure_state_loaded_locked();
        if (slot >= g_virtual_bag_state.capacities[bag]) {
            return extension_put_jewel_error("slot beyond derived capacity");
        }
        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) {
            return extension_put_jewel_error("slot empty");
        }
        if (g_item_facts == nullptr || g_item_facts->is_jewel_category == nullptr ||
            g_character_extras->read.party_member == nullptr ||
            g_character_extras->read.equipped_item == nullptr ||
            g_write_port->equipment.put_jewel == nullptr) {
            return extension_put_jewel_error("symbol not resolved");
        }
        int32_t is_jewel = 0;
        if (!qol::contracts::succeeded(
                g_item_facts->is_jewel_category(descriptor.category, &is_jewel)) ||
            is_jewel == 0) return extension_put_jewel_error("not jewel");

        void* ch = nullptr;
        (void)g_character_extras->read.party_member(role, &ch);
        if (ch == nullptr) return extension_put_jewel_error("role not found");
        jewel = module_item_locked(bag, slot);
        if (!qol::contracts::succeeded(
                g_character_extras->read.equipped_item(ch, equip_slot, &equip))) {
            return extension_put_jewel_error("equip slot unavailable");
        }
        if (jewel == nullptr) return extension_put_jewel_error("extension jewel unavailable");
        if (equip == nullptr) return extension_put_jewel_error("equip slot empty");

        // 消耗前数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
        before_count = static_cast<int>(virtual_bag_effective_count(jewel, static_cast<uint32_t>(descriptor.count)));
        if (!module_use_begin_locked(bag, slot, jewel, &use_token)) {
            return extension_put_jewel_error("extension jewel already in use");
        }
    }

    previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    int32_t result = -1;
    const bool put_jewel_succeeded =
        g_game != nullptr &&
        qol::contracts::succeeded(
            g_write_port->equipment.put_jewel(equip, jewel, &result));
    g_active_module_use_token = previous_use_token;
    if (!put_jewel_succeeded) {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        (void)module_use_abort_locked(use_token);
        return extension_put_jewel_error("put jewel unavailable");
    }

    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG_ERROR("ERROR API put jewel token mismatch bag=%d slot=%d item=%p marker_cleared=%d",
                        bag, slot, jewel, isolated ? 1 : 0);
            return extension_put_jewel_error("extension jewel changed during native operation");
        }
        if (result != 0) {
            return extension_put_jewel_error(result == 2 ? "no socket" : "not jewel");
        }
        ensure_state_loaded_locked();
        if (!consumed) {
            int current_bag = -1;
            int current_slot = -1;
            if (!module_slot_of_item_locked(jewel, &current_bag, &current_slot) ||
                current_bag != bag || current_slot != slot ||
                g_module_objects[bag][slot] != jewel) {
                return extension_put_jewel_error("extension jewel changed during native operation");
            }
            if (!consume_extension_item_after_native_locked(
                    bag, slot, jewel, before_count, before_count, "api_put_jewel", nullptr)) {
                return extension_put_jewel_error("extension jewel state update failed");
            }
        }
        g_item_state_dirty = true;
        refresh_extension_item_operation_locked(bag);
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            return extension_put_jewel_error("extension jewel release failed");
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            return extension_put_jewel_error("extension jewel release finalize failed");
        }
    }
    return qol::contracts::ok();
}

std::string extension_bag_api_put_jewel_impl(int role, int logical_bag, int slot,
                                             int equip_slot) {
    const QolResult result =
        extension_bag_api_put_jewel_typed(role, logical_bag, slot, equip_slot);
    return qol::contracts::succeeded(result) ? op_ok() : op_err(result.message);
}

std::string virtual_bag_inventory_bags_json() {
    if (!g_virtual_bag_enabled.load() || !read_world_in_world()) return "";
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    ensure_state_loaded_locked();
    std::string out;
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        const int capacity = g_virtual_bag_state.capacities[bag];
        int filled = 0;
        std::string items;
        // 只遍历派生容量内的槽位：容量外不是有效槽（R-05），不参与 items 输出
        // 与 slot_count 统计。
        for (int slot = 0; slot < capacity && slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = g_virtual_bag_state.items[bag][slot];
            if (item.category <= 0 || item.count <= 0) continue;
            if (filled > 0) items += ",";
            // count 输出模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
            items += "{\"slot\":" + std::to_string(slot) +
                     ",\"category\":" + std::to_string(item.category) +
                     ",\"count\":" +
                     std::to_string(static_cast<int>(virtual_bag_effective_count(g_module_objects[bag][slot], static_cast<uint32_t>(item.count)))) +
                     "}";
            ++filled;
        }
        out += ",{\"bag\":" + std::to_string(bag + 6) +
               ",\"items\":[" + items + "]" +
               ",\"capacity\":" + std::to_string(capacity) +
               ",\"slot_count\":" + std::to_string(filled) + "}";
    }
    return out;
}
