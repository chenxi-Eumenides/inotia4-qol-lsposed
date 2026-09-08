// P3 所有权账本：借出对象四态跟踪。
ownership::Ledger g_ownership_ledger{};
uint32_t g_module_object_handles[virtual_bag::kBagCount][virtual_bag::kSlotCount]{};

void defer_item_free_locked(void* item) {
    (void)item;
}

void log_ownership_audit_locked(const char* context) {
    const ownership::Audit report = ownership::audit(g_ownership_ledger);
    VIRTBAG_LOG(
        "ownership audit context=%s balanced=%d allocated=%u released=%u handed_over=%u "
        "live=%u objects=%u borrows=%u inventory_owned=%u",
        context, report.balanced ? 1 : 0, g_ownership_ledger.total_allocated,
        g_ownership_ledger.total_released, g_ownership_ledger.total_handed_over,
        report.live_handles, report.outstanding_objects, report.outstanding_borrows,
        report.inventory_owned);
}

bool object_touch_window_active_locked() {
    if (g_extension_touch_capture || g_extension_drag.active) return true;
    if (g_base == 0 || !g_module_view_installed || g_projected_item_root == nullptr ||
        !virtual_bag::valid_index(g_module_view_index)) return false;
    const uint8_t* touch_state = reinterpret_cast<const uint8_t*>(g_base + G_TOUCH_STATE_VMA);
    const void* moving = *reinterpret_cast<const void* const*>(touch_state + TOUCH_STATE_MOVING_CTRL);
    if (moving == nullptr) return false;
    const int capacity = g_virtual_bag_state.capacities[g_module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (valid_child_locked(g_projected_item_root, slot) == moving) return true;
    }
    return false;
}

void retire_custody_item_locked(uint32_t handle, void* item, const char* context) {
    const bool released =
        ownership::release(&g_ownership_ledger, handle) == ownership::Outcome::kOk;
    if (released && !object_touch_window_active_locked() && fn_itempool_free != nullptr) {
        fn_itempool_free(item);
    } else {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership defer free context=%s released=%d touch_window=%d", context,
                    released ? 1 : 0, object_touch_window_active_locked() ? 1 : 0);
    }
    log_ownership_audit_locked(context);
}

void* load_item_payload_tracked_locked(const uint8_t* payload, int payload_size,
                                       const char* context, uint32_t* out_handle) {
    *out_handle = 0;
    void* item = load_item_payload_locked(payload, payload_size, context);
    if (item == nullptr) return nullptr;
    uint32_t handle = 0;
    if (ownership::allocate(&g_ownership_ledger, &handle) != ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
        VIRTBAG_LOG("ownership allocate reject; temp freed context=%s", context);
        return nullptr;
    }
    *out_handle = handle;
    return item;
}

void handover_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::handover_to_inventory(&g_ownership_ledger, handle) !=
        ownership::Outcome::kOk) {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership handover reject; deferred context=%s", context);
    }
    log_ownership_audit_locked(context);
}

void release_tracked_item_locked(uint32_t handle, void* item, const char* context) {
    if (ownership::release(&g_ownership_ledger, handle) == ownership::Outcome::kOk) {
        release_temporary_item_locked(item);
    } else {
        defer_item_free_locked(item);
        VIRTBAG_LOG("ownership release reject; deferred context=%s", context);
    }
    log_ownership_audit_locked(context);
}

bool module_slot_is_assignable_locked(int bag, int slot) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const virtual_bag::Item& descriptor = g_virtual_bag_state.items[bag][slot];
    const ModuleUseState& use = g_module_object_use[bag][slot];
    // handle==0 is the only valid empty-slot ownership state. A pending-release
    // object remains physically live until finish and must never be reused.
    return descriptor.category == 0 && descriptor.count == 0 &&
           descriptor.payload_size == 0 && g_module_objects[bag][slot] == nullptr &&
           !use.active && !use.pending_release && g_module_object_handles[bag][slot] == 0;
}

