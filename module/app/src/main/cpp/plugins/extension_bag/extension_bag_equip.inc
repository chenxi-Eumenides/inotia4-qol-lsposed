enum class ExtensionBagEquipResult {
    kOk,
    kReject,         // 前置不满足：非背包物品 / 扩展位已装备 / 不在原版库存
    kFailed,         // 移除或回滚失败（状态可能不一致，强日志）
    kPersistFailed,  // persist 失败且已完整回滚
};

void* extension_menu_character() {
    if (g_game == nullptr || g_character_extras->read.menu_character == nullptr) {
        return nullptr;
    }
    void* character = nullptr;
    return qol::contracts::succeeded(
               g_character_extras->read.menu_character(&character))
               ? character
               : nullptr;
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
        if (src_bag < 0 || !read_item_category(item, &category)) {
            return ExtensionBagEquipResult::kReject;
        }
    }
    if (!is_backpack_category(category)) return ExtensionBagEquipResult::kReject;
    const auto* write_api = extension_bag_write_api();
    if (!module_source &&
        (write_api == nullptr || write_api->inventory.remove_item_direct == nullptr ||
         write_api->inventory.save_item_on_empty == nullptr ||
         write_api->inventory.load_item == nullptr || write_api->inventory.free_item == nullptr)) {
        return ExtensionBagEquipResult::kFailed;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (module_source) {
        const virtual_bag::Item& source = g_virtual_bag_state.items[src_bag][src_slot];
        payload = source.payload;
        payload_size = source.payload_size;
    } else if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip reject reason=payload_serialize_failed src=%d/%d", src_bag,
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
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            if (!free_module_object_locked(src_bag, src_slot)) {
                g_virtual_bag_state.types[internal_bag] = previous_type;
                g_virtual_bag_state.items[src_bag][src_slot] = source;
                g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
                virtual_bag::normalize(&g_virtual_bag_state);
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip rejected source release bag=%d slot=%d",
                            src_bag, src_slot);
                return ExtensionBagEquipResult::kFailed;
            }
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension equip ok source=extension bag=%d category=%d src=%d/%d",
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
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip ledger exhausted src=%d/%d", src_bag, src_slot);
        return ExtensionBagEquipResult::kFailed;
    }

    if (!remove_item_direct_unlocked(lock, src_bag, src_slot, item, "extension equip")) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip remove verify failed bag=%d slot=%d", src_bag, src_slot);
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
        extension_bag_mark_dirty_locked();
        if (persist_state_locked()) {
            // P4.3：装备对象已被状态 payload 取代（原指针无人引用），终止保管；
            // drop 路径处于触摸窗口内 → 隔离区，API 路径窗口外 → 真释放。
            retire_custody_item_locked(custody_handle, item, "equip custody retired");
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension equip ok bag=%d category=%d src=%d/%d", internal_bag,
                        category, src_bag, src_slot);
            return ExtensionBagEquipResult::kOk;
        }
        g_virtual_bag_state.types[internal_bag] = 0;
        g_virtual_bag_state.isolation_now_ms = isolation_now_ms_locked();
        virtual_bag::normalize(&g_virtual_bag_state);
    } else {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip state reject bag=%d category=%d", internal_bag, category);
    }
    bool restored = false;
    uint32_t rebuilt_handle = 0;
    void* rebuilt = load_item_payload_tracked_locked(payload.data(), payload_size,
                                                     "equip_rollback", &rebuilt_handle);
    if (rebuilt != nullptr) {
        if (extension_save_item_on_empty(rebuilt, src_bag)) {
            handover_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback handover");
            restored = true;
        } else {
            release_tracked_item_locked(rebuilt_handle, rebuilt, "equip rollback insert failed");
        }
    }
    retire_custody_item_locked(custody_handle, item, "equip rollback custody retired");
    persist_state_locked();
    QOL_LOG_WARN(QolDomain::kExtensionBag, "extension equip persist failed bag=%d restored=%d", internal_bag,
                restored ? 1 : 0);
    return restored ? ExtensionBagEquipResult::kPersistFailed : ExtensionBagEquipResult::kFailed;
}

