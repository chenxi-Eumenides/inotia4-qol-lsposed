int extension_tab_index(void* ctrl) {
    for (int index = 0; index < virtual_bag::kBagCount; ++index) {
        if (g_extension_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

// 扩展标签的已装备背包物品对象与 RefreshBagArea 使用同一 data[0] 语义。
bool defer_item_free_locked(void* item);

void refresh_tab_bag_items_locked() {
    for (int i = 0; i < virtual_bag::kBagCount; ++i) {
        const uint8_t type = g_virtual_bag_state.types[i];
        void*& slot = g_tab_bag_items[i];
        if (type == 0) {
            if (slot != nullptr) {
                if (defer_item_free_locked(slot)) slot = nullptr;
            }
            continue;
        }
        if (slot != nullptr) {
            int category = -1;
            if (read_item_category(slot, &category) && category == static_cast<int>(type)) continue;
            if (defer_item_free_locked(slot)) slot = nullptr;
        }
        slot = extension_create_item(static_cast<int32_t>(type), 0, 0, 0);
        if (slot != nullptr) {
            const uint32_t count_flags =
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(slot) + I_COUNT);
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(slot) + I_COUNT) =
                ::qol::features::extension_bag::write_native_bag_object_marker(count_flags);
        }
    }
}
