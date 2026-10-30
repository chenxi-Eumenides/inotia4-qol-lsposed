#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/observation.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/core/drop_routing.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "contracts/inventory_refresh.h"

#include <array>
#include <cstring>

PhysicalInventorySnapshot capture_physical_inventory_snapshot_locked() {
    PhysicalInventorySnapshot snapshot{};
    snapshot.digest = 1469598103934665603ULL;
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        void* item = nullptr;
        qol::contracts::QolItemHandle handle{};
        if (runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->physical_item_at == nullptr ||
            !qol::contracts::succeeded(runtime_state().host.item_facts->physical_item_at(
                static_cast<int32_t>(index / virtual_bag::kSlotCount),
                static_cast<int32_t>(index % virtual_bag::kSlotCount), &handle)) ||
            (handle.slot != 0 && !extension_bag_private_item_for_handle(handle, &item))) {
            return snapshot;
        }
        snapshot.slots[index] = item;
        if (item != nullptr) ++snapshot.nonnull;
        snapshot.digest ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(item));
        snapshot.digest *= 1099511628211ULL;
    }
    snapshot.valid = true;
    return snapshot;
}

namespace {

void log_original_event_inventory_snapshot(const char* phase, uint64_t event,
                                           bool session_has_projected_item,
                                           int source_bag, int source_slot,
                                           const PhysicalInventorySnapshot& snapshot) {
    VIRTBAG_LOG("orig_event %s event=0x%llx session_projected=%d source=%d/%d inven_valid=%d digest=0x%llx nonnull=%d",
                phase, static_cast<unsigned long long>(event),
                session_has_projected_item ? 1 : 0, source_bag, source_slot,
                snapshot.valid ? 1 : 0,
                static_cast<unsigned long long>(snapshot.digest), snapshot.nonnull);
}

bool c18_native_bag_candidate_locked() {
    const auto* game = runtime_state().host.game;
    return game != nullptr && game->target.has_capability != nullptr &&
           game->target.has_capability(
               qol::contracts::kQolTargetCapabilityC18NativeBagLayoutCandidate) != 0;
}

uint64_t call_original_event_with_inventory_guard(uint64_t event, uint64_t param,
                                                  uint64_t param2) {
    if (qol::extension_bag::ui_state().persistence_hooks.orig_event == nullptr) return 0;

    bool session_has_projected_item = false;
    {
        ::qol::extension_bag::ReentryLock lock;
        session_has_projected_item = projected_drag_session_active_locked();
    }
    // Only a release (0x18) can commit INVEN_MoveItem. Press/move events (0x17/
    // 0x19) do not change physical inventory, so scanning all 6x16 slots twice
    // for every move only adds input latency.
    if (!session_has_projected_item || event != 0x18) {
        return qol::extension_bag::ui_state().persistence_hooks.orig_event(event, param, param2);
    }

    PhysicalInventorySnapshot before{};
    int source_bag = -1;
    int source_slot = -1;
    {
        ::qol::extension_bag::ReentryLock lock;
        source_bag = qol::extension_bag::ui_state().ui.extension_drag_session.source_bag;
        source_slot = qol::extension_bag::ui_state().ui.extension_drag_session.source_slot;
        before = capture_physical_inventory_snapshot_locked();
        log_original_event_inventory_snapshot("pre", event, true, source_bag, source_slot, before);
    }

    const uint64_t result = qol::extension_bag::ui_state().persistence_hooks.orig_event(event, param, param2);
    {
        ::qol::extension_bag::ReentryLock lock;
        const PhysicalInventorySnapshot after = capture_physical_inventory_snapshot_locked();
        log_original_event_inventory_snapshot("post", event, session_has_projected_item,
                                              source_bag, source_slot, after);
        if (session_has_projected_item && before.valid && after.valid &&
            virtual_bag::physical_inventory_snapshot_changed(before.slots, after.slots)) {
            // 原版库存的写入权归 core.item。这里只保留诊断，不通过 EB 的写端口回滚；
            // 否则 EB 会在 core.item 不知情的情况下改写原版背包指针表。
            QOL_LOG_ERROR(QolDomain::kExtensionBag,
                          "ERROR original event changed physical inventory while projected session "
                          "active event=0x%llx before_digest=0x%llx after_digest=0x%llx",
                          static_cast<unsigned long long>(event),
                          static_cast<unsigned long long>(before.digest),
                          static_cast<unsigned long long>(after.digest));
            for (size_t index = 0; index < before.slots.size(); ++index) {
                if (before.slots[index] == after.slots[index]) continue;
                const int bag = static_cast<int>(index / virtual_bag::kSlotCount);
                const int slot = static_cast<int>(index % virtual_bag::kSlotCount);
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR physical inventory mutation bag=%d slot=%d old=%p new=%p", bag,
                              slot, before.slots[index], after.slots[index]);
            }
        }
    }
    return result;
}