bool equip_module_item_on_character_locked(std::unique_lock<std::mutex>& lock,
                                           void* character, void* item, int source_bag,
                                           int source_slot) {
    if (!lock.owns_lock() || character == nullptr || item == nullptr ||
        !virtual_bag::valid_index(source_bag) ||
        source_slot < 0 || source_slot >= virtual_bag::kSlotCount ||
        g_module_objects[source_bag][source_slot] != item ||
        g_game == nullptr || g_character_extras->read.can_equip == nullptr ||
        g_character_extras->read.find_equip_slot == nullptr ||
        extension_bag_write_api() == nullptr ||
        !g_module_view_installed || g_module_view_index != source_bag ||
        g_module_window_original_bag < 0 || g_module_window_original_bag >= 5) {
        return false;
    }
    if (!module_object_replacement_allowed_locked(source_bag, source_slot,
                                                  "character equip")) {
        return false;
    }
    if (!read_char_can_equip(character, item)) return false;

    int target = -1;
    if (!read_char_find_equip_slot(character, item, &target)) return false;
    if (target < 0) return false;
    const int original_bag = g_module_window_original_bag;
    const auto* inventory_write = extension_bag_write_api();
    if (inventory_write == nullptr ||
        inventory_write->inventory.set_physical_item == nullptr) return false;

    // Projection deliberately leaves the real inventory untouched. The original
    // exchange entry point reads INVEN[bag][slot], so expose the projected item
    // only for the duration of that call and restore the real slot afterwards.
    void* original_inventory_item = nullptr;
    qol::contracts::QolItemHandle original_inventory_handle{};
    if (g_item_facts == nullptr || g_item_facts->physical_item_at == nullptr ||
        !qol::contracts::succeeded(g_item_facts->physical_item_at(
            original_bag, source_slot, &original_inventory_handle)) ||
        (original_inventory_handle.slot != 0 &&
         !extension_bag_private_item_for_handle(original_inventory_handle,
                                                &original_inventory_item))) return false;
    void* previous_equipped_item = read_char_equipped_item(character, target);
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> previous_equipped_payload{};
    int previous_equipped_payload_size = 0;
    if (previous_equipped_item != nullptr &&
        !serialize_item_payload_locked(previous_equipped_item, &previous_equipped_payload,
                                       &previous_equipped_payload_size)) {
        return false;
    }
    const uint32_t source_handle = g_module_object_handles[source_bag][source_slot];
    const uint64_t source_generation = g_module_object_use[source_bag][source_slot].generation;
    const uint64_t source_hash = virtual_bag::payload_hash(
        g_virtual_bag_state.items[source_bag][source_slot]);
    if (!qol::contracts::succeeded(inventory_write->inventory.set_physical_item(
            original_bag, source_slot, item))) return false;

    // 原版交换入口会重入库存 hook，因此临时安装物理槽后必须解锁调用，再重锁复核。
    extension_bag_begin_internal_equip();
    lock.unlock();
    const bool equip_result = extension_bag_write_api() != nullptr &&
                              extension_equip_from_inventory(character, original_bag,
                                                             source_slot, target);
    extension_bag_end_internal_equip();
    lock.lock();
    const QolResult restore_result = inventory_write->inventory.set_physical_item(
        original_bag, source_slot, original_inventory_item);
    if (!qol::contracts::succeeded(restore_result)) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR extension character equip physical slot restore failed bag=%d slot=%d item=%p code=%d",
                      original_bag, source_slot, original_inventory_item, restore_result.code);
        return false;
    }
    const bool source_unchanged = g_module_objects[source_bag][source_slot] == item &&
        g_module_object_handles[source_bag][source_slot] == source_handle &&
        g_module_object_use[source_bag][source_slot].generation == source_generation &&
        virtual_bag::payload_hash(g_virtual_bag_state.items[source_bag][source_slot]) ==
            source_hash &&
        g_module_view_installed && g_module_view_index == source_bag &&
        g_module_window_original_bag == original_bag;
    if (!source_unchanged) {
        QOL_LOG_ERROR(QolDomain::kExtensionBag,
                      "ERROR extension character equip source changed during original exchange bag=%d slot=%d item=%p handle=%u generation=%llu",
                      source_bag, source_slot, item, source_handle,
                      static_cast<unsigned long long>(source_generation));
        return false;
    }
    if (!equip_result || read_char_equipped_item(character, target) != item) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension character equip original exchange failed bag=%d slot=%d target=%d result=%d",
                     source_bag, source_slot, target, equip_result ? 1 : 0);
        return false;
    }
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
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension character equip ownership handover failed bag=%d slot=%d",
                        source_bag, source_slot);
        }
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[source_bag][source_slot];
    if (previous_equipped_item != nullptr) {
        descriptor.payload = previous_equipped_payload;
        descriptor.payload_size = previous_equipped_payload_size;
        (void)read_item_category(previous_equipped_item, &descriptor.category);
        int previous_count = 1;
        descriptor.count = read_item_cumulate_count(previous_equipped_item, &previous_count)
            ? static_cast<int>(virtual_bag_canonical_count(previous_equipped_item, 1)) : previous_count;
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
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension character equip adopt ledger exhausted bag=%d slot=%d",
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
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension character equip ok bag=%d slot=%d target=%d", source_bag,
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
    if (tab != nullptr) {
        void* data = extension_bag_ui_data(tab);
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
    } else if (g_game != nullptr && g_write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    extension_update_character_equip();
    if (moving_control != nullptr) reset_drag_state_locked(moving_control);
    if (!switched_view) play_extension_switch_sound();
}

bool try_equip_on_extension_tab_drop_locked(std::unique_lock<std::mutex>& lock,
                                            int index, void* moving_control) {
    if (!g_virtual_bag_enabled.load() || !read_world_in_world()) return false;
    void* data = extension_bag_ui_data(moving_control);
    void* item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
    if (item == nullptr) return false;
    const ExtensionBagEquipResult result = equip_extension_bag_item_locked(lock, index, item);
    if (result != ExtensionBagEquipResult::kOk) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension tab drop equip result=%d index=%d", static_cast<int>(result),
                    index);
        return false;
    }
    finish_extension_equip_ui_locked(index, moving_control);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension tab drop equipped index=%d", index);
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
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension source tab equip result=%d target=%d source=%d/%d",
                    static_cast<int>(result), target_bag, source_bag, source_slot);
        return false;
    }
    finish_extension_equip_ui_locked(target_bag, nullptr);
    return true;
}

// ---- Path A：装备按钮语义（函数级 hook）----
// 详情"装备"按钮 execute proc 恒为 UIEquip_ButtonEquipExe（0xb7c18），因此
// 其全部扩展语义由 native 第 10 hook（button_equip_exe_wrapper →
// virtual_bag_handle_backpack_button_equip）承担：背包物品（装原版/扩展袋）
// 与扩展槽装备物品（装角色）在原函数内联删源之前分流；其余交还原函数。
// 装备/卸下按钮不再由详情实例绑定器接管，避免与函数级入口 hook 重复执行。




