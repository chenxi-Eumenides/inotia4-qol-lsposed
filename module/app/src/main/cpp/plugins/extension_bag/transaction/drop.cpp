#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/assembly/move_merge.h"

#include <algorithm>
#include <cstring>

#define is_backpack_category extension_bag_runtime_is_backpack_category

bool route_deferred_extension_to_original_locked(std::unique_lock<std::mutex>& lock) {
    virtual_bag::ExtensionDragSession& session = g_extension_drag_session;
    if (session.target_kind != virtual_bag::DragTargetKind::kOriginalBag ||
        !virtual_bag::valid_original_transaction_bag(session.target_bag) ||
        !virtual_bag::drag_session_generation_current(session, g_extension_tab_generation) ||
        !virtual_bag::valid_index(session.source_bag) ||
        session.source_slot < 0 || session.source_slot >= virtual_bag::kSlotCount) {
        return false;
    }
    if (!virtual_bag::drag_session_source_identity_matches(
            session, session.source_bag, session.source_slot,
            g_module_object_handles[session.source_bag][session.source_slot])) {
        cancel_projected_drag_session_locked("source-identity-replaced");
        return false;
    }
    if (session.phase == virtual_bag::DragPhase::kPressed) {
        virtual_bag::session_on_native_moving(&session, g_extension_tab_generation);
    }
    if (virtual_bag::session_resolve_target(&session, g_extension_tab_generation,
                                            virtual_bag::DragTargetKind::kOriginalBag) ==
            virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_begin_transaction(&session, g_extension_tab_generation) ==
            virtual_bag::DragTransition::kIllegal ||
        !virtual_bag::session_claim_transaction(&session)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "drop deferred ext->orig rejected source=%d/%d target_bag=%d phase=%d",
                    session.source_bag, session.source_slot, session.target_bag,
                    static_cast<int>(session.phase));
        return false;
    }
    const int src_bag = session.source_bag;
    const int src_slot = session.source_slot;
    const int target_bag = session.target_bag;
    const bool moved = move_extension_to_original_locked(lock, src_bag, src_slot, target_bag);
    virtual_bag::session_finish_transaction(&session, moved);
    if (moved) {
        persist_state_locked();
        refresh_projected_module_view_after_move_locked();
        QOL_LOG_INFO(QolDomain::kExtensionBag, "p5 session drop committed source=%d/%d target=%d/%d kind=%d deferred=1",
                    src_bag, src_slot, target_bag, -1,
                    static_cast<int>(session.target_kind));
    } else {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "p5 session drop rejected source=%d/%d target=%d/%d kind=%d deferred=1",
                    src_bag, src_slot, target_bag, -1,
                    static_cast<int>(session.target_kind));
    }
    return moved;
}
bool handle_bag_drop_release_locked(std::unique_lock<std::mutex>& lock, int64_t x, int64_t y) {
    ensure_state_loaded_locked();
    recover_pending_transaction_locked(lock);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "drop release mode=%d selected=%d x=%lld y=%lld moving=%p extension_drag=%d",
                static_cast<int>(g_virtual_bag_state.mode), g_virtual_bag_state.selected,
                static_cast<long long>(x), static_cast<long long>(y),
                touch_moving_item_control_locked(), g_extension_drag.active ? 1 : 0);
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kOriginal) {
        void* moving_control = touch_moving_item_control_locked();
        if (moving_control == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "drop reject original->extension no moving original control");
            return false;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (!extension_tab_hit(index, x, y)) continue;
            void* item = nullptr;
            {
                void* data = extension_bag_ui_data(moving_control);
                item = data != nullptr ? *reinterpret_cast<void**>(data) : nullptr;
            }
            const int category = original_item_category_locked(item);
            if (g_virtual_bag_state.types[index] == 0 && is_backpack_category(category)) {
                // 原版背包来源的袋物品拖到空扩展标签也必须走“袋槽装备”，
                // 不能直接落入普通扩展物品槽。
                return try_equip_on_extension_tab_drop_locked(lock, index, moving_control);
            }
            // 目标非空，或来源为装备/其他物品：保持原版物品的普通移动语义。
            return move_original_to_extension_locked(lock, index, moving_control);
        }
        QOL_LOG_WARN(QolDomain::kExtensionBag, "drop reject original->extension no extension tab hit x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
        return false;
    }
    if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule && g_extension_drag.active) {
    const int target_slot = extension_bag_grid_slot_index(x, y, kGridX, kGridY);
            if (target_slot >= 0) {
            const bool handled = move_extension_to_extension_locked(
                g_extension_drag.bag, g_extension_drag.slot,
                g_virtual_bag_state.selected, target_slot);
            if (handled) refresh_projected_module_view_after_move_locked();
            QOL_LOG_INFO(QolDomain::kExtensionBag, "drop result direction=extension->extension handled=%d src=%d/%d dst=%d/%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot,
                        g_virtual_bag_state.selected, target_slot);
            return handled;
        }
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (index == g_extension_drag.bag) continue;
            if (!extension_tab_hit(index, x, y)) continue;
            const int category = g_virtual_bag_state.items[g_extension_drag.bag][g_extension_drag.slot].category;
            const bool should_equip = g_virtual_bag_state.types[index] == 0 &&
                                      is_backpack_category(category);
            const bool handled = should_equip
                ? try_equip_on_extension_tab_drop_locked(
                      lock, index, touch_moving_item_control_locked())
                : move_extension_to_extension_locked(
                    g_extension_drag.bag, g_extension_drag.slot, index, -1);
            if (handled) refresh_projected_module_view_after_move_locked();
            QOL_LOG_INFO(QolDomain::kExtensionBag, "drop result direction=extension->tab handled=%d equip=%d src=%d/%d dst_bag=%d",
                        handled ? 1 : 0, should_equip ? 1 : 0, g_extension_drag.bag,
                        g_extension_drag.slot, index);
            return handled;
        }
        const int target_bag = original_bag_button_index(x, y);
        if (target_bag >= 0 && target_bag < 6) {
            if (!virtual_bag::valid_original_transaction_bag(target_bag)) {
                // 索引 5 = 原版任务袋（ADR-006）：不得成为扩展移动目标（触摸路径与 API 门禁对齐）。
                QOL_LOG_WARN(QolDomain::kExtensionBag, "drop reject extension->original reason=invalid_transaction_domain target_bag=%d",
                            target_bag);
                return false;
            }
            const bool handled = move_extension_to_original_locked(
                lock, g_extension_drag.bag, g_extension_drag.slot, target_bag);
            if (handled) refresh_projected_module_view_after_move_locked();
            QOL_LOG_INFO(QolDomain::kExtensionBag, "drop result direction=extension->original handled=%d src=%d/%d target_bag=%d",
                        handled ? 1 : 0, g_extension_drag.bag, g_extension_drag.slot, target_bag);
            return handled;
        }
        QOL_LOG_WARN(QolDomain::kExtensionBag, "drop reject extension destination not found x=%lld y=%lld",
                    static_cast<long long>(x), static_cast<long long>(y));
    } else if (g_virtual_bag_state.mode == virtual_bag::Mode::kModule) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "drop reject extension no active drag selected=%d",
                    g_virtual_bag_state.selected);
    }
    return false;
}

