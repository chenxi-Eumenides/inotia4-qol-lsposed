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

void free_module_object_locked(int bag, int slot) {
    if (bag < 0 || bag >= virtual_bag::kBagCount || slot < 0 || slot >= virtual_bag::kSlotCount) {
        return;
    }
    void* item = g_module_objects[bag][slot];
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
}