void reset_extension_desc_item_hooks_locked() {
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr) {
        g_extension_desc_item_bound.fill(false);
        g_extension_desc_item_ids.fill(0);
        return;
    }
    for (size_t index = 0; index < g_extension_desc_item_bound.size(); ++index) {
        const uint64_t id = g_extension_desc_item_ids[index];
        if (!g_extension_desc_item_bound[index] || id == 0) {
            g_extension_desc_item_bound[index] = false;
            g_extension_desc_item_ids[index] = 0;
            continue;
        }
        const QolResult result = api->unbind(id);
        if (!qol::contracts::succeeded(result) && result.code != kQolErrNotFound &&
            result.code != kQolErrNotReady) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension desc hook unbind failed index=%d id=%llu reason=host_unbind_error code=%d message=%.*s",
                         static_cast<int>(index), static_cast<unsigned long long>(id), result.code,
                         static_cast<int>(result.message_len),
                         result.message != nullptr ? result.message : "");
        }
        g_extension_desc_item_bound[index] = false;
        g_extension_desc_item_ids[index] = 0;
    }
}

void extension_desc_item_execute_at(size_t index, void* button);
void extension_desc_item_execute_0(void* button) { extension_desc_item_execute_at(0, button); }
void extension_desc_item_execute_1(void* button) { extension_desc_item_execute_at(1, button); }
void extension_desc_item_execute_2(void* button) { extension_desc_item_execute_at(2, button); }
void extension_desc_item_execute_3(void* button) { extension_desc_item_execute_at(3, button); }
void extension_desc_item_execute_4(void* button) { extension_desc_item_execute_at(4, button); }
void extension_desc_item_execute_5(void* button) { extension_desc_item_execute_at(5, button); }
void extension_desc_item_execute_6(void* button) { extension_desc_item_execute_at(6, button); }
void extension_desc_item_execute_7(void* button) { extension_desc_item_execute_at(7, button); }
void extension_desc_item_execute_8(void* button) { extension_desc_item_execute_at(8, button); }
void extension_desc_item_execute_9(void* button) { extension_desc_item_execute_at(9, button); }
void extension_desc_item_execute_10(void* button) { extension_desc_item_execute_at(10, button); }

constexpr std::array<qol::contracts::QolDescButtonProc, 11> kDescButtonProcs = {
    &extension_desc_item_execute_0, &extension_desc_item_execute_1,
    &extension_desc_item_execute_2, &extension_desc_item_execute_3,
    &extension_desc_item_execute_4, &extension_desc_item_execute_5,
    &extension_desc_item_execute_6, &extension_desc_item_execute_7,
    &extension_desc_item_execute_8, &extension_desc_item_execute_9,
    &extension_desc_item_execute_10,
};

bool extension_desc_original_matches(uint64_t id, const char* symbol) {
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr || id == 0 || symbol == nullptr) return false;
    int32_t match = 0;
    return qol::contracts::succeeded(api->original_matches(id, symbol, &match)) && match != 0;
}

bool extension_desc_call_original(uint64_t id, void* button) {
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    return api != nullptr && id != 0 && button != nullptr &&
           qol::contracts::succeeded(api->call_original(id, button));
}

void extension_destroy_ok(void*);
void extension_sell_ok(void*);
void extension_destroy_cancel(void*);
void extension_sell_cancel(void*);
bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context);