bool module_object_replacement_allowed_locked(int bag, int slot, const char* context) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const ModuleUseState& use = g_module_object_use[bag][slot];
    if (use.active || use.pending_release) {
        VIRTBAG_LOG("module object replacement rejected while confirm-use active context=%s bag=%d slot=%d item=%p generation=%llu",
                    context, bag, slot, use.item,
                    static_cast<unsigned long long>(use.generation));
        return false;
    }
    return true;
}

bool module_object_release_allowed_locked(int bag, int slot, void* item, const char* context,
                                          bool allow_owner, const ModuleUseToken* token) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    const ModuleUseState& use = g_module_object_use[bag][slot];
    if (!use.active) return true;
    const bool token_matches = token != nullptr && token->item == item &&
                               token->item == use.item &&
                               token->generation == use.generation &&
                               token->owner == use.owner &&
                               g_module_objects[bag][slot] == token->item;
    if (allow_owner && token_matches && token->owner == std::this_thread::get_id()) return true;
    VIRTBAG_LOG("module object release rejected while confirm-use active context=%s bag=%d slot=%d item=%p",
                context, bag, slot, item);
    return false;
}

bool module_object_generation_advance_locked(int bag, int slot, const char* context) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[bag][slot];
    if (!module_object_replacement_allowed_locked(bag, slot, context)) return false;
    ++use.generation;
    if (use.generation == 0) ++use.generation;
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    return true;
}

bool module_use_begin_locked(int bag, int slot, void* item, ModuleUseToken* out_token) {
    if (out_token == nullptr || bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 ||
        slot >= virtual_bag::kSlotCount || item == nullptr || g_module_objects[bag][slot] != item) {
        return false;
    }
    ModuleUseState& use = g_module_object_use[bag][slot];
    if (use.active) return false;
    ++use.generation;
    if (use.generation == 0) ++use.generation;
    use.active = true;
    use.consumed = false;
    use.pending_release = false;
    use.item = item;
    use.owner = std::this_thread::get_id();
    *out_token = {bag, slot, item, use.generation, use.owner};
    return true;
}

bool module_use_finish_locked(const ModuleUseToken& token, bool* out_consumed) {
    if (token.bag < 0 || token.bag >= virtual_bag::kBagCount || token.slot < 0 ||
        token.slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("ERROR module use finish token mismatch invalid slot active=-1 "
                    "pending_release=-1 generation=0 item=%p handle=0 owner_thread=0 "
                    "token_bag=%d token_slot=%d token_generation=%llu token_item=%p",
                    token.item, token.bag, token.slot,
                    static_cast<unsigned long long>(token.generation), token.item);
        return false;
    }
    ModuleUseState& use = g_module_object_use[token.bag][token.slot];
    if (!use.active || use.item != token.item || use.generation != token.generation ||
        use.owner != token.owner || g_module_objects[token.bag][token.slot] != token.item) {
        const auto thread_key = [](const std::thread::id& id) -> unsigned long long {
            return static_cast<unsigned long long>(std::hash<std::thread::id>{}(id));
        };
        VIRTBAG_LOG("ERROR module use finish token mismatch bag=%d slot=%d active=%d "
                    "pending_release=%d generation=%llu item=%p handle=%u owner_thread=%llu "
                    "token_bag=%d token_slot=%d token_generation=%llu token_item=%p "
                    "token_owner_thread=%llu current_object=%p",
                    token.bag, token.slot, use.active ? 1 : 0,
                    use.pending_release ? 1 : 0,
                    static_cast<unsigned long long>(use.generation), use.item,
                    g_module_object_handles[token.bag][token.slot], thread_key(use.owner),
                    token.bag, token.slot, static_cast<unsigned long long>(token.generation),
                    token.item, thread_key(token.owner),
                    g_module_objects[token.bag][token.slot]);
        return false;
    }
    if (out_consumed != nullptr) *out_consumed = use.consumed;
    const bool pending_release = use.pending_release;
    const int bag = token.bag;
    const int slot = token.slot;
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    if (pending_release && !free_module_object_locked(bag, slot, nullptr)) {
        VIRTBAG_LOG("ERROR module use finish deferred release failed bag=%d slot=%d item=%p",
                    bag, slot, token.item);
        return false;
    }
    return true;
}