bool route_projected_session_drop_locked(std::unique_lock<std::mutex>& lock, int64_t x, int64_t y) {
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kIdle ||
        g_virtual_bag_state.mode != virtual_bag::Mode::kModule ||
        !virtual_bag::valid_index(g_virtual_bag_state.selected)) {
        return false;
    }
    const auto& source_session = g_extension_drag_session;
    if (!virtual_bag::valid_index(source_session.source_bag) ||
        source_session.source_slot < 0 || source_session.source_slot >= virtual_bag::kSlotCount ||
        !virtual_bag::drag_session_source_identity_matches(
            source_session, source_session.source_bag, source_session.source_slot,
            g_module_object_handles[source_session.source_bag][source_session.source_slot]) ||
        g_module_objects[source_session.source_bag][source_session.source_slot] == nullptr) {
        cancel_projected_drag_session_locked("source-identity-replaced");
        return false;
    }
    // Tab rects and grid rows overlap in the current UI coordinate space; a tab hit is
    // therefore the explicit switch-bag intent and must take precedence over a slot hit.
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        const int target_bag = index;
        const int target_slot = -1;
        if (target_bag == g_extension_drag_session.source_bag ||
            !extension_tab_hit(target_bag, x, y)) {
            continue;
        }
        const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
            target_bag, target_slot, true);
        if (target_kind == virtual_bag::DragTargetKind::kExtensionTab) {
            return route_projected_session_to_tab_locked(
                lock, virtual_bag::extension_logical_bag(target_bag));
        }
    }
    int target_bag = g_virtual_bag_state.selected;
    int target_slot = extension_bag_grid_slot_index(x, y, kGridX, kGridY);
    virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
        virtual_bag::kExtensionLogicalBagFirst + target_bag, target_slot);
    if (target_slot < 0 || target_slot >= g_virtual_bag_state.capacities[target_bag]) {
        target_bag = -1;
        for (int index = 0; index < virtual_bag::kBagCount; ++index) {
            if (index == g_extension_drag_session.source_bag) continue;
            if (!extension_tab_hit(index, x, y)) continue;
            target_bag = index;
            break;
        }
        if (target_bag < 0) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "p5 session target miss x=%lld y=%lld source=%d/%d",
                        static_cast<long long>(x), static_cast<long long>(y),
                        g_extension_drag_session.source_bag,
                        g_extension_drag_session.source_slot);
            return false;
        }
        target_slot = -1;
        target_kind = virtual_bag::classify_drag_target(target_bag, target_slot, true);
    }
    if (g_extension_drag_session.phase == virtual_bag::DragPhase::kPressed) {
        advance_projected_drag_session_locked();
    }
    if (virtual_bag::session_resolve_target(&g_extension_drag_session,
                                            g_extension_tab_generation,
                                            target_kind) ==
            virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_begin_transaction(&g_extension_drag_session,
                                               g_extension_tab_generation) ==
             virtual_bag::DragTransition::kIllegal ||
        virtual_bag::session_on_release(&g_extension_drag_session,
                                        g_extension_tab_generation) !=
            virtual_bag::DragTransition::kAdvanced ||
        !virtual_bag::session_claim_transaction(&g_extension_drag_session)) {
        return false;
    }
    const bool should_equip = target_kind == virtual_bag::DragTargetKind::kExtensionTab &&
                              extension_source_should_equip_locked(target_bag);
    bool handled = false;
    const int source_bag = g_extension_drag_session.source_bag;
    const int source_slot = g_extension_drag_session.source_slot;
    void* material = virtual_bag::valid_index(source_bag) && source_slot >= 0 &&
        source_slot < virtual_bag::kSlotCount
        ? g_module_objects[source_bag][source_slot] : nullptr;
    void* target_item = target_slot >= 0 && target_slot < g_virtual_bag_state.capacities[target_bag]
        ? g_module_objects[target_bag][target_slot] : nullptr;
    const int source_category = virtual_bag::valid_index(source_bag) && source_slot >= 0 &&
        source_slot < virtual_bag::kSlotCount
        ? g_virtual_bag_state.items[source_bag][source_slot].category : 0;
    const bool source_is_apply_material =
        read_item_is_jewel(source_category) || read_item_is_enchant_scroll(source_category);
    // S-03 apply 准入（R-50）：同袋 + 扩展宝石/强化卷轴源 + 目标为装备类 +
    // IsApplyStuff 判真。目标类别用 item_is_equip 判定（数据不可用按非装备
    // fail-closed），普通物品目标不得进入 apply；不设提前 return，统一落到
    // 下方事务分支（交换/移动）并保证 session_finish_transaction 必然执行。
    int32_t apply_stuff_value = 0;
    const bool apply_stuff_allowed =
        g_game != nullptr && g_character_extras->read.can_apply_stuff != nullptr &&
        material != nullptr && target_item != nullptr &&
        qol::contracts::succeeded(g_character_extras->read.can_apply_stuff(
            target_item, material, &apply_stuff_value)) && apply_stuff_value != 0;
    const bool apply_candidate = extension_bag_is_apply_candidate(
        source_bag == target_bag && target_slot >= 0, source_is_apply_material,
        target_item != nullptr && item_is_equip(target_item), apply_stuff_allowed);
    if (apply_candidate) {
        handled = apply_extension_material_to_slot_locked(
            lock, source_bag, source_slot, target_bag, target_slot, material, target_item);
    } else if (should_equip) {
        handled = equip_extension_source_on_tab_locked(
            lock, target_bag, source_bag, source_slot);
    } else {
        handled = move_extension_to_extension_locked(
            source_bag, source_slot,
            target_bag, target_slot);
        if (handled) {
            persist_state_locked();
            refresh_projected_module_view_after_move_locked();
        }
    }
    virtual_bag::session_finish_transaction(&g_extension_drag_session, handled);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "p5 session drop %s source=%d/%d target=%d/%d kind=%d equip=%d",
                handled ? "committed" : "rejected",
                g_extension_drag_session.source_bag, g_extension_drag_session.source_slot,
                target_bag, target_slot, static_cast<int>(target_kind), should_equip ? 1 : 0);
    return handled;
}