void restore_extension_desc_popup_callbacks_locked() {
    if (g_game != nullptr &&
        g_write_port->ui.restore_confirmation != nullptr) {
        (void)g_write_port->ui.restore_confirmation(
            1, reinterpret_cast<void*>(&extension_destroy_ok),
            reinterpret_cast<void*>(&extension_destroy_cancel),
            reinterpret_cast<void*>(g_extension_destroy_original_ok),
            reinterpret_cast<void*>(g_extension_destroy_original_cancel));
        (void)g_write_port->ui.restore_confirmation(
            0, reinterpret_cast<void*>(&extension_sell_ok),
            reinterpret_cast<void*>(&extension_sell_cancel),
            reinterpret_cast<void*>(g_extension_sell_original_ok),
            reinterpret_cast<void*>(g_extension_sell_original_cancel));
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
    extension_description_set_off();
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
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected stale detail bag=%d slot=%d item=%p", bag,
                        slot, item);
            reset_extension_desc_item_hooks_locked();
            g_extension_desc_item = nullptr;
            g_extension_desc_item_bag = -1;
            g_extension_desc_item_slot = -1;
            clear_original_desc_locked();
            return;
        }
        if (g_game == nullptr || g_item_facts == nullptr || g_item_facts->sell_price == nullptr ||
            g_item_facts->is_no_sell == nullptr || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.add_money == nullptr ||
            extension_bag_write_api()->equipment.minus_money == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected symbols unavailable bag=%d slot=%d", bag, slot);
            clear_original_desc_locked();
            return;
        }
                // 卖出数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
                count = static_cast<int>(virtual_bag_effective_count(
                    item, static_cast<uint32_t>(g_virtual_bag_state.items[bag][slot].count)));
        int category = -1;
        int32_t no_sell = 0;
        if (!read_item_category(item, &category) ||
            !qol::contracts::succeeded(g_item_facts->is_no_sell(category, &no_sell)) ||
            no_sell != 0) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected no-sell item bag=%d slot=%d category=%d",
                        bag, slot, category);
            clear_original_desc_locked();
            return;
        }
        int64_t unit_price = 0;
        if (!read_item_sell_price(item, &unit_price)) {
            clear_original_desc_locked();
            return;
        }
        price = extension_bag_sell_price(item, count, apply_variant_discount);
        if (price < 0) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                "sell price reject reason=price_bounds source=equip category=%d count=%d unit=%lld variant=%d payload=%u gen=%llu",
                category, count, static_cast<long long>(unit_price),
                apply_variant_discount ? 70 : 100,
                virtual_bag::payload_hash(g_virtual_bag_state.items[bag][slot]),
                static_cast<unsigned long long>(g_module_object_use[bag][slot].generation));
            clear_original_desc_locked();
            return;
        }
        QOL_LOG_INFO(QolDomain::kExtensionBag,
            "sell price source=equip category=%d count=%d unit=%lld variant=%d final=%lld payload=%u gen=%llu",
            category,
            count,
            static_cast<long long>(unit_price), apply_variant_discount ? 70 : 100,
            static_cast<long long>(price),
            virtual_bag::payload_hash(g_virtual_bag_state.items[bag][slot]),
            static_cast<unsigned long long>(g_module_object_use[bag][slot].generation));
    }

    if (price < 0 || !extension_add_money(price)) {
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
                                  virtual_bag_effective_count(item, static_cast<uint32_t>(g_virtual_bag_state.items[bag][slot].count)) ==
                                      static_cast<uint32_t>(count) &&
                                  module_object_release_allowed_locked(
                                      bag, slot, item, "extension sell commit", false, nullptr);
        if (current_item) {
            const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
            g_virtual_bag_state.items[bag][slot] = {};
            extension_bag_mark_dirty_locked();
            if (!free_module_object_locked(bag, slot)) {
                g_virtual_bag_state.items[bag][slot] = previous_item;
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected object release bag=%d slot=%d", bag, slot);
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
        const bool refunded = extension_minus_money(price);
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
            extension_bag_mark_dirty_locked();
            if (!free_module_object_locked(bag, slot)) {
                g_virtual_bag_state.items[bag][slot] = previous_item;
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension destroy rejected object release bag=%d slot=%d", bag, slot);
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
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension destroy rejected stale detail bag=%d slot=%d", bag, slot);
        }
    }
    if (committed && g_game != nullptr &&
        g_write_port->ui.refresh_item_area != nullptr) {
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
    int observed_count = before_count - 1;
    (void)read_item_cumulate_count(item, &observed_count);
    int count = observed_count;
    if (count == observed_before) --count;
    if (count >= before_count) count = before_count - 1;
    if (count < 0) count = 0;
    uint32_t old_count_flags = 0;
    if (!read_item_encoded_field(item, &old_count_flags)) return false;
    if (count == 0) {
        const virtual_bag::Item previous_item = g_virtual_bag_state.items[bag][slot];
        g_virtual_bag_state.items[bag][slot] = {};
        if (!free_module_object_locked(bag, slot, raw_use_token)) {
            g_virtual_bag_state.items[bag][slot] = previous_item;
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension consume rejected object release context=%s bag=%d slot=%d",
                        context, bag, slot);
            return false;
        }
        return true;
    }
    const ::qol::contracts::QolItemCountEncoding count_encoding = item_count_encoding(item);
    if (count_encoding != ::qol::contracts::QolItemCountEncoding::kEncoded) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension consume rejected count encoding=%d context=%s bag=%d slot=%d",
                    static_cast<int>(count_encoding), context, bag, slot);
        return false;
    }
    // S2 写回（R-47 决策 b 模式感知）：count 为 getter 返回的模式视图数量
    //（启用态 canonical 128a+b、关闭态 b），扣减后的写回经 item-count facade：
    // 启用态拆段写回借位自动完成（a 借位 → b+128）；关闭态只写 b、保留 a——
    // descriptor 随后回写 canonical 解码值，重开后读回完整值。非可堆叠类别已在
    // 上方按三态类别门控拒绝，不会触碰 marker/S2 重叠位段。
    if (!virtual_bag_write_effective(item, static_cast<uint32_t>(count))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension consume rejected count write context=%s bag=%d slot=%d",
                     context, bag, slot);
        return false;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        const auto* count_api = item_count_api();
        if (count_api != nullptr && count_api->write_encoded_field != nullptr) {
            (void)count_api->write_encoded_field(item, old_count_flags);
        }
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension consume reserialize failed context=%s bag=%d slot=%d",
                    context, bag, slot);
        return false;
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    // descriptor.count 恒为 canonical：关闭态 b 写保留 a，重开读回完整值。
    descriptor.count = static_cast<int>(virtual_bag_canonical_count(
        item, virtual_bag_canonical_from_field(old_count_flags,
                                               static_cast<uint32_t>(descriptor.count))));
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
    if (item == nullptr) return;
    static bool unavailable_logged = false;
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr) {
        if (!unavailable_logged) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension desc hook skipped reason=desc hook v2 unavailable");
            unavailable_logged = true;
        }
        return;
    }
    unavailable_logged = false;

    std::array<void*, 11> resolved_execute_slots{};
    size_t resolved_execute_slot_count = 0;
    for (size_t index = 0; index < g_extension_desc_item_bound.size(); ++index) {
        if (index == 2) continue;
        if (g_extension_desc_item_bound[index] || g_extension_desc_item_ids[index] != 0) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook skip index=%d reason=already_bound id=%llu",
                          static_cast<int>(index),
                          static_cast<unsigned long long>(g_extension_desc_item_ids[index]));
            continue;
        }

        qol::contracts::QolDescButtonTarget target{};
        const QolResult resolve_result = api->resolve(static_cast<uint32_t>(index), &target);
        if (!qol::contracts::succeeded(resolve_result)) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook resolve skip index=%d code=%d",
                          static_cast<int>(index), resolve_result.code);
            continue;
        }
        if (target.execute_slot == nullptr) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook resolve skip index=%d reason=null_execute_slot",
                          static_cast<int>(index));
            continue;
        }

        bool duplicate_execute_slot = false;
        for (size_t seen = 0; seen < resolved_execute_slot_count; ++seen) {
            if (resolved_execute_slots[seen] == target.execute_slot) {
                duplicate_execute_slot = true;
                break;
            }
        }
        if (duplicate_execute_slot) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook resolve skip index=%d reason=duplicate_execute_slot execute=%p",
                          static_cast<int>(index), target.execute_slot);
            continue;
        }

        uint64_t id = 0;
        const QolResult bind_result = api->bind(static_cast<uint32_t>(index), &target,
                                                 kDescButtonProcs[index], &id);
        if (!qol::contracts::succeeded(bind_result) || id == 0) {
            QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                          "extension desc hook bind skip index=%d code=%d id=%llu",
                          static_cast<int>(index), bind_result.code,
                          static_cast<unsigned long long>(id));
            continue;
        }
        // only successful bindings occupy an execute slot for dedupe; a failed
        // bind must not hide a later alias that can still be installed.
        resolved_execute_slots[resolved_execute_slot_count++] = target.execute_slot;

        int32_t equip_match = 0;
        int32_t unequip_match = 0;
        const QolResult equip_result = api->original_matches(id, "desc.button.equip", &equip_match);
        const QolResult unequip_result =
            api->original_matches(id, "desc.button.unequip", &unequip_match);
        if (!qol::contracts::succeeded(equip_result) ||
            !qol::contracts::succeeded(unequip_result)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension desc hook classify failed index=%d id=%llu equip_code=%d unequip_code=%d reason=original_matches_error",
                         static_cast<int>(index), static_cast<unsigned long long>(id),
                         equip_result.code, unequip_result.code);
            const QolResult unbind_result = api->unbind(id);
            if (!qol::contracts::succeeded(unbind_result)) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "extension desc hook cleanup failed index=%d id=%llu reason=host_unbind_error code=%d",
                             static_cast<int>(index), static_cast<unsigned long long>(id),
                             unbind_result.code);
                if (unbind_result.code != kQolErrNotFound &&
                    unbind_result.code != kQolErrNotReady &&
                    unbind_result.code != kQolErrConflict) {
                    g_extension_desc_item_ids[index] = id;
                    g_extension_desc_item_bound[index] = true;
                }
            }
            continue;
        }
        if (equip_match != 0 || unequip_match != 0) {
            const QolResult unbind_result = api->unbind(id);
            if (!qol::contracts::succeeded(unbind_result)) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "extension desc hook equip/unequip unbind failed index=%d id=%llu equip=%d unequip=%d reason=host_unbind_error code=%d",
                             static_cast<int>(index), static_cast<unsigned long long>(id),
                             equip_match, unequip_match, unbind_result.code);
                if (unbind_result.code != kQolErrNotFound &&
                    unbind_result.code != kQolErrNotReady &&
                    unbind_result.code != kQolErrConflict) {
                    g_extension_desc_item_ids[index] = id;
                    g_extension_desc_item_bound[index] = true;
                }
            }
            continue;
        }

        g_extension_desc_item_ids[index] = id;
        g_extension_desc_item_bound[index] = true;
        QOL_LOG_DEBUG(QolDomain::kExtensionBag,
                      "extension desc hook installed index=%d id=%llu button=%p data=%p execute_slot=%p original=%p",
                      static_cast<int>(index), static_cast<unsigned long long>(id),
                      target.button, target.data, target.execute_slot, target.original_proc);
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
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension confirm-use lock busy item=%p", item);
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
        if (character == nullptr || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.use_item == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                        "extension confirm-use unavailable bag=%d slot=%d character=%p",
                        bag, slot, character);
            return false;
        }
        ModuleUseToken use_token{};
        if (!module_use_begin_locked(bag, slot, item, &use_token)) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension confirm-use already active bag=%d slot=%d item=%p",
                        bag, slot, item);
            return false;
        }
        // CHAR_UseItemEx 会同步进入 INVEN_ConsumeItem；不得持有背包锁调用。
        ModuleUseToken* previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        lock.unlock();
        int use_result = 0;
        used = extension_use_item(character, item, 0, &use_result) && use_result != 0;
        lock.lock();
        g_active_module_use_token = previous_use_token;
        bool consumed = false;
        const bool use_token_valid = module_use_finish_locked(use_token, &consumed, &release_plan);
        const bool same_item = g_module_objects[bag][slot] == item;
        if (!use_token_valid) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension confirm-use token lost after use bag=%d slot=%d item=%p marker_cleared=%d; treated handled",
                        bag, slot, item, isolated ? 1 : 0);
            if (isolated) {
                reset_extension_desc_item_hooks_locked();
                g_extension_desc_item = nullptr;
                g_extension_desc_item_bag = -1;
                g_extension_desc_item_slot = -1;
            } else {
                QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension confirm-use abort failed; slot may remain active "
                            "bag=%d slot=%d item=%p",
                            bag, slot, item);
            }
            return true;
        }
        if (!used) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension confirm-use executed failed bag=%d slot=%d", bag, slot);
            return true;
        }
        if (!same_item && !consumed) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension confirm-use identity changed unexpectedly bag=%d slot=%d item=%p",
                        bag, slot, item);
            return true;
        }
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            VIRTBAG_LOG_ERROR("ERROR extension confirm-use native release failed bag=%d slot=%d",
                        bag, slot);
            return true;
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension confirm-use release finalize failed bag=%d slot=%d",
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

