#include "plugins/extension_bag/api/item_writeback.h"

#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/transaction/transaction.h"

namespace qol::features::extension_bag {
namespace {

using QolResult = ::QolResult;

QolResult invalid_argument() {
    static constexpr char kMessage[] = "invalid item handle";
    return contracts::error(kQolErrInvalidArgument, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult wrong_thread() {
    static constexpr char kMessage[] = "wrong thread";
    return contracts::error(kQolErrBlocked, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult sync_failed() {
    static constexpr char kMessage[] = "item writeback failed";
    return contracts::error(kQolErrInternal, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult commit_item(contracts::QolItemHandle handle) {
    if (handle.slot == 0 || handle.generation == 0) return invalid_argument();
    if (!::qol::extension_bag::extension_bag_on_main_thread()) return wrong_thread();

    ::qol::extension_bag::ReentryLock lock;
    void* item = nullptr;
    // Non-provider handles (原版袋 / detached 产物) 没有扩展袋权威存储，调用即成功无操作。
    if (!extension_bag_private_item_for_handle(handle, &item) || item == nullptr) {
        return contracts::ok();
    }
    int bag = -1;
    int slot = -1;
    if (!module_slot_of_item_locked(item, &bag, &slot)) return contracts::ok();
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        qol::extension_bag::ui_state().objects.module_objects[bag][slot] != item) {
        return contracts::ok();
    }

    bool changed = false;
    if (!sync_item_descriptor_from_object_locked(bag, slot, item, &changed)) {
        return sync_failed();
    }
    if (changed) extension_bag_mark_dirty_locked();
    return contracts::ok();
}

}  // namespace

const contracts::InventoryItemWritebackApi kInventoryItemWritebackApi{&commit_item};

}  // namespace qol::features::extension_bag
