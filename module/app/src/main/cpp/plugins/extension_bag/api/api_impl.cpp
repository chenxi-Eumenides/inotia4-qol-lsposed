#include "plugins/extension_bag/api/api_impl.h"

#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/response.h"
#include "plugins/extension_bag/runtime/session.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/state_access.h"

namespace {

std::string api_not_ready_error() {
    return extension_bag_op_err(runtime_state().enabled.load() ? "not in game"
                                                              : "extension bag disabled");
}

}  // namespace

std::string extension_bag_api_use_item_impl(int logical_bag, int slot) {
    if (!extension_bag_ready_locked()) return api_not_ready_error();
    const int bag = extension_storage_index(logical_bag);
    if (bag < 0) return extension_bag_op_err("bad extension bag (6-10)");
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return extension_bag_op_err("bad slot");

    bool used = false;
    bool is_seal = false;
    void* item = nullptr;
    void* leader = nullptr;
    bool closes_description = false;
    ModuleUseToken use_token{};
    ModuleUseToken* previous_use_token = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        ::qol::extension_bag::ReentryLock lock;
        if (slot >= ::qol::extension_bag::access::bag().capacities[bag]) {
            return extension_bag_op_err("slot beyond derived capacity");
        }
        const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) {
            return extension_bag_op_err("slot empty");
        }
        // R-63 佣兵徽章（category 42..50 / 928..933）：ITEMDATA_IsUse 不认（+2=0x1d），
        // 效果只在原版 MERCENARYSYSTEM_MakeMercenary，CHAR_UseItemEx 不实现，故豁免
        // fn_is_use 并按原版按钮顺序 SAVE_IsOK→IsEmptyManagerSlot→MakeMercenary。
        is_seal = read_item_is_mercenary_seal(descriptor.category);
        if (is_seal) {
            const auto* save_facts = extension_bag_save_facts();
            if (save_facts == nullptr || save_facts->is_ok == nullptr ||
                ::qol::extension_bag::access::host().character_extras->read.has_empty_mercenary_slot == nullptr ||
                extension_bag_write_api() == nullptr ||
                extension_bag_write_api()->equipment.make_mercenary == nullptr) {
                return extension_bag_op_err("symbol not resolved");
            }
        } else if (::qol::extension_bag::access::host().game == nullptr ||
                   ::qol::extension_bag::access::host().game->read_only.item.is_usable == nullptr ||
                   extension_bag_write_api() == nullptr ||
                   extension_bag_write_api()->equipment.use_item == nullptr) {
            return extension_bag_op_err("symbol not resolved");
        }
        if (!is_seal && !read_item_is_usable(descriptor.category)) {
            return extension_bag_op_err("item not usable");
        }

        item = module_item_locked(bag, slot);
        leader = nullptr;
        if (::qol::extension_bag::access::host().game != nullptr &&
            ::qol::extension_bag::access::host().character_extras->read.party_member != nullptr) {
            (void)::qol::extension_bag::access::host().character_extras->read.party_member(0,
                                                                                           &leader);
        }
        if (item == nullptr) return extension_bag_op_err("extension item unavailable");
        if (!is_seal && leader == nullptr) return extension_bag_op_err("no leader");
        closes_description = qol::extension_bag::ui_state().ui.extension_desc_item == item;
        if (!module_use_begin_locked(bag, slot, item, &use_token)) {
            return extension_bag_op_err("extension item already in use");
        }
    }
    if (closes_description) extension_description_set_off();
    if (is_seal) {
        // SAVE_IsOK/空槽检查不得持背包锁调用；失败先 abort token 再返回明确错误。
        const bool save_ok = read_save_is_ok();
        const bool slot_ok = save_ok && read_has_empty_mercenary_slot();
        if (!save_ok || !slot_ok) {
            ::qol::extension_bag::ReentryLock lock;
            module_use_abort_locked(use_token);
            return extension_bag_op_err(!save_ok ? "save busy" : "mercenary slots full");
        }
        previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        void* mercenary = nullptr;
        used = extension_make_mercenary(item, &mercenary);
        g_active_module_use_token = previous_use_token;
    } else {
        previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        int use_result = 0;
        used = extension_use_item(leader, item, 0, &use_result) && use_result != 0;
        g_active_module_use_token = previous_use_token;
    }
    {
        ::qol::extension_bag::ReentryLock lock;
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG_ERROR("ERROR API use token mismatch bag=%d slot=%d item=%p marker_cleared=%d",
                              bag, slot, item, isolated ? 1 : 0);
            return extension_bag_op_err("extension item changed during native operation");
        }
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            return extension_bag_op_err("extension item release failed");
        }
        ::qol::extension_bag::ReentryLock lock;
        if (!module_use_finalize_release_locked(release_plan)) {
            return extension_bag_op_err("extension item release finalize failed");
        }
    }
    if (!used) return extension_bag_op_err("on cooldown");
    if (closes_description) {
        ::qol::extension_bag::ReentryLock lock;
        reset_extension_desc_item_hooks_locked();
        qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
        qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
        qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
    }
    int32_t current_tutorial_state = -1;
    const auto* world_facts = extension_bag_world_facts();
    if (used && world_facts != nullptr && world_facts->tutorial_state != nullptr &&
        ::qol::extension_bag::access::host().game->world_action.cancel_tutorial != nullptr &&
        qol::contracts::succeeded(world_facts->tutorial_state(&current_tutorial_state)) &&
        current_tutorial_state == 6) {
        (void)::qol::extension_bag::access::host().game->world_action.cancel_tutorial();
    }
    return extension_bag_op_ok();
}