bool consume_projected_release_locked(::qol::extension_bag::ReentryLock& lock, int64_t x, int64_t y) {
    if (!projected_drag_session_active_locked()) return false;
    auto& session = qol::extension_bag::ui_state().ui.extension_drag_session;
    if (session.phase == virtual_bag::DragPhase::kCommitted ||
        session.phase == virtual_bag::DragPhase::kRejected ||
        session.phase == virtual_bag::DragPhase::kCancelled) {
        QOL_LOG_DEBUG(QolDomain::kExtensionBag, "projected release consumed terminal session event=0x18 token=%llu phase=%d",
                    static_cast<unsigned long long>(session.token),
                    static_cast<int>(session.phase));
        session = {};
        return true;
    }
    const int original_target_bag = original_bag_button_index(x, y);
    bool routed = false;
    if (original_target_bag >= 0 &&
        virtual_bag::valid_original_transaction_bag(original_target_bag)) {
        session.target_kind = virtual_bag::DragTargetKind::kOriginalBag;
        session.target_bag = original_target_bag;
        session.target_slot = -1;
        routed = route_deferred_extension_to_original_locked(lock);
    } else {
        routed = route_projected_session_drop_locked(lock, x, y);
    }
    if (session.phase != virtual_bag::DragPhase::kCommitted &&
        session.phase != virtual_bag::DragPhase::kRejected &&
        session.phase != virtual_bag::DragPhase::kCancelled) {
        cancel_projected_drag_session_locked("release-intercepted-without-target");
    }
    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "projected release intercepted event=0x18 source=%d/%d target_original=%d routed=%d",
                session.source_bag, session.source_slot, original_target_bag, routed ? 1 : 0);
    session = {};
    return true;
}

}

