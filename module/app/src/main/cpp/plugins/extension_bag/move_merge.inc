// move_merge / extension source protection（D2c-2b-2 由 feature/patch/game_patch_move_merge.inc
// 迁入 EB；wrapper 业务体与自门控语义逐字）。include 于 plugins/extension_bag/game_ui_virtbag.cpp
// 末尾（claims 之前），全局标志 g_move_merge_mtx / g_move_merge_requested /
// g_extension_source_protection_requested 定义在该 TU 首部匿名命名空间。

#define MOVE_LOG(...) QOL_LOG_INFO(QolDomain::kPlatform, __VA_ARGS__)

bool move_merge_control_slot(void* control, int32_t* out_slot) {
    return g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->control_item_slot != nullptr &&
           out_slot != nullptr &&
           qol::contracts::succeeded(g_sdk->ui->control_item_slot(control, out_slot));
}

bool move_merge_can_apply_stuff(void* target, void* material) {
    int32_t allowed = 0;
    return g_game != nullptr && g_character_extras->read.can_apply_stuff != nullptr &&
           qol::contracts::succeeded(
               g_character_extras->read.can_apply_stuff(target, material, &allowed)) &&
           allowed != 0;
}

bool move_merge_set_cursor(void* control, void* cursor) {
    return g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->set_touch_cursor != nullptr &&
           qol::contracts::succeeded(g_sdk->ui->set_touch_cursor(control, cursor));
}

uint64_t move_merge_call_original(void* control, uint64_t event, void* x2, void* param) {
    uint64_t result = 0;
    if (g_sdk == nullptr || g_sdk->hooks.call_original_control_proc == nullptr ||
        !qol::contracts::succeeded(g_sdk->hooks.call_original_control_proc(
            control, event, x2, param, &result))) {
        return 0;
    }
    return result;
}

uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const ::qol::contracts::InFlightGuard in_flight_guard(
        g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
        {"ptrslot:MOVE_MERGE_GOT"});
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
            if (virtual_bag_allow_original_tab_drop(control, ctrl_src, 0)) {
                const uint64_t result = move_merge_call_original(control, event, x2, param);
                return finish(result);
            }
        }
        // G-9 防护：扩展标签挂物品列表 root（索引 16+），GetItemSlotIndex 对其返回
        // 越界值 16——drop 落到标签控件时直接拒绝，防越界写。
        int32_t control_slot = -1;
        if (move_merge_control_slot(control, &control_slot) && control_slot >= 16) {
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
        g_item_facts != nullptr && g_item_facts->cumulate_count != nullptr &&
        g_item_facts->category != nullptr && extension_bag_write_api() != nullptr &&
        extension_bag_write_api()->inventory.move_item != nullptr &&
        g_character_extras->read.can_apply_stuff != nullptr &&
        g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->control_item_slot != nullptr) {
        void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param, ctrl_src);
        void* src_data = ctrl_src != nullptr ? extension_bag_ui_data(ctrl_src) : nullptr;
        void* dst_data = extension_bag_ui_data(control);
        void* item_a = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
        void* item_b = dst_data != nullptr ? *reinterpret_cast<void**>(dst_data) : nullptr;
        if (item_a != nullptr && item_b != nullptr &&
            !move_merge_can_apply_stuff(item_b, item_a) &&
            item_count_encoding(item_a) == ::qol::contracts::QolItemCountEncoding::kEncoded &&
            item_count_encoding(item_b) == ::qol::contracts::QolItemCountEncoding::kEncoded) {
            int category_a = -1;
            int category_b = -1;
            if (read_item_category(item_a, &category_a) &&
                read_item_category(item_b, &category_b) && category_a == category_b) {
                int32_t bag = -1;
                if (g_game == nullptr || g_write_port->ui.current_bag == nullptr ||
                    !qol::contracts::succeeded(
                        g_write_port->ui.current_bag(&bag))) {
                    return finish(0);
                }
                int32_t dst_slot = -1;
                int32_t src_slot = -1;
                (void)move_merge_control_slot(control, &dst_slot);
                if (ctrl_src != nullptr) (void)move_merge_control_slot(ctrl_src, &src_slot);
                int count = 0;
                int target_count = 0;
                if (!read_item_cumulate_count(item_a, &count) ||
                    !read_item_cumulate_count(item_b, &target_count)) {
                    return finish(0);
                }
                const bool extended = virtual_bag_extended_mode(item_b);
                 const uint32_t stack_limit = virtual_bag_count_limit(extended);
                const bool valid_slots = bag >= 0 && bag < 6 && src_slot >= 0 && src_slot < 16 &&
                                         dst_slot >= 0 && dst_slot < 16;
                const bool has_merge_room = target_count > 0 && count > 0 &&
                                            static_cast<uint64_t>(target_count) < stack_limit;
                if (valid_slots && has_merge_room) {
                    int result = 0;
                    const bool move_called = extension_move_item(item_a, count, bag, dst_slot, &result);
                    if (move_called && result) {
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
                        void* panel_ctrl = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
                        if (panel_ctrl != nullptr) {
                            (void)move_merge_set_cursor(panel_ctrl, nullptr);
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
    const uint64_t result = move_merge_call_original(control, event, x2, param);
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
