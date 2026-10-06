bool merge_projection_available() {
    return g_provider_active.load(std::memory_order_acquire);
}

qol::contracts::MoveMergeProjectionDropResult merge_projection_drop(
    void* destination_control, void* source_control) {
    if (!merge_projection_available()) {
        return qol::contracts::MoveMergeProjectionDropResult::kNotHandled;
    }
    return static_cast<qol::contracts::MoveMergeProjectionDropResult>(
        virtual_bag_projection_drop_to_slot(destination_control, source_control));
}

QolResult merge_projection_identify_extension_item(qol::contracts::QolItemHandle item,
                                                    int32_t* out_bag,
                                                    int32_t* out_slot) {
    if (out_bag == nullptr || out_slot == nullptr || item.slot == 0 || item.generation == 0) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid item identity", 21);
    }
    if (!merge_projection_available()) {
        return qol::contracts::error(kQolErrNotReady, "merge projection unavailable", 29);
    }
    void* native_item = nullptr;
    if (!extension_bag_private_item_for_handle(item, &native_item) || native_item == nullptr) {
        return qol::contracts::error(kQolErrNotFound, "extension item not found", 24);
    }
    int32_t bag = -1;
    int32_t slot = -1;
    if (!virtual_bag_identify_native_item(native_item, &bag, &slot)) {
        return qol::contracts::error(kQolErrNotFound, "extension item not found", 24);
    }
    *out_bag = bag;
    *out_slot = slot;
    return qol::contracts::ok();
}