uint64_t virtual_bag_event(uint64_t event, uint64_t param, uint64_t param2) {
    bool session_admitted = false;
    if (p5_panel_observation_enabled()) {
        ::qol::extension_bag::ReentryLock lock;
        log_p5_observation_locked("panel-entry", nullptr, event, param2 != 0, param != 0,
                                  nullptr, false, 0);
        session_admitted = extension_bag_session_admitted_locked();
    } else {
        ::qol::extension_bag::ReentryLock lock;
        session_admitted = extension_bag_session_admitted_locked();
    }
    // Slot-data admission gates every extension mutation; rejected sessions only delegate.
    if (!session_admitted) {
        VIRTBAG_LOG("touch bypass event=0x%llx reason=session_not_admitted",
                    static_cast<unsigned long long>(event));
        // Explicit state access equivalent: return g_orig_event != nullptr ? g_orig_event(event, param, param2) : 0
        return qol::extension_bag::ui_state().persistence_hooks.orig_event != nullptr ? qol::extension_bag::ui_state().persistence_hooks.orig_event(event, param, param2) : 0;
    }
    const bool extension_enabled = runtime_state().enabled.load();
    int32_t state_open = 0;
    const auto* world_facts = extension_bag_world_facts();
    const bool state_ready = world_facts != nullptr && world_facts->game_state_gate != nullptr &&
        qol::contracts::succeeded(world_facts->game_state_gate(&state_open));
    const uint32_t gamestate = state_ready && state_open != 0 ? 0U : 1U;
    if (!extension_enabled || gamestate != 0) {
        if (extension_enabled && event == 0x18) {
            bool release_cleanup_needed = false;
            int64_t release_x = 0;
            int64_t release_y = 0;
            {
                ::qol::extension_bag::ReentryLock lock;
                if (projected_drag_session_active_locked()) {
                    if (param != 0) {
                        release_x = *reinterpret_cast<const int64_t*>(param);
                        release_y = *reinterpret_cast<const int64_t*>(param + 8);
                    } else {
                        void* moving = nullptr;
                        void* drop_source = nullptr;
                        (void)extension_bag_ui_touch_state(&moving, &drop_source, &release_x, &release_y);
                    }
                    cancel_projected_drag_session_locked("release-bypassed-gamestate");
                    qol::extension_bag::ui_state().ui.extension_drag_session = {};
                    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "projected release intercepted event=0x18 reason=gamestate=%u",
                                gamestate);
                    // Non-world cancellation is unhandled: clear native selection as original does.
                    release_cleanup_needed = true;
                }
            }
            if (release_cleanup_needed) {
                complete_original_release_cleanup_locked(nullptr, release_x, release_y, param2, false);
                return 1;
            }
        }
        if (event == 0x17 || event == 0x18 || event == 0x19) {
            VIRTBAG_LOG("touch bypass event=0x%llx enabled=%d gamestate=%u hooked=%d",
                        static_cast<unsigned long long>(event), extension_enabled ? 1 : 0,
                        gamestate, runtime_state().persistence.state_entry != nullptr ? 1 : 0);
        }
        const uint64_t result = call_original_event_with_inventory_guard(event, param, param2);
        if (p5_panel_observation_enabled()) {
            ::qol::extension_bag::ReentryLock lock;
            log_p5_observation_locked("panel-bypass-post", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, result);
        }
        return result;
    }
    int64_t x = 0;
    int64_t y = 0;
    bool exiting_to_original = false;
    if (event == 0x17 && param != 0) {
        x = *reinterpret_cast<const int64_t*>(param);
        y = *reinterpret_cast<const int64_t*>(param + 8);
        ::qol::extension_bag::ReentryLock lock;
        log_exit_trace_locked("event_enter", event, param, param2, x, y);
        // 只留随输入变化的量：网格原点/尺寸与页签原点是常量，指针本身不携带
        // 可读信息；代际号足以判断"命中的是不是当前那一份视图"。
        QOL_LOG_DEBUG(QolDomain::kExtensionBag, "touch bounds event=0x%llx x=%lld y=%lld grid_hit=%d mode=%d selected=%d overlay=%d root_gen=%llu state_gen=%llu",
                    static_cast<unsigned long long>(event), static_cast<long long>(x), static_cast<long long>(y),
                    extension_bag_grid_hit(x, y) ? 1 : 0, static_cast<int>(runtime_state().objects.bag.mode),
                    runtime_state().objects.bag.selected, qol::extension_bag::ui_state().objects.module_view_installed ? 1 : 0,
                    static_cast<unsigned long long>(qol::extension_bag::ui_state().ui.extension_tab_generation),
                    static_cast<unsigned long long>(qol::extension_bag::ui_state().ui.inventory_generation));
        if (qol::extension_bag::ui_state().ui.extension_touch_capture) {
            log_p5_observation_locked("panel-capture-press", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
    if (extension_bag_grid_hit(x, y)) {
            // G-6/G-7 零干预原则：投影常驻时网格触摸完全放行原版控件链
            // （TouchHandle 自带命中/选中动画/详情；操作按钮由 menu_gate 拦截；
            // 拖动 drop 由 SaveItemOnEmpty 门禁拦截）。模块不做任何状态清理——
            // press 时 reset/clear 会破坏 TouchHandle 的按压记录导致点击失效。
        }
    int projected_slot = -1;
        bool projected_item_press = false;
        if (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
            qol::extension_bag::ui_state().objects.module_view_installed && virtual_bag::valid_index(qol::extension_bag::ui_state().objects.module_view_index) &&
            qol::extension_bag::ui_state().objects.projected_item_root != nullptr) {
            // Hit the projected control rects directly. The fixed grid constants
            // disagree with the live control spacing at the cell edges, which
            // made an item press resolve to the neighbouring empty slot and
            // skipped the projected drag session.
            projected_slot = extension_bag_projected_slot_index(
                qol::extension_bag::ui_state().objects.projected_item_root, x, y,
                runtime_state().objects.bag.capacities[qol::extension_bag::ui_state().objects.module_view_index]);
            projected_item_press = projected_slot >= 0;
        }
        if (runtime_state().objects.bag.mode != virtual_bag::Mode::kOriginal &&
            extension_bag_grid_hit(x, y) && !projected_item_press) {
            // The original grid is hidden underneath the module grid layout.
            // Consume it even when no extension drag is active.
            qol::extension_bag::ui_state().ui.extension_drag = {};
            reset_drag_state_locked(nullptr);
            qol::extension_bag::ui_state().ui.extension_touch_capture = true;
            log_p5_observation_locked("panel-hidden-grid-press", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
        if (projected_item_press && qol::extension_bag::ui_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kIdle) {
            begin_projected_drag_session_locked(projected_slot);
        }
        const int target_original_bag = original_bag_button_index(x, y);
        if (!qol::extension_bag::ui_state().ui.extension_touch_capture && runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
            target_original_bag >= 0) {
            restore_module_view_locked();
            virtual_bag::begin_exit_module(&runtime_state().objects.bag);
            clear_original_bag_selection_locked();
            qol::extension_bag::ui_state().ui.exit_display_bag = qol::extension_bag::ui_state().objects.original_current_got < virtual_bag::kOriginalNativeBagCount ?
                                 qol::extension_bag::ui_state().objects.original_current_got : qol::extension_bag::ui_state().objects.original_current_direct;
            if (bind_original_exit_display_bag_locked(target_original_bag)) {
                qol::extension_bag::ui_state().ui.exit_display_bag = static_cast<uint8_t>(target_original_bag);
            }
            exiting_to_original = true;
            log_exit_trace_locked("original_press_pre_orig", event, param, param2, x, y);
            log_p5_observation_locked("panel-original-pre", nullptr, event, param2 != 0,
                                      param != 0, nullptr, false, 0, x, y);
        }
    }
    if (event == 0x18) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else {
            void* moving = nullptr;
            void* drop_source = nullptr;
            (void)extension_bag_ui_touch_state(&moving, &drop_source, &x, &y);
        }
        bool release_cleanup_needed = false;
        bool release_cleanup_handled = false;
        {
            ::qol::extension_bag::ReentryLock lock;
            log_exit_trace_locked("event_release", event, param, param2, x, y);
        if (virtual_bag::projected_release_owns_event(
                event, projected_drag_session_active_locked())) {
            // 0x18 是投影拖拽的唯一提交 owner。必须在原版 TouchHandle/INVEN_MoveItem
            // 之前完成扩展事务；失败也吞掉原版，防止真实物理槽被二次改写。
            consume_projected_release_locked(lock, x, y);
                // Owner/terminal release is handled by the module; preserve native selection.
                release_cleanup_needed = true;
                release_cleanup_handled = true;
            } else if (qol::extension_bag::ui_state().ui.extension_touch_capture) {
            if (param == 0 && x == 0 && y == 0 && qol::extension_bag::ui_state().ui.extension_drag.active) {
                x = qol::extension_bag::ui_state().ui.extension_drag.current_x;
                y = qol::extension_bag::ui_state().ui.extension_drag.current_y;
            }
            if (param == 0 && x == 0 && y == 0) {
                qol::extension_bag::ui_state().ui.extension_drag = {};
                qol::extension_bag::ui_state().ui.extension_touch_capture = false;
                log_p5_observation_locked("panel-capture-release-empty", nullptr, event,
                                          param2 != 0, param != 0, nullptr, true, 1, x, y);
                // Empty capture has no module transaction; use the unhandled cleanup variant.
                release_cleanup_needed = true;
                release_cleanup_handled = false;
            } else {
                constexpr int64_t kTouchSlop = 24;
                const int64_t dx = x - qol::extension_bag::ui_state().ui.extension_drag.press_x;
                const int64_t dy = y - qol::extension_bag::ui_state().ui.extension_drag.press_y;
                const bool is_click = qol::extension_bag::ui_state().ui.extension_drag.active &&
                                      dx >= -kTouchSlop && dx <= kTouchSlop &&
                                      dy >= -kTouchSlop && dy <= kTouchSlop;
                if (is_click) {
                    runtime_state().objects.bag.inspected = static_cast<int>(qol::extension_bag::ui_state().ui.extension_drag.slot);
                    const virtual_bag::Item& item =
                        runtime_state().objects.bag.items[qol::extension_bag::ui_state().ui.extension_drag.bag][qol::extension_bag::ui_state().ui.extension_drag.slot];
                    QOL_LOG_DEBUG(QolDomain::kExtensionBag, "extension item click bag=%d slot=%d category=%d count=%d",
                                qol::extension_bag::ui_state().ui.extension_drag.bag, qol::extension_bag::ui_state().ui.extension_drag.slot, item.category,
                                item.count);
                    log_exit_trace_locked("extension_item_click", event, param, param2, x, y);
                    // The module consumed the click; still clear native selection state.
                    release_cleanup_handled = false;
                } else {
                    const bool handled = qol::extension_bag::ui_state().ui.extension_drag.active &&
                                         handle_bag_drop_release_locked(lock, x, y);
                    // Drop keeps the transaction result; failed/no-source drops use unhandled cleanup.
                    release_cleanup_handled = handled;
                }
                qol::extension_bag::ui_state().ui.extension_drag = {};
                qol::extension_bag::ui_state().ui.extension_touch_capture = false;
                log_p5_observation_locked("panel-capture-release", nullptr, event, param2 != 0,
                                          param != 0, nullptr, true, 1, x, y);
                release_cleanup_needed = true;
            }
        } else if (runtime_state().objects.bag.mode == virtual_bag::Mode::kOriginal &&
            handle_bag_drop_release_locked(lock, x, y)) {
            log_p5_observation_locked("panel-original-release-handled", nullptr, event,
                                      param2 != 0, param != 0, nullptr, true, 1, x, y);
            // Original-bag drop was consumed by the module route; skip selected-control clearing.
            release_cleanup_needed = true;
            release_cleanup_handled = true;
        }
        }
        if (release_cleanup_needed) {
            complete_original_release_cleanup_locked(nullptr, x, y, param2, release_cleanup_handled);
            return 1;
        }
        // G-10：tab 点击的坐标兜底已删除——tab 点击由控件回调
        // （extension_tab_button_clicked → queue_extension_tab_click）唯一处理，
        // 消除坐标常量与控件树的双路径分歧。
        // Phase one is deliberately read-only. Do not route release events to
        // the legacy projection/move transaction handlers.
    }
    if (event == 0x19) {
        if (param != 0) {
            x = *reinterpret_cast<const int64_t*>(param);
            y = *reinterpret_cast<const int64_t*>(param + 8);
        } else {
            void* moving = nullptr;
            void* drop_source = nullptr;
            (void)extension_bag_ui_touch_state(&moving, &drop_source, &x, &y);
        }
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().ui.extension_touch_capture) {
            // 0x19 is the native MOVE event. Keep the source and capture
            // alive until 0x18 performs click-vs-drop classification.
            if (qol::extension_bag::ui_state().ui.extension_drag.active) {
                qol::extension_bag::ui_state().ui.extension_drag.current_x = x;
                qol::extension_bag::ui_state().ui.extension_drag.current_y = y;
            }
            log_p5_observation_locked("panel-capture-move", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
            return 1;
        }
    }
    if (qol::extension_bag::ui_state().ui.extension_touch_capture && event != 0x17 && event != 0x18 && event != 0x19) {
        ::qol::extension_bag::ReentryLock lock;
        qol::extension_bag::ui_state().ui.extension_drag = {};
        qol::extension_bag::ui_state().ui.extension_touch_capture = false;
        reset_drag_state_locked(nullptr);
        log_exit_trace_locked("event_cancel", event, param, param2, x, y);
        log_p5_observation_locked("panel-capture-cancel", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, 1, x, y);
        return 1;
    }
    if (qol::extension_bag::ui_state().ui.extension_touch_capture) {
        // Consume every event in the captured sequence, not only release and cancel.
        if (p5_panel_observation_enabled()) {
            ::qol::extension_bag::ReentryLock lock;
            log_p5_observation_locked("panel-capture-consume", nullptr, event, param2 != 0,
                                      param != 0, nullptr, true, 1, x, y);
        }
        return 1;
    }
    if (event != 0x17) {
        ::qol::extension_bag::ReentryLock lock;
        log_exit_trace_locked("event_enter", event, param, param2);
    }
    if (exiting_to_original) {
        const uint64_t original_result = call_original_event_with_inventory_guard(event, param, param2);
        ::qol::extension_bag::ReentryLock lock;
        log_exit_trace_locked("original_press_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-original-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, original_result, x, y);
        return original_result;
    }
    {
        ::qol::extension_bag::ReentryLock lock;
        log_exit_trace_locked("delegate_pre_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-pre", nullptr, event, param2 != 0,
                                  param != 0, nullptr, false, 0, x, y);
    }
    const uint64_t result = call_original_event_with_inventory_guard(event, param, param2);
    {
        ::qol::extension_bag::ReentryLock lock;
        log_exit_trace_locked("delegate_post_orig", event, param, param2, x, y);
        log_p5_observation_locked("panel-delegate-post", nullptr, event, param2 != 0,
                                  param != 0, nullptr, true, result, x, y);
        // 0x17/0x19 只负责原版 TouchHandle 的 press/move 握手，不提交扩展事务。
        // 投影 session 的 0x18 已在 delegate 前返回，不能在这里再出现第二个 owner。
        if (event == 0x17 || event == 0x19) advance_projected_drag_session_locked();
        if ((event == 0x17 || event == 0x18) &&
            runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
            c18_native_bag_candidate_locked() &&
            !projected_drag_session_active_locked()) {
            const int raw_bag = raw_current_bag_locked();
            if (raw_bag >= 0 && raw_bag != qol::extension_bag::ui_state().objects.original_current_got) {
                // 必须先归还投影并让原版按原生当前袋重画物品区：改版袋 6..N-1 的
                // 内容只在投影撤掉、容量字还原后才会正确切过去。
                restore_module_view_locked();
                if (raw_bag < virtual_bag::kOriginalNativeBagCount) {
                    set_original_bag_locked(raw_bag);
                    if (runtime_state().host.game != nullptr &&
                        runtime_state().host.write_port->ui.refresh_item_area != nullptr) {
                        virtual_bag_call_original_refresh_item_area_with_guard();
                    }
                    virtual_bag::enter_original(&runtime_state().objects.bag, raw_bag);
                    extension_bag_mark_dirty_locked();
                } else {
                    virtual_bag::enter_original(&runtime_state().objects.bag, 0);
                    extension_bag_mark_dirty_locked();
                }
                persist_state_locked();
                qol::extension_bag::ui_state().ui.exit_display_bag = -1;
                return result;
            }
        }
        if (runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule) {
            if (event == 0x18 && !complete_original_exit_locked()) {
                cancel_original_exit_locked();
            }
            return result;
        }
        // 物品格点击也会产生 0x17；原版袋按钮已在上方单独处理。
        // 这里仅处理原版事件确实改写当前袋的兜底退出路径。
        const bool original_bag_changed = runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
                                          original_bag_locked() != qol::extension_bag::ui_state().objects.original_current_got;
        if (event == 0x17 && runtime_state().objects.bag.mode == virtual_bag::Mode::kModule &&
            original_bag_changed) {
            const int selected_original_bag = original_bag_locked();
            restore_module_view_locked();
            set_original_bag_locked(selected_original_bag);
            if (runtime_state().host.game != nullptr && runtime_state().host.write_port->ui.refresh_item_area != nullptr) {
                virtual_bag_call_original_refresh_item_area_with_guard();
            }
            virtual_bag::enter_original(&runtime_state().objects.bag, selected_original_bag);
            extension_bag_mark_dirty_locked();
            persist_state_locked();
            qol::extension_bag::ui_state().ui.exit_display_bag = -1;
        } else if (event == 0x17 || event == 0x18) {
            if (c18_native_bag_candidate_locked()) {
                const int raw_bag = raw_current_bag_locked();
                runtime_state().objects.bag.original_selected =
                    raw_bag >= 0 && raw_bag < virtual_bag::kOriginalNativeBagCount ? raw_bag : 0;
            } else {
                runtime_state().objects.bag.original_selected = original_bag_locked();
            }
        }
    }
    return result;
}

// 生命周期节拍（D2a-3c）：原 150ms 轮询线程体的主线程化，改由 infra::scheduler 帧任务
// （kLogicPre，节流 30 帧，count=0 常驻）。锁纪律与原线程逐点一致：g_virtual_bag_mtx
// 短临界区包住 prepare_main_menu_locked；R-44 不变量不变。
namespace {

int32_t extension_bag_lifecycle_tick(int64_t frame, void* ctx) {
    (void)ctx;
    int world_state = -1;
    (void)frame;
    if (!read_world_state_id(&world_state)) return 1;
    const bool due = qol::features::extension_bag::model::main_menu_cleanup_due(
        qol::extension_bag::ui_state().aux.lifecycle_last_state, world_state, true,
        qol::extension_bag::ui_state().aux.main_menu_cleanup_dirty.load(std::memory_order_acquire));
    if (due) {
        ::qol::extension_bag::ReentryLock lock;
        if (!extension_bag_session_admitted_locked()) {
            // An unloaded or degraded slot is not an empty inventory that may be reset.
            qol::extension_bag::ui_state().aux.main_menu_cleanup_dirty.store(false, std::memory_order_release);
        } else if (!prepare_main_menu_locked()) {
            qol::extension_bag::ui_state().aux.main_menu_cleanup_dirty.store(true, std::memory_order_release);
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "lifecycle main-menu cleanup failed; retry scheduled");
        } else {
            qol::extension_bag::ui_state().aux.main_menu_cleanup_dirty.store(false, std::memory_order_release);
        }
    }
    // 进档后尽早装载 sidecar 状态：快捷栏/物品数量查询走 EB 扫描，若等到打开
    // 背包面板才装载，进档后到首次开包前的数量会显示为 0。
    {
        ::qol::extension_bag::ReentryLock lock;
    }
    uint64_t shortcut_revision = 0;
    const qol::contracts::QolHostSdk* sdk = nullptr;
    bool refresh_shortcut = false;
    {
        ::qol::extension_bag::ReentryLock lock;
        if (world_state == 5 && extension_bag_session_admitted_locked()) {
            shortcut_revision = extension_bag_item_revision_locked();
            refresh_shortcut = runtime_state().persistence.shortcut_refreshed_revision !=
                               static_cast<int64_t>(shortcut_revision);
            sdk = runtime_state().host.sdk;
        }
    }
    if (refresh_shortcut && sdk != nullptr && sdk->capabilities.require != nullptr) {
        const void* refresh_iface = nullptr;
        const QolResult require_result = sdk->capabilities.require(
            qol::contracts::kInventoryRefreshCapabilityId,
            QolVersionRange{static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMajor),
                            static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMinor)},
            &refresh_iface);
        const auto* refresh = require_result.ok == kQolOk
            ? static_cast<const qol::contracts::InventoryRefreshApi*>(refresh_iface)
            : nullptr;
        if (refresh != nullptr && refresh->update_shortcut_category != nullptr) {
            // 原版 PLAYER_UpdateShortcut(category) 只刷新同类别快捷槽。先在锁内收集
            // 扩展袋中出现的类别，释放锁后再逐个调用，避免 GetItemCount 回调重入 EB。
            int32_t categories[virtual_bag::kMaxExtensionBags * virtual_bag::kSlotCount]{};
            size_t category_count = 0;
            {
                ::qol::extension_bag::ReentryLock lock;
                if (extension_bag_session_admitted_locked()) {
                    const int active = virtual_bag::active_extension_bags();
                    for (int bag_index = 0; bag_index < active; ++bag_index) {
                        const int capacity =
                            runtime_state().objects.bag.capacities[bag_index];
                        for (int slot_index = 0; slot_index < capacity; ++slot_index) {
                            const virtual_bag::Item& item =
                                runtime_state().objects.bag.items[bag_index][slot_index];
                            const int32_t category = item.category;
                            if (category <= 0) continue;
                            bool seen = false;
                            for (size_t index = 0; index < category_count; ++index) {
                                if (categories[index] == category) {
                                    seen = true;
                                    break;
                                }
                            }
                            if (!seen &&
                                category_count <
                                    sizeof(categories) / sizeof(categories[0])) {
                                categories[category_count++] = category;
                            }
                        }
                    }
                }
            }
            for (size_t index = 0; index < category_count; ++index) {
                refresh->update_shortcut_category(categories[index]);
            }
            ::qol::extension_bag::ReentryLock lock;
            if (extension_bag_session_admitted_locked() &&
                extension_bag_item_revision_locked() == shortcut_revision) {
                runtime_state().persistence.shortcut_refreshed_revision =
                    static_cast<int64_t>(shortcut_revision);
            }
        } else if (refresh != nullptr && refresh->update_shortcut != nullptr) {
            refresh->update_shortcut();
        }
    }
    qol::extension_bag::ui_state().aux.lifecycle_last_state = world_state;
    return 1;
}

}