bool route_projected_session_to_tab_locked(std::unique_lock<std::mutex>& lock, int target_bag) {
    // 入口 target_bag 使用扩展逻辑袋号 6..10；事务状态和 State 数组统一使用
    // 内部索引 0..4。成功装备由 finish_extension_equip_ui_locked
    // 复用 handle_extension_tab_click_locked/install_module_view_locked 同步当前视图，
    // 不能只结束 session 后继续沿用源袋的 g_module_view_index。
    auto& session = g_extension_drag_session;
    const int target_internal = virtual_bag::extension_internal_bag(target_bag);
    const auto reject = [&](const char* reason) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "cross tab reject reason=%s bag=%d slot=%d target_bag=%d target_internal=%d "
                    "phase=%d generation=%llu/%llu/%llu",
                    reason, session.source_bag, session.source_slot, target_bag, target_internal,
                    static_cast<int>(session.phase),
                    static_cast<unsigned long long>(session.view_generation),
                    static_cast<unsigned long long>(g_extension_tab_generation),
                    static_cast<unsigned long long>(g_inventory_generation));
        return false;
    };
    if (session.phase == virtual_bag::DragPhase::kIdle) {
        return reject("session_idle");
    }
    if (g_virtual_bag_state.mode != virtual_bag::Mode::kModule) {
        return reject("mode_not_module");
    }
    if (target_internal < 0) {
        return reject("target_bag_invalid");
    }
    if (target_internal == session.source_bag) {
        return reject("same_source_target");
    }
    if (!virtual_bag::drag_session_generation_current(session, g_extension_tab_generation)) {
        return reject("session_generation_mismatch");
    }
    if (g_extension_tab_generation != g_inventory_generation) {
        return reject("tab_generation_mismatch");
    }
    if (!virtual_bag::valid_index(session.source_bag) || session.source_slot < 0 ||
        session.source_slot >= virtual_bag::kSlotCount) {
        return reject("source_session_invalid");
    }
    if (!virtual_bag::drag_session_source_identity_matches(
            session, session.source_bag, session.source_slot,
            g_module_object_handles[session.source_bag][session.source_slot]) ||
        g_module_objects[session.source_bag][session.source_slot] == nullptr) {
        return reject("source_identity_replaced");
    }
    if (session.phase == virtual_bag::DragPhase::kPressed) {
        advance_projected_drag_session_locked();
    }
    const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
        target_bag, -1, true);
    if (target_kind != virtual_bag::DragTargetKind::kExtensionTab) {
        return reject("target_kind_invalid");
    }
    if (virtual_bag::session_resolve_target(
            &session, g_extension_tab_generation, target_kind) ==
        virtual_bag::DragTransition::kIllegal) {
        return reject("target_transition_illegal");
    }
    if (session.target_kind != target_kind) {
        return reject("session_target_kind_mismatch");
    }
    if (virtual_bag::session_begin_transaction(&session, g_extension_tab_generation) ==
        virtual_bag::DragTransition::kIllegal) {
        return reject("transaction_begin_illegal");
    }
    if (virtual_bag::session_on_release(&session, g_extension_tab_generation) !=
        virtual_bag::DragTransition::kAdvanced) {
        return reject("release_transition_illegal");
    }
    if (!virtual_bag::session_claim_transaction(&session)) {
        return reject("transaction_claim_failed");
    }
    const bool should_equip = extension_source_should_equip_locked(target_internal);
    bool handled = false;
    if (should_equip) {
        handled = equip_extension_source_on_tab_locked(
            lock, target_internal, session.source_bag, session.source_slot);
    } else {
        handled = move_extension_to_extension_locked(
            session.source_bag, session.source_slot, target_internal, -1);
        if (handled) {
            persist_state_locked();
            refresh_projected_module_view_after_move_locked();
        }
    }
    virtual_bag::session_finish_transaction(&session, handled);
    if (handled) {
        QOL_LOG_INFO(QolDomain::kExtensionBag, "tab commit handled=1 bag=%d slot=%d target_bag=%d target_kind=%d equip=%d",
                    session.source_bag, session.source_slot, target_bag,
                    static_cast<int>(target_kind), should_equip ? 1 : 0);
    } else {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "tab reject reason=transaction_failed bag=%d slot=%d target_bag=%d "
                    "target_kind=%d equip=%d",
                    session.source_bag, session.source_slot, target_bag,
                    static_cast<int>(target_kind), should_equip ? 1 : 0);
    }
    return handled;
}

#undef is_backpack_category
