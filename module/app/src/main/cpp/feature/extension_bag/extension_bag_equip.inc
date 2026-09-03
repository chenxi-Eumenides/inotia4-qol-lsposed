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
    if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
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

// ---- Path A：装备按钮溢出（原版 ButtonEquipExe 背包分支的扩展对应物）----
// 原版全满判定 = 扫 INVEN_pBagSlot 1..4（b7d58-b7d70）→ 弹窗 6（b7d74）。
// 挂法：BL hook 0xb9188（物品 desc 打开调 MakeDesc）→ 调原函数后 PtrHook
// 装备按钮（panel+0x78，SetDescMenu desc_type=2 装备分支 b8668 创建）的
// execute proc；wrapper 仅在「背包物品 + 原版 1..4 全满 + 扩展有空位」时
// 接管，其余一律透传原 proc（原版行为不变）。
void extension_desc_equip_execute(void* button);

void reset_extension_desc_equip_hook_locked() {
    // 与解除 hook 同款：desc 菜单按钮为游戏所有，可能已销毁，只清模块记录。
    g_extension_desc_equip_hook = {};
    g_extension_desc_equip_item = nullptr;
}

void install_extension_desc_equip_hook_locked(void* item) {
    reset_extension_desc_equip_hook_locked();
    if (g_base == 0 || item == nullptr) return;
    void* button = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_VMA + 0x78);
    if (button == nullptr) return;
    void* data = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(button) + CO_DATA);
    if (data == nullptr) return;
    void* execute_proc = reinterpret_cast<uint8_t*>(data) + CB_EXECUTE_PROC;
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
        reset_extension_desc_equip_hook_locked();
        const bool current_button = button != nullptr && button == hooked_button;
        if (raw != 0 && current_button && g_virtual_bag_enabled.load() && game_in_world() &&
            item != nullptr && fn_get_bit != nullptr && fn_get_bag_size != nullptr) {
            const uint16_t flags =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
            const int category = fn_get_bit(flags, 15, 6);
            if (category >= 1 && category <= 4) {
                bool original_free = false;
                for (int bag = 1; bag <= 4; ++bag) {
                    if (fn_get_bag_size(bag) <= 0) {
                        original_free = true;
                        break;
                    }
                }
                int equip_index = -1;
                if (!original_free) {
                    for (int i = 0; i < virtual_bag::kBagCount; ++i) {
                        if (g_virtual_bag_state.types[i] == 0) {
                            equip_index = i;
                            break;
                        }
                    }
                }
                if (equip_index >= 0) {
                    const ExtensionBagEquipResult result =
                        equip_extension_bag_item_locked(equip_index, item);
                    takeover = result == ExtensionBagEquipResult::kOk;
                    if (takeover) {
                        // 对齐原版 ButtonEquipExe 入口行为（b7c34 UIDesc_SetOff
                        // 关详情）；clear_original_desc_locked 为锁内既有模式。
                        clear_original_desc_locked();
                        finish_extension_equip_ui_locked(equip_index, nullptr);
                    } else {
                        VIRTBAG_LOG("extension equip button overflow result=%d index=%d",
                                    static_cast<int>(result), equip_index);
                    }
                }
            }
        }
    }
    if (!takeover && raw != 0) {
        reinterpret_cast<void (*)(void*)>(raw)(button);
    }
}

// BL 门禁（0xb9188）：物品 desc 打开 → 调原 MakeDesc（同步生成含装备按钮的
// 菜单）→ 锁内重装装备按钮 hook。物品从触发控件 data[0] 取（MakeDesc 同源）。
void make_desc_equip_gate(void* ctrl, void* arg) {
    const uintptr_t raw = g_base != 0
        ? g_base + fn_resolve("F_UIEQUIP_MAKE_DESC_VMA", F_UIEQUIP_MAKE_DESC_VMA)
        : 0;
    if (raw == 0 || ctrl == nullptr) return;
    void* item = nullptr;
    if (fn_control_object_get_data != nullptr) {
        void* data = fn_control_object_get_data(ctrl);
        item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    }
    reinterpret_cast<UiEquipMakeDescFn>(raw)(ctrl, arg);
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_virtual_bag_enabled.load() && game_in_world()) {
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