// R-63 佣兵徽章（category 42..50 / 928..933）使用按钮函数级接管。
// 原版 UIEquip_ButtonUseMercenarySealExe(0xb8144) 经 ControlItem_GetItem 从 g_inven
// 取物品，扩展物品不进物理袋会取到 null/错位而静默失败。此处按原版流程用模块详情
// 身份直接调原版 MERCENARYSYSTEM_MakeMercenary（0x119658），消耗交给已 Hook 的
// INVEN_RemoveItem（H-04）承接扩展槽删除；SAVE_IsOK/空槽校验顺序与原版一致。
// 返回 true=已接管（原版按钮不再执行）；false=非扩展详情佣兵徽章，交原版透传。
bool extension_use_mercenary_seal() {
    bool used = false;
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        std::unique_lock<std::mutex> lock(g_virtual_bag_mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension mercenary-seal lock busy");
            return false;
        }
        const auto* save_facts = qol::features::extension_bag::save_facts();
        if (!extension_bag_enabled() || g_extension_desc_item == nullptr ||
            g_game == nullptr || g_game->read_only.item.is_mercenary_seal == nullptr ||
            save_facts == nullptr || save_facts->is_ok == nullptr ||
            g_character_extras->read.has_empty_mercenary_slot == nullptr ||
            extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.make_mercenary == nullptr) {
            return false;
        }
        item = g_extension_desc_item;
        bag = g_extension_desc_item_bag;
        slot = g_extension_desc_item_slot;
        if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            g_module_objects[bag][slot] != item) {
            return false;
        }
        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) return false;
        // 用物品自身 flags 的类别（与原版 SetDescMenu 创建 0x98 按钮同源）判定，
        // descriptor.category 仅作回退，避免序列化差异导致误判。
        int category = descriptor.category;
        (void)read_item_category(item, &category);
        if (!read_item_is_mercenary_seal(category)) return false;
    }
    // 原版按钮顺序：UIDesc_SetOff → SAVE_IsOK → IsEmptyManagerSlot → MakeMercenary。
    // 游戏函数一律在 g_virtual_bag_mtx 外调用（MakeMercenary 内部会进入
    // INVEN_RemoveItem → H-04 再次取锁，持锁调用必死锁）。
    extension_description_set_off();
    const auto show_popup = [](uint32_t text_id) {
        if (g_sdk != nullptr && g_sdk->ui != nullptr &&
            g_sdk->ui->show_text_popup != nullptr) {
            (void)g_sdk->ui->show_text_popup(static_cast<int32_t>(text_id));
        }
    };
    if (!read_save_is_ok()) {
        VIRTBAG_LOG("extension mercenary-seal reject reason=save_busy bag=%d slot=%d", bag, slot);
        show_popup(0x25);
        return true;
    }
    if (!read_has_empty_mercenary_slot()) {
        VIRTBAG_LOG("extension mercenary-seal reject reason=slots_full bag=%d slot=%d", bag, slot);
        show_popup(0x17);
        return true;
    }
    ModuleUseToken use_token{};
    bool begin_failed = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_module_objects[bag][slot] != item ||
            !module_use_begin_locked(bag, slot, item, &use_token)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension mercenary-seal begin failed bag=%d slot=%d item=%p", bag, slot,
                        item);
            begin_failed = true;
        }
    }
    // UI 弹窗会进入原版 UI，不在背包锁内创建。
    if (begin_failed) {
        show_popup(0xc);
        return true;
    }
    ModuleUseToken* previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    void* mercenary = nullptr;
    (void)extension_make_mercenary(item, &mercenary);
    g_active_module_use_token = previous_use_token;
    used = mercenary != nullptr;
    bool token_lost = false;
    bool make_failed = false;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension mercenary-seal token lost bag=%d slot=%d item=%p "
                          "marker_cleared=%d; treated handled",
                          bag, slot, item, isolated ? 1 : 0);
            if (isolated) {
                reset_extension_desc_item_hooks_locked();
                g_extension_desc_item = nullptr;
                g_extension_desc_item_bag = -1;
                g_extension_desc_item_slot = -1;
            }
            token_lost = true;
        } else if (!used) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension mercenary-seal make failed bag=%d slot=%d", bag, slot);
            make_failed = true;
        }
    }
    if (token_lost) return true;
    if (make_failed) {
        show_popup(0xc);
        return true;
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            VIRTBAG_LOG_ERROR("ERROR extension mercenary-seal native release failed bag=%d slot=%d", bag,
                        slot);
            return true;
        }
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension mercenary-seal release finalize failed bag=%d slot=%d", bag,
                        slot);
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
    show_popup(0x18);
    VIRTBAG_LOG("extension mercenary-seal handled bag=%d slot=%d used=1", bag, slot);
    return true;
}

