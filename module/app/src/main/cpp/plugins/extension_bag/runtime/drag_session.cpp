#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/runtime/logging.h"

void begin_projected_drag_session_locked(int source_slot) {
    if (runtime_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) return;
    const uint64_t token = runtime_state().p5_observation_sequence.fetch_add(1) + 1;
    if (virtual_bag::session_begin(&runtime_state().ui.extension_drag_session, token,
                                   runtime_state().ui.extension_tab_generation, runtime_state().objects.module_view_index,
                                   source_slot) == virtual_bag::DragTransition::kAdvanced) {
        if (virtual_bag::valid_index(runtime_state().objects.module_view_index) && source_slot >= 0 &&
            source_slot < virtual_bag::kSlotCount) {
            runtime_state().ui.extension_drag_session.source_identity =
                runtime_state().objects.module_object_handles[runtime_state().objects.module_view_index][source_slot];
        }
        VIRTBAG_LOG("p5 session begin token=%llu source=%d/%d generation=%llu",
                    static_cast<unsigned long long>(token), runtime_state().objects.module_view_index, source_slot,
                    static_cast<unsigned long long>(runtime_state().ui.extension_tab_generation));
    }
}

void advance_projected_drag_session_locked() {
    if (runtime_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kPressed) return;
    if (touch_moving_item_control_locked() == nullptr) return;
    virtual_bag::session_on_native_moving(&runtime_state().ui.extension_drag_session,
                                          runtime_state().ui.extension_tab_generation);
}

bool projected_drag_session_active_locked() {
    const auto& session = runtime_state().ui.extension_drag_session;
    // session 本身只由当前投影槽 press 建立；即使投影 generation 随后失效，也必须
    // 继续把 release 视为模块拥有，不能让 stale session 穿过原版 INVEN 链。
    return session.phase != virtual_bag::DragPhase::kIdle &&
           virtual_bag::valid_index(session.source_bag) &&
           session.source_slot >= 0 && session.source_slot < virtual_bag::kSlotCount;
}

void cancel_projected_drag_session_locked(const char* reason) {
    auto& session = runtime_state().ui.extension_drag_session;
    if (session.phase == virtual_bag::DragPhase::kIdle) return;
    if (session.phase == virtual_bag::DragPhase::kRejected ||
        session.phase == virtual_bag::DragPhase::kCommitted) {
        VIRTBAG_LOG("p5 session cleanup reason=%s token=%llu phase=%d",
                    reason != nullptr ? reason : "unknown",
                    static_cast<unsigned long long>(session.token),
                    static_cast<int>(session.phase));
        session = {};
        return;
    }
    const virtual_bag::DragTransition transition =
        virtual_bag::session_on_cancel(&session, runtime_state().ui.extension_tab_generation);
    VIRTBAG_LOG("p5 session cancel reason=%s token=%llu phase=%d transition=%d",
                reason != nullptr ? reason : "unknown",
                static_cast<unsigned long long>(session.token),
                static_cast<int>(session.phase), static_cast<int>(transition));
    if (session.phase == virtual_bag::DragPhase::kCancelled ||
        session.phase == virtual_bag::DragPhase::kRejected ||
        session.phase == virtual_bag::DragPhase::kCommitted) {
        session = {};
    }
}
