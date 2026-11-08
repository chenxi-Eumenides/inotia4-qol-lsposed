#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/core/tabs.h"

#include "plugins/extension_bag/runtime/logging.h"

int extension_tab_index(void* ctrl) {
    const int visible = virtual_bag::rendered_extension_bag_tabs(
        virtual_bag::active_extension_bags());
    for (int index = 0; index < visible; ++index) {
        if (qol::extension_bag::ui_state().ui.extension_tab_buttons[index] == ctrl) return index;
    }
    return -1;
}

// 扩展标签的已装备背包物品对象与 RefreshBagArea 使用同一 data[0] 语义。
bool defer_item_free_locked(void* item);

void refresh_tab_bag_items_locked() {
    const int visible = virtual_bag::rendered_extension_bag_tabs(
        virtual_bag::active_extension_bags());
    for (int i = 0; i < virtual_bag::kMaxExtensionBags; ++i) {
        const uint8_t type = runtime_state().objects.bag.types[i];
        void*& slot = qol::extension_bag::ui_state().ui.tab_bag_items[i];
        if (type == 0 || i >= visible) {
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
