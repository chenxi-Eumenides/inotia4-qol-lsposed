// move_merge / extension source protection（D2c-2b-2 由 feature/patch/game_patch_move_merge.inc
// 迁入 EB；wrapper 业务体与自门控语义逐字）。include 于 plugins/extension_bag/game_ui_virtbag.cpp
// 末尾（claims 之前），全局标志 g_move_merge_mtx / g_move_merge_requested /
// g_extension_source_protection_requested 定义在该 TU 首部匿名命名空间。

#define MOVE_LOG(...) QOL_LOG_INFO(QolDomain::kPlatform, __VA_ARGS__)

uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const uint64_t observation_token = virtual_bag_observe_item_proc_pre(control, event, x2, param);
    const auto finish = [observation_token, control, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, control, event, x2, param, result);
        return result;
    };
    // G-8：投影拖动 drop 到扩展格（0x02）→ 路由 ext→ext 事务，抑制原版
    // INVEN_MoveItem（借出对象不在 INVEN，原版移动不可靠）。
    if (event == 0x2 && param != nullptr) {
        void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param, ctrl_src);
        const VirtualBagProjectionDropResult drop_result =
            virtual_bag_projection_drop_to_slot(control, ctrl_src);
        if (drop_result == VirtualBagProjectionDropResult::kHandled) {
            return finish(1);
        }
        if (drop_result == VirtualBagProjectionDropResult::kRejected) {
            QOL_LOG_DEBUG(QolDomain::kPlatform,
                          "move_merge: extension drop rejected; original proc suppressed");
            return finish(0);
        }
    }
    // G-7：扩展视图内只拦截 drop（0x04，借出对象不得拖入原版袋——SaveItemOnEmpty
    // 门禁为第二层）；详情(0x80)/选中(0x01,0x02)/其余事件放行原版控件链。
    if (virtual_bag_original_item_input_blocked() && event == 0x04) {
        if (param != nullptr) {
            void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
            virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param,
                                                 ctrl_src);
            const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            const uint64_t caller_offset =
                g_base != 0 && caller >= g_base ? static_cast<uint64_t>(caller - g_base) : 0;
            if (virtual_bag_allow_original_tab_drop(control, ctrl_src, caller_offset)) {
                const uint64_t result = fn_ui_equip_inven_item_control_event_proc(
                    control, event, x2, param);
                return finish(result);
            }
        }
        // G-9 防护：扩展标签挂物品列表 root（索引 16+），GetItemSlotIndex 对其返回
        // 越界值 16——drop 落到标签控件时直接拒绝，防越界写。
        if (fn_ui_equip_get_item_slot_index != nullptr &&
            fn_ui_equip_get_item_slot_index(control) >= 16) {
            QOL_LOG_DEBUG(QolDomain::kPlatform,
                          "drop suppressed: dst is extension tab control=%p", control);
            return finish(0);
        }
        QOL_LOG_DEBUG(QolDomain::kPlatform,
                      "move_merge: drop suppressed in extension view control=%p",
                      control);
        return finish(0);
    }
    if (g_move_merge_requested.load() && event == 4 && param != nullptr &&
        fn_control_object_get_data != nullptr &&
        fn_ui_equip_is_apply_stuff != nullptr && fn_ui_equip_get_item_slot_index != nullptr &&
                 fn_get_cumulate_count != nullptr && fn_get_bit != nullptr && fn_inven_move_item != nullptr) {
        void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param, ctrl_src);
        void* src_data = ctrl_src != nullptr ? fn_control_object_get_data(ctrl_src) : nullptr;
        void* dst_data = fn_control_object_get_data(control);
        void* item_a = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
        void* item_b = dst_data != nullptr ? *reinterpret_cast<void**>(dst_data) : nullptr;
        if (item_a != nullptr && item_b != nullptr &&
            !fn_ui_equip_is_apply_stuff(item_b, item_a) &&
            item_count_encoding(item_a) == ::qol::contracts::QolItemCountEncoding::kEncoded &&
            item_count_encoding(item_b) == ::qol::contracts::QolItemCountEncoding::kEncoded) {
            const uint16_t flags_a =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item_a) + I_TYPE);
            const uint16_t flags_b =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item_b) + I_TYPE);
            if (fn_get_bit(flags_a, 15, 6) == fn_get_bit(flags_b, 15, 6)) {
                const int bag = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
                const int dst_slot = fn_ui_equip_get_item_slot_index(control);
                const int src_slot = ctrl_src != nullptr ? fn_ui_equip_get_item_slot_index(ctrl_src) : -1;
                const int count = fn_get_cumulate_count(item_a);
                const int target_count = fn_get_cumulate_count(item_b);
                const bool extended = virtual_bag_extended_mode(item_b);
                 const uint32_t stack_limit = virtual_bag_count_limit(extended);
                const bool valid_slots = bag >= 0 && bag < 6 && src_slot >= 0 && src_slot < 16 &&
                                         dst_slot >= 0 && dst_slot < 16;
                const bool has_merge_room = target_count > 0 && count > 0 &&
                                            static_cast<uint64_t>(target_count) < stack_limit;
                if (valid_slots && has_merge_room) {
                    const int result = fn_inven_move_item(item_a, count, bag, dst_slot);
                    if (result) {
                        if (virtual_bag_module_view_installed()) {
                            virtual_bag_sync_projected_slot(bag, src_slot);
                            virtual_bag_sync_projected_slot(bag, dst_slot);
                        }
                        // D2c-2b-1：原 `if (fn_ui_equip_refresh_item_area != nullptr) 直呼
                        // refresh trampoline` 改经 inventory_refresh capability 现取；能力缺失
                        // （provider 未安装/撤销中）保持既有 no-op 语义（不直呼符号入口，避免
                        // 经 hook 回入 wrapper）。
                        const void* refresh_iface = nullptr;
                        const QolResult refresh_result =
                            g_sdk != nullptr && g_sdk->capabilities.require != nullptr
                                ? g_sdk->capabilities.require(
                                      ::qol::contracts::kInventoryRefreshCapabilityId,
                                      QolVersionRange{
                                          static_cast<uint16_t>(
                                              ::qol::contracts::kInventoryRefreshVersionMajor),
                                          static_cast<uint16_t>(
                                              ::qol::contracts::kInventoryRefreshVersionMinor)},
                                      &refresh_iface)
                                : QolResult{1, kQolErrNotReady, nullptr, 0};
                        const auto* refresh = refresh_result.ok == kQolOk
                            ? static_cast<const ::qol::contracts::InventoryRefreshApi*>(
                                  refresh_iface)
                            : nullptr;
                        if (refresh != nullptr && refresh->call_original != nullptr) {
                            refresh->call_original();
                        }
                        void* panel_ctrl = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
                        if (fn_touch_handle_set_cursor != nullptr && panel_ctrl != nullptr) {
                            fn_touch_handle_set_cursor(panel_ctrl, nullptr);
                        }
                        QOL_LOG_DEBUG(QolDomain::kPlatform,
                                      "move_merge: merged bag=%d src=%d dst=%d count=%d", bag,
                                      src_slot, dst_slot, count);
                        return finish(0);
                    }
                    int extension_source_bag = -1;
                    int extension_source_slot = -1;
                    if (virtual_bag_identify_native_item(item_a, &extension_source_bag,
                                                         &extension_source_slot)) {
                        QOL_LOG_DEBUG(QolDomain::kPlatform,
                                      "move_merge: extension guard rejected; original proc suppressed "
                                      "item=%p source=%d/%d target=%d/%d",
                                      item_a, extension_source_bag, extension_source_slot, bag,
                                      dst_slot);
                        return finish(0);
                    }
                    QOL_LOG_DEBUG(QolDomain::kPlatform,
                                  "move_merge: native merge rejected; falling through bag=%d src=%d dst=%d",
                                  bag, src_slot, dst_slot);
                } else {
                    QOL_LOG_DEBUG(QolDomain::kPlatform,
                                  "move_merge: invalid/full target; falling through bag=%d src=%d dst=%d "
                                  "source_count=%d target_count=%d limit=%u",
                                  bag, src_slot, dst_slot, count, target_count,
                                  static_cast<unsigned int>(stack_limit));
                }
            }
        }
    }
    if (fn_ui_equip_inven_item_control_event_proc == nullptr) return finish(0);
    const uint64_t result = fn_ui_equip_inven_item_control_event_proc(control, event, x2, param);
    if (virtual_bag_module_view_installed()) {
        virtual_bag_sync_projected_bag();
    }
    return finish(result);
}

bool set_move_merge_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_move_merge_mtx);
    g_move_merge_requested.store(enabled);
    MOVE_LOG("move_merge: behavior=%d extension_protection=%d (wrapper resident)",
             enabled ? 1 : 0, g_extension_source_protection_requested.load() ? 1 : 0);
    return true;
}

bool set_extension_source_protection_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_move_merge_mtx);
    g_extension_source_protection_requested.store(enabled);
    MOVE_LOG("move_merge: extension_source_protection=%d merge=%d (wrapper resident)",
             enabled ? 1 : 0, g_move_merge_requested.load() ? 1 : 0);
    return true;
}

bool move_merge_enabled() {
    std::lock_guard<std::mutex> lock(g_move_merge_mtx);
    return g_move_merge_requested.load();
}

#undef MOVE_LOG