void extension_desc_item_execute_at(size_t index, void* button) {
    if (index >= g_extension_desc_item_bound.size() ||
        !g_extension_desc_item_bound[index] || button == nullptr) return;
    const uint64_t id = g_extension_desc_item_ids[index];
    const auto* api = qol::contracts::desc_hook_v2_if_available(g_sdk);
    if (api == nullptr || id == 0 || button == nullptr) return;
    qol::contracts::QolDescButtonTarget current{};
    int32_t matches = 0;
    const QolResult validate_result = api->validate(id, button, &current, &matches);
    if (!qol::contracts::succeeded(validate_result) || matches == 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension desc hook stale index=%d id=%llu button=%p current_button=%p data=%p execute_slot=%p code=%d matches=%d reason=identity_validation_failed",
                     static_cast<int>(index), static_cast<unsigned long long>(id), button,
                     current.button, current.data, current.execute_slot,
                     validate_result.code, matches);
        return;
    }
    const int hook_index = static_cast<int>(index);
    void* original = current.original_proc;
    void* item = nullptr;
    int item_bag = -1;
    int item_slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        item = g_extension_desc_item;
        item_bag = g_extension_desc_item_bag;
        item_slot = g_extension_desc_item_slot;
    }
    VIRTBAG_LOG("extension desc execute index=%d original=%p bag=%d slot=%d",
                hook_index, original, item_bag, item_slot);

    const bool direct_use = extension_desc_original_matches(
        id, "desc.button.use");
    const bool confirm_use = extension_desc_original_matches(
        id, "desc.button.use_after_confirm");
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
        if (leader == nullptr || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.use_item == nullptr || !current_item) {
            // 扩展侧没有完整接管前置条件时，direct/confirm 都必须回退原版按钮，
            // 不能清状态或吞掉原版确认弹窗及其 OK 回调。
            (void)extension_desc_call_original(id, button);
            return;
        }
        if (current_item && leader != nullptr && extension_bag_write_api() != nullptr &&
            extension_bag_write_api()->equipment.use_item != nullptr) {
            if (confirm_use) {
                // IsUseAfterConfirm 类物品（如卓越灵药）：原版按钮会弹确认
                // YesNo，其 OK 回调（UIEquip_OKConfrimUseItem）在调
                // CHAR_UseItemEx 前先 INVEN_FindItemSlot 定位——扩展物品不在
                // INVEN 会被拦截而无效果。确认使用函数入口由常驻 LSPosed
                // Native Hook 接管，按回调传入的物品身份分流；此处只让原版
                // 按钮创建确认弹窗。弹窗创建/关闭不持有背包锁。
                extension_description_set_off();
                (void)extension_desc_call_original(id, button);
                return;
            }
            int use_result = 0;
            used = extension_use_item(leader, item, 0, &use_result) && use_result != 0;
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

    // 装备页出售（0.7 倍率、只能全卖）走原版销毁链：真实 execute proc 可能是
    // UIEquip_ButtonDestroyExe（创建确认弹窗），也可能是其结算入口
    // UIEquip_OKDestroyItem（0xb83d0）。两者都属目标 proc，任一命中即由模块
    // 接管并走 canonical（下方 item-count facade）；其余 proc 行为不变（非目标
    // proc 直通 call_orig）。
    const bool direct_sell = extension_desc_original_matches(
        id, "desc.button.sell");
    const bool modified_destroy_sell =
        extension_desc_original_matches(id, "desc.button.destroy") ||
        extension_desc_original_matches(id, "desc.button.destroy_ok");
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
            if (current_item && g_game != nullptr &&
                g_item_facts != nullptr && g_item_facts->sell_price != nullptr &&
                g_item_facts->is_no_sell != nullptr) {
        // 卖出数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
        count = static_cast<int>(virtual_bag_effective_count(item, static_cast<uint32_t>(g_virtual_bag_state.items[bag][slot].count)));
                int32_t no_sell = 0;
                if (read_item_category(item, &category) &&
                    qol::contracts::succeeded(
                        g_item_facts->is_no_sell(category, &no_sell)) &&
                    no_sell == 0 && count > 0 && read_item_sell_price(item, &unit_price)) {
                    g_extension_sell_apply_variant_discount = modified_destroy_sell;
                    price = extension_bag_sell_price(item, count, modified_destroy_sell);
                    if (price >= 0) {
                        valid = true;
                        const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
                        payload = virtual_bag::payload_hash(descriptor);
                        generation = g_module_object_use[bag][slot].generation;
                        const int legal_count = count;
                        QOL_LOG_INFO(QolDomain::kExtensionBag,
                            "sell price source=equip category=%d count=%d unit=%lld variant=%d final=%lld payload=%u gen=%llu",
                            category, legal_count, static_cast<long long>(unit_price),
                            modified_destroy_sell ? 70 : 100, static_cast<long long>(price), payload,
                            static_cast<unsigned long long>(generation));
                    } else {
                        QOL_LOG_WARN(QolDomain::kExtensionBag,
                            "sell price reject reason=price_bounds source=equip category=%d count=%d unit=%lld variant=%d payload=%u gen=%llu",
                            category, count, static_cast<long long>(unit_price),
                            modified_destroy_sell ? 70 : 100, payload,
                            static_cast<unsigned long long>(g_module_object_use[bag][slot].generation));
                    }
                } else {
                    QOL_LOG_WARN(QolDomain::kExtensionBag,
                        "sell price reject reason=no_sell_or_count source=equip category=%d count=%d variant=%d",
                        category, count, modified_destroy_sell ? 70 : 100);
                }
            } else if (!current_item) {
                QOL_LOG_WARN(QolDomain::kExtensionBag, "sell price reject reason=stale_or_release_guard source=equip bag=%d slot=%d",
                            bag, slot);
            } else {
                QOL_LOG_WARN(QolDomain::kExtensionBag, "sell price reject reason=symbols source=equip bag=%d slot=%d",
                            bag, slot);
            }
            g_extension_sell_original_ok = nullptr;
            g_extension_sell_original_cancel = nullptr;
        }
        const auto* ui_write = g_game == nullptr ? nullptr : &g_write_port->ui;
        bool popup_created = valid && price <= UINT32_MAX && ui_write != nullptr &&
                             ui_write->create_confirmation != nullptr;
        if (popup_created) {
            // 原版按钮先关闭详情再创建确认弹窗；保持同样的 UI 生命周期，且不持有背包锁调用游戏函数。
            extension_description_set_off();
            const QolResult result = ui_write->create_confirmation(
                modified_destroy_sell ? 1 : 0, static_cast<uint32_t>(price),
                reinterpret_cast<void*>(&extension_sell_ok),
                reinterpret_cast<void*>(&extension_sell_cancel),
                reinterpret_cast<void**>(&g_extension_sell_original_ok),
                reinterpret_cast<void**>(&g_extension_sell_original_cancel));
            if (!qol::contracts::succeeded(result)) {
                VIRTBAG_LOG("extension sell confirmation unavailable code=%d", result.code);
                g_extension_sell_original_ok = nullptr;
                g_extension_sell_original_cancel = nullptr;
                popup_created = false;
            }
            if (popup_created) {
                VIRTBAG_LOG("extension sell confirmation created price=%lld", static_cast<long long>(price));
            }
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
    // 未命中任何目标 proc 时回退原版；item 为分流块锁内捕获的本地副本。
    if (item != nullptr) {
        VIRTBAG_LOG("extension desc execute unmatched index=%d original=%p bag=%d slot=%d",
                    hook_index, original, item_bag, item_slot);
    }
    (void)extension_desc_call_original(id, button);

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
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension desc operation reserialize failed bag=%d slot=%d", item_bag,
                    item_slot);
        reset_extension_desc_item_hooks_locked();
        return;
    }
    virtual_bag::Item& descriptor = g_virtual_bag_state.items[item_bag][item_slot];
    const virtual_bag::Item previous_descriptor = descriptor;
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    (void)read_item_category(item, &descriptor.category);
    // descriptor.count 恒为 canonical（R-38/R-48）：getter 在关闭态只返回 b 视图
    //（R-47 决策 b），物化/重同步必须保存完整 128a+b。
    if (read_item_cumulate_count(item, &descriptor.count)) {
        descriptor.count = static_cast<int>(virtual_bag_canonical_count(item, 1));
    }
    if (descriptor.category <= 0 || descriptor.count <= 0) {
        descriptor = {};
        if (!free_module_object_locked(item_bag, item_slot)) {
            descriptor = previous_descriptor;
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension desc sync rejected object release bag=%d slot=%d",
                        item_bag, item_slot);
            reset_extension_desc_item_hooks_locked();
            return;
        }
    } else {
        g_module_object_categories[item_bag][item_slot] = descriptor.category;
        g_module_object_hashes[item_bag][item_slot] = virtual_bag::payload_hash(descriptor);
    }
    extension_bag_mark_dirty_locked();
    reset_extension_desc_item_hooks_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension desc operation synced bag=%d slot=%d category=%d count=%d",
                item_bag, item_slot, descriptor.category, descriptor.count);
}

