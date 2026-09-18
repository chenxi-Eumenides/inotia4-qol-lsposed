enum class DragPhase : uint8_t {
    kIdle,
    kPressed,
    kNativeMoving,
    kTargetResolved,
    kTransactionInFlight,
    kCancelled,
    kRejected,
    kCommitted,
};

enum class DragTerminalCause : uint8_t {
    kNone,
    kReleased,
    kCancelled,
    kStaleGeneration,
    kInvalidTarget,
    kIllegalEvent,
    kTransactionRejected,
};

enum class DragTransition : uint8_t {
    kAdvanced,
    kIdempotentNoop,
    kIllegal,
};

enum class DragTargetKind : uint8_t {
    kOriginalBag,
    kExtensionSlot,
    kExtensionTab,
    kEquipmentSlot,
    kTaskBagRejected,
    kInvalid,
    kCancel,
};

struct ExtensionDragSession {
    uint32_t protocol_version = 1;
    uint64_t token = 0;
    uint64_t view_generation = 0;
    int source_bag = -1;
    int source_slot = -1;
    DragTargetKind target_kind = DragTargetKind::kInvalid;
    int target_bag = -1;
    int target_slot = -1;
    uint32_t press_sequence = 0;
    uint32_t release_sequence = 0;
    DragPhase phase = DragPhase::kIdle;
    DragTerminalCause terminal_cause = DragTerminalCause::kNone;
    bool transaction_granted = false;
    bool transaction_dispatched = false;
};

inline bool valid_index(int index);
inline bool valid_original_transaction_bag(int bag);
inline bool valid_extension_logical_bag(int bag);

static_assert(!std::is_pointer<decltype(ExtensionDragSession::protocol_version)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::token)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::view_generation)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::source_bag)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::source_slot)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::target_kind)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::target_bag)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::target_slot)>::value,
              "drag session must not contain pointers");

inline bool drag_session_generation_current(const ExtensionDragSession& session,
                                            uint64_t view_generation) {
    return session.view_generation == view_generation;
}

inline DragTransition session_begin(ExtensionDragSession* session, uint64_t token,
                                    uint64_t view_generation, int source_bag, int source_slot) {
    if (session == nullptr || session->phase != DragPhase::kIdle || token == 0 ||
        !valid_index(source_bag) || source_slot < 0 || source_slot >= kSlotCount) {
        return DragTransition::kIllegal;
    }
    session->token = token;
    session->view_generation = view_generation;
    session->source_bag = source_bag;
    session->source_slot = source_slot;
    session->target_kind = DragTargetKind::kInvalid;
    session->target_bag = -1;
    session->target_slot = -1;
    session->press_sequence = 0;
    session->release_sequence = 0;
    session->terminal_cause = DragTerminalCause::kNone;
    session->transaction_granted = false;
    session->transaction_dispatched = false;
    session->phase = DragPhase::kPressed;
    return DragTransition::kAdvanced;
}

inline DragTransition session_on_native_moving(ExtensionDragSession* session,
                                               uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kNativeMoving) return DragTransition::kIdempotentNoop;
    if (session->phase != DragPhase::kPressed) return DragTransition::kIllegal;
    ++session->press_sequence;
    session->phase = DragPhase::kNativeMoving;
    return DragTransition::kAdvanced;
}

inline DragTransition session_resolve_target(ExtensionDragSession* session,
                                             uint64_t view_generation, DragTargetKind target) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation) ||
        session->phase == DragPhase::kCancelled || session->phase == DragPhase::kRejected) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kTargetResolved && target == DragTargetKind::kExtensionSlot) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase != DragPhase::kNativeMoving || target == DragTargetKind::kCancel) {
        return DragTransition::kIllegal;
    }
    if (target != DragTargetKind::kOriginalBag && target != DragTargetKind::kExtensionSlot &&
        target != DragTargetKind::kExtensionTab && target != DragTargetKind::kEquipmentSlot) {
        session->phase = DragPhase::kRejected;
        session->terminal_cause = DragTerminalCause::kInvalidTarget;
        return DragTransition::kAdvanced;
    }
    session->target_kind = target;
    session->phase = DragPhase::kTargetResolved;
    return DragTransition::kAdvanced;
}

inline DragTransition session_begin_transaction(ExtensionDragSession* session,
                                                 uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kTransactionInFlight) return DragTransition::kIdempotentNoop;
    if (session->phase != DragPhase::kTargetResolved || session->transaction_granted) {
        return DragTransition::kIllegal;
    }
    session->transaction_granted = true;
    session->phase = DragPhase::kTransactionInFlight;
    return DragTransition::kAdvanced;
}

inline DragTransition session_finish_transaction(ExtensionDragSession* session, bool committed) {
    if (session == nullptr || session->phase != DragPhase::kTransactionInFlight) {
        return DragTransition::kIllegal;
    }
    session->phase = committed ? DragPhase::kCommitted : DragPhase::kRejected;
    session->terminal_cause = committed ? DragTerminalCause::kReleased
                                        : DragTerminalCause::kTransactionRejected;
    return DragTransition::kAdvanced;
}

inline DragTransition session_on_release(ExtensionDragSession* session, uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kCommitted || session->phase == DragPhase::kRejected ||
        session->phase == DragPhase::kCancelled) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase != DragPhase::kTransactionInFlight) return DragTransition::kIllegal;
    ++session->release_sequence;
    return DragTransition::kAdvanced;
}

inline DragTransition session_on_cancel(ExtensionDragSession* session,
                                        uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kCancelled) return DragTransition::kIdempotentNoop;
    if (session->phase != DragPhase::kPressed && session->phase != DragPhase::kNativeMoving &&
        session->phase != DragPhase::kTargetResolved) {
        return DragTransition::kIllegal;
    }
    session->phase = DragPhase::kCancelled;
    session->terminal_cause = DragTerminalCause::kCancelled;
    return DragTransition::kAdvanced;
}

inline bool session_may_create_transaction(const ExtensionDragSession& session) {
    return session.phase == DragPhase::kTransactionInFlight && session.transaction_granted &&
           !session.transaction_dispatched;
}

inline bool session_claim_transaction(ExtensionDragSession* session) {
    if (session == nullptr || !session_may_create_transaction(*session)) return false;
    session->transaction_dispatched = true;
    return true;
}

inline DragTargetKind classify_drag_target(int bag, int slot, bool extension_tab = false,
                                           bool cancelled = false) {
    if (cancelled) return DragTargetKind::kCancel;
    if (extension_tab) return DragTargetKind::kExtensionTab;
    if (bag == kOriginalTaskBag) return DragTargetKind::kTaskBagRejected;
    if (slot < 0 || slot >= kSlotCount) return DragTargetKind::kInvalid;
    if (valid_original_transaction_bag(bag)) return DragTargetKind::kOriginalBag;
    if (valid_extension_logical_bag(bag)) return DragTargetKind::kExtensionSlot;
    return DragTargetKind::kInvalid;
}