void ensure_lifecycle_task() {
    if (qol::extension_bag::ui_state().aux.lifecycle_task != 0) return;
    qol::extension_bag::ui_state().aux.lifecycle_last_state = -1;
    if (runtime_state().host.sdk == nullptr || runtime_state().host.sdk->scheduler.add == nullptr) return;
    uint64_t task = 0;
    const QolResult result = runtime_state().host.sdk->scheduler.add(1, &extension_bag_lifecycle_tick, nullptr, 30, 0,
                                                  &task);
    if (result.ok == kQolOk) qol::extension_bag::ui_state().aux.lifecycle_task = task;
}

void remove_lifecycle_task() {
    if (qol::extension_bag::ui_state().aux.lifecycle_task == 0) return;
    if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->scheduler.remove != nullptr) {
        (void)runtime_state().host.sdk->scheduler.remove(qol::extension_bag::ui_state().aux.lifecycle_task);
    }
    qol::extension_bag::ui_state().aux.lifecycle_task = 0;
}

bool prepare_main_menu_locked() {
    if (!extension_bag_session_admitted_locked()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "prepare main-menu rejected: slot data session not admitted");
        return false;
    }
    if (!module_cache_clear_preflight_locked()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "prepare main-menu preflight failed");
        return false;
    }
    teardown_extension_bag_hosts_locked();
    if (!clear_module_cache_locked()) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "prepare main-menu blocked by active module object");
        return false;
    }
    runtime_state().objects.bag = {};
    runtime_state().persistence.loaded_slot = -2;
    runtime_state().persistence.dirty = false;
    qol::extension_bag::ui_state().ui.inventory_frame_active = false;
    cancel_projected_drag_session_locked("main-menu");
    return true;
}
