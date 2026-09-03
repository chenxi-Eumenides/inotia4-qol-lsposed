void begin_projected_drag_session_locked(int source_slot) {
    if (g_extension_drag_session.phase != virtual_bag::DragPhase::kIdle) return;
    const uint64_t token = g_p5_observation_sequence.fetch_add(1) + 1;
    if (virtual_bag::session_begin(&g_extension_drag_session, token,
                                   g_extension_tab_generation, g_module_view_index,
                                   source_slot) == virtual_bag::DragTransition::kAdvanced) {
        VIRTBAG_LOG("p5 session begin token=%llu source=%d/%d generation=%llu",
                    static_cast<unsigned long long>(token), g_module_view_index, source_slot,
                    static_cast<unsigned long long>(g_extension_tab_generation));
    }
}

void advance_projected_drag_session_locked() {
    if (g_extension_drag_session.phase != virtual_bag::DragPhase::kPressed) return;
    if (touch_moving_item_control_locked() == nullptr) return;
    virtual_bag::session_on_native_moving(&g_extension_drag_session,
                                          g_extension_tab_generation);
}

void cancel_projected_drag_session_locked(const char* reason) {
    auto& session = g_extension_drag_session;
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
        virtual_bag::session_on_cancel(&session, g_extension_tab_generation);
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
