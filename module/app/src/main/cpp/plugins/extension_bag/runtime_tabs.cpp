#include "plugins/extension_bag/extension_bag_internal.h"

#include "plugins/extension_bag/extension_bag_logging.h"

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
                if (defer_item_free_locked(slot)) {
                    slot = nullptr;
                } else {
                    VIRTBAG_LOG_ERROR("ERROR tab bag item retirement failed bag=%d item=%p",
                                      i, slot);
                }
            }
            continue;
        }
        if (slot != nullptr) {
            int category = -1;
            if (read_item_category(slot, &category) && category == static_cast<int>(type)) continue;
            if (!defer_item_free_locked(slot)) {
                VIRTBAG_LOG_ERROR("ERROR tab bag item replacement deferred free failed bag=%d item=%p type=%u",
                                  i, slot, static_cast<unsigned int>(type));
                continue;
            }
            slot = nullptr;
        }
        slot = extension_create_item(static_cast<int32_t>(type), 0, 0, 0);
        if (slot != nullptr) {
            if (!mark_native_bag_object(slot)) {
                if (defer_item_free_locked(slot)) {
                    slot = nullptr;
                } else {
                    VIRTBAG_LOG_ERROR("ERROR unmarked tab bag item retirement failed bag=%d item=%p type=%u",
                                      i, slot, static_cast<unsigned int>(type));
                }
            }
        }
    }
}