// BL 门禁（0xb9188）：物品 desc 打开 → 调原 MakeDesc（同步生成含装备按钮的
// 菜单）→ 锁内重装装备按钮 hook。物品从触发控件 data[0] 取（MakeDesc 同源）。
void make_desc_equip_gate(void* ctrl, void* arg) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"patch:P_EXT_DESC_OPEN_GATE"});
    if (ctrl == nullptr || g_game == nullptr ||
        g_write_port->ui.make_item_description == nullptr) return;
    // UIEquip_RefreshItemArea may have restored the original inventory object
    // immediately before the original event reaches MakeDesc. Rebind only
    // projected controls so the original MakeDesc reads the extension item.
    virtual_bag_sync_projected_item_control(ctrl);
    void* item = nullptr;
    {
        void* data = extension_bag_ui_data(ctrl);
        if (data != nullptr) item = *reinterpret_cast<void**>(data);
    }
    (void)arg;
    (void)g_write_port->ui.make_item_description(ctrl);
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
                // 装备/卸下按钮由函数级 hook
                // （ButtonEquipExe 第 10）承担，观察 hook 只覆盖其他详情按钮。
                install_extension_desc_item_hooks_locked(item);
                QOL_LOG_INFO(QolDomain::kExtensionBag, "extension desc item captured bag=%d slot=%d item=%p", item_bag,
                            item_slot, item);
            }
        }
    }
}

// ---- Path 卸下按钮：函数级 hook ----
// 卸下按钮 execute proc 恒为 UIEquip_ButtonUnequipExe（0xb7e14），扩展语义由
// native 第 11 hook（button_unequip_exe_wrapper →
// virtual_bag_handle_original_bag_unequip）承担：扩展视图内识别扩展袋 desc
// 解除扩展袋；原版视图解除原版袋并在背包满时放回其他袋行/收编扩展空位。
// g_extension_desc_unequip_bag 仍由扩展袋 desc 打开时记录（供函数 hook 识别），
// 详情记录仅供函数级入口 hook 识别。

// virtual_bag_handle_backpack_button_equip 定义在 game_ui_virtbag.cpp 的
// 匿名 namespace 之外（本文件整体位于匿名 namespace 内，internal linkage
// 无法被 extension_bag_port.cpp 跨 TU 调用），依赖的本 TU 函数在匿名
// namespace 内仍可见。