bool module_use_abort_locked(const ModuleUseToken& token) {
    if (token.bag < 0 || token.bag >= virtual_bag::kBagCount || token.slot < 0 ||
        token.slot >= virtual_bag::kSlotCount) {
        VIRTBAG_LOG("ERROR module use abort token mismatch; slot may remain active invalid slot "
                    "active=-1 pending_release=-1 generation=0 item=%p handle=0 owner_thread=0 "
                    "token_bag=%d token_slot=%d token_generation=%llu token_item=%p",
                    token.item, token.bag, token.slot,
                    static_cast<unsigned long long>(token.generation), token.item);
        return false;
    }
    ModuleUseState& use = g_module_object_use[token.bag][token.slot];
    if (!use.active || use.item != token.item || use.generation != token.generation ||
        use.owner != token.owner || g_module_objects[token.bag][token.slot] != token.item) {
        const auto thread_key = [](const std::thread::id& id) -> unsigned long long {
            return static_cast<unsigned long long>(std::hash<std::thread::id>{}(id));
        };
        VIRTBAG_LOG("ERROR module use abort token mismatch; slot may remain active "
                    "bag=%d slot=%d active=%d pending_release=%d generation=%llu item=%p "
                    "handle=%u owner_thread=%llu token_bag=%d token_slot=%d "
                    "token_generation=%llu token_item=%p token_owner_thread=%llu current_object=%p",
                    token.bag, token.slot, use.active ? 1 : 0,
                    use.pending_release ? 1 : 0,
                    static_cast<unsigned long long>(use.generation), use.item,
                    g_module_object_handles[token.bag][token.slot], thread_key(use.owner),
                    token.bag, token.slot, static_cast<unsigned long long>(token.generation),
                    token.item, thread_key(token.owner),
                    g_module_objects[token.bag][token.slot]);
        return false;
    }
    use.active = false;
    use.consumed = false;
    use.pending_release = false;
    use.item = nullptr;
    use.owner = {};
    return true;
}

bool free_module_object_locked(int bag, int slot, void* raw_use_token) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return false;
    }
    void* item = g_module_objects[bag][slot];
    if (item == nullptr) {
        if (g_module_object_use[bag][slot].active) {
            VIRTBAG_LOG("ERROR module free rejected active token with empty slot bag=%d slot=%d",
                        bag, slot);
            return false;
        }
        return module_object_generation_advance_locked(bag, slot, "free-empty");
    }
    ModuleUseToken* use_token = static_cast<ModuleUseToken*>(raw_use_token);
    const ModuleUseState& current_use = g_module_object_use[bag][slot];
    if (current_use.active && use_token == nullptr) {
        VIRTBAG_LOG("module free rejected without real token bag=%d slot=%d item=%p",
                    bag, slot, item);
        return false;
    }
    if (!module_object_release_allowed_locked(
            bag, slot, item, "free_module_object", true, use_token)) {
        return false;
    }
    if (current_use.active) {
        ModuleUseState& use = g_module_object_use[bag][slot];
        use.consumed = true;
        use.pending_release = true;
        return true;
    }
    if (item != nullptr) {
        if (g_module_view_installed && g_module_view_index == bag &&
            g_projected_item_root != nullptr && fn_control_object_get_child != nullptr &&
            fn_control_item_set_item != nullptr) {
            void* ctrl = valid_child_locked(g_projected_item_root, slot);
            if (ctrl != nullptr) fn_control_item_set_item(ctrl, nullptr);
        }
        const uint32_t handle = g_module_object_handles[bag][slot];
        if (ownership::live_state(g_ownership_ledger, handle,
                                  ownership::State::kBorrowedForView)) {
            ownership::return_from_view(&g_ownership_ledger, handle);
        }
        retire_custody_item_locked(handle, item, "free_module_object");
    }
    g_module_objects[bag][slot] = nullptr;
    g_module_object_categories[bag][slot] = 0;
    g_module_object_hashes[bag][slot] = 0;
    g_module_object_handles[bag][slot] = 0;
    return module_object_generation_advance_locked(bag, slot, "free-object");
}
