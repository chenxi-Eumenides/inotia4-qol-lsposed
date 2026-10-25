#include "plugins/extension_bag/core/ui_state.h"
#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/model/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "plugins/extension_bag/model/serialization.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/interface/bag_query.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/transaction/ownership.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/model/store_sell_rules.h"
#include "plugins/extension_bag/usecases/item.h"
#include "plugins/extension_bag/usecases/move.h"
#include "plugins/extension_bag/usecases/unequip.h"
#include "plugins/extension_bag/usecases/view.h"
#include "plugins/extension_bag/usecases/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>



std::string extension_bag_api_status_json_impl() {
    return qol::features::extension_bag::query_status_json();
}

namespace {

std::string api_not_ready_error() {
    return extension_bag_op_err(runtime_state().enabled.load()
                                     ? "not in game"
                                     : "extension bag disabled");
}

const char* recovery_action_name_api() {
    const virtual_bag::RecoveryAction action =
        virtual_bag::recovery_action(runtime_state().objects.bag,
                                     runtime_state().objects.bag.pending);
    if (action == virtual_bag::RecoveryAction::kComplete) return "complete";
    if (action == virtual_bag::RecoveryAction::kRollback) return "rollback";
    return "none";
}

std::string api_status_json_locked() {
    std::string original_capacities = "[";
    constexpr uint32_t kCapacityMask = (1u << 25) - 1u;
    for (int bag = 0; bag < 5; ++bag) {
        if (bag != 0) original_capacities.push_back(',');
        uint32_t word = 0;
        if (bag_size_word_locked(bag, &word)) {
            original_capacities += std::to_string(word & kCapacityMask);
        } else {
            original_capacities += "-1";
        }
    }
    original_capacities.push_back(']');
    const auto& state = runtime_state();
    return "{\"enabled\":" + std::string(state.enabled.load() ? "true" : "false") +
           ",\"injected\":" + std::string(state.persistence.state_entry != nullptr ? "true" : "false") +
           ",\"save_panel_hook\":" +
            std::string(qol::extension_bag::ui_state().persistence_hooks.save_panel_hook_installed ? "true" : "false") +
            ",\"extension_tab_button\":" +
            std::string(qol::extension_bag::ui_state().ui.extension_tab_buttons[0] != nullptr ? "true" : "false") +
            ",\"inventory_frame_active\":" +
            std::string(qol::extension_bag::ui_state().ui.inventory_frame_active ? "true" : "false") +
           ",\"recovery_action\":\"" + recovery_action_name_api() + "\"" +
           ",\"bagCount\":" + std::to_string(virtual_bag::active_extension_bags()) +
           ",\"maxBagCount\":" + std::to_string(virtual_bag::kMaxExtensionBags) +
           ",\"originalCapacities\":" + original_capacities +
           ",\"state\":" + virtual_bag::state_json(state.objects.bag, true) + "}";
}

std::string api_op_result(qol::features::extension_bag::usecases::OpResult result,
                          bool include_state) {
    if (!result.ok) {
        return extension_bag_op_err(
            qol::features::extension_bag::usecases::error_text(result.error));
    }
    return include_state ? "{\"ok\":true,\"state\":" + api_status_json_locked() + "}"
                         : extension_bag_op_ok();
}

}  // namespace

std::string extension_bag_api_enter_view_impl(int logical_bag) {
    return api_op_result(qol::features::extension_bag::usecases::enter_view(logical_bag), true);
}

std::string extension_bag_api_exit_view_impl() {
    return api_op_result(qol::features::extension_bag::usecases::exit_view(), true);
}

std::string extension_bag_api_select_bag_impl(int logical_bag) {
    return api_op_result(qol::features::extension_bag::usecases::select_bag(logical_bag), true);
}

std::string extension_bag_api_click_item_impl(int logical_bag, int slot) {
    const auto result = qol::features::extension_bag::usecases::click_item(logical_bag, slot);
    if (!result.ok) {
        return extension_bag_op_err(
            qol::features::extension_bag::usecases::error_text(result.error));
    }
    if (!result.has_item) return "{\"ok\":true,\"item\":null}";
    return "{\"ok\":true,\"item\":{\"bag\":" + std::to_string(result.logical_bag) +
           ",\"slot\":" + std::to_string(result.slot) +
           ",\"category\":" + std::to_string(result.item.category) +
           ",\"count\":" + std::to_string(result.effective_count) +
           ",\"payload\":\"" + virtual_bag::base64_encode(
               result.item.payload.data(), result.item.payload_size) + "\"}}";
}

std::string extension_bag_api_unequip_impl(int logical_bag) {
    return api_op_result(qol::features::extension_bag::usecases::unequip_bag(logical_bag), false);
}

std::string extension_bag_api_move_item_impl(int from_bag, int from_slot, int to_bag, int to_slot) {
    const auto result = qol::features::extension_bag::usecases::move_item(
        from_bag, from_slot, to_bag, to_slot);
    if (!result.ok) {
        return extension_bag_op_err(
            qol::features::extension_bag::usecases::error_text(result.error));
    }
    return "{\"ok\":true,\"state\":" + api_status_json_locked() + "}";
}

namespace {
void refresh_extension_item_operation_locked(int bag) {
    if (qol::extension_bag::ui_state().objects.module_view_installed && qol::extension_bag::ui_state().objects.module_view_index == bag) {
        refresh_module_item_area_locked(bag);
    }
}

}  // namespace

std::string extension_bag_api_use_item_impl(int logical_bag, int slot) {
    if (!extension_bag_ready_locked()) return api_not_ready_error();
    const int bag = extension_internal_bag(logical_bag);
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
        if (slot >= ::qol::extension_bag::access::bag().capacities[bag]) return extension_bag_op_err("slot beyond derived capacity");
        const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) return extension_bag_op_err("slot empty");
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
        } else if (::qol::extension_bag::access::host().game == nullptr || ::qol::extension_bag::access::host().game->read_only.item.is_usable == nullptr ||
                   extension_bag_write_api() == nullptr ||
                   extension_bag_write_api()->equipment.use_item == nullptr) {
            return extension_bag_op_err("symbol not resolved");
        }
        if (!is_seal && !read_item_is_usable(descriptor.category)) {
            return extension_bag_op_err("item not usable");
        }

        item = module_item_locked(bag, slot);
        leader = nullptr;
        if (::qol::extension_bag::access::host().game != nullptr && ::qol::extension_bag::access::host().character_extras->read.party_member != nullptr) {
            (void)::qol::extension_bag::access::host().character_extras->read.party_member(0, &leader);
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

QolResult extension_put_jewel_error(const char* message) {
    return qol::contracts::error(kQolErrInternal, message,
                                 static_cast<uint32_t>(std::strlen(message)));
}

QolResult extension_bag_api_put_jewel_typed(int role, int logical_bag, int slot,
                                            int equip_slot) {
    if (!extension_bag_ready_locked()) {
        return extension_put_jewel_error(!::qol::extension_bag::access::enabled().load()
                                             ? "extension bag disabled"
                                             : "not in game");
    }
    const int bag = extension_internal_bag(logical_bag);
    if (bag < 0) return extension_put_jewel_error("bad extension bag (6-10)");
    if (slot < 0 || slot >= virtual_bag::kSlotCount) return extension_put_jewel_error("bad slot");
    if (equip_slot < 0 || equip_slot >= kEquipSlotCount) {
        return extension_put_jewel_error("bad equip slot");
    }

    void* jewel = nullptr;
    void* equip = nullptr;
    int before_count = 0;
    ModuleUseToken use_token{};
    ModuleUseToken* previous_use_token = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        ::qol::extension_bag::ReentryLock lock;
        if (slot >= ::qol::extension_bag::access::bag().capacities[bag]) {
            return extension_put_jewel_error("slot beyond derived capacity");
        }
        const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) {
            return extension_put_jewel_error("slot empty");
        }
        if (::qol::extension_bag::access::host().item_facts == nullptr || ::qol::extension_bag::access::host().item_facts->is_jewel_category == nullptr ||
            ::qol::extension_bag::access::host().character_extras->read.party_member == nullptr ||
            ::qol::extension_bag::access::host().character_extras->read.equipped_item == nullptr ||
            ::qol::extension_bag::access::host().write_port->equipment.put_jewel == nullptr) {
            return extension_put_jewel_error("symbol not resolved");
        }
        int32_t is_jewel = 0;
        if (!qol::contracts::succeeded(
                ::qol::extension_bag::access::host().item_facts->is_jewel_category(descriptor.category, &is_jewel)) ||
            is_jewel == 0) return extension_put_jewel_error("not jewel");

        void* ch = nullptr;
        (void)::qol::extension_bag::access::host().character_extras->read.party_member(role, &ch);
        if (ch == nullptr) return extension_put_jewel_error("role not found");
        jewel = module_item_locked(bag, slot);
        if (!qol::contracts::succeeded(
                ::qol::extension_bag::access::host().character_extras->read.equipped_item(ch, equip_slot, &equip))) {
            return extension_put_jewel_error("equip slot unavailable");
        }
        if (jewel == nullptr) return extension_put_jewel_error("extension jewel unavailable");
        if (equip == nullptr) return extension_put_jewel_error("equip slot empty");

        // 消耗前数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
        before_count = static_cast<int>(virtual_bag_effective_count(jewel, static_cast<uint32_t>(descriptor.count)));
        if (!module_use_begin_locked(bag, slot, jewel, &use_token)) {
            return extension_put_jewel_error("extension jewel already in use");
        }
    }

    previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    int32_t result = -1;
    const bool put_jewel_succeeded =
        ::qol::extension_bag::access::host().game != nullptr &&
        qol::contracts::succeeded(
            ::qol::extension_bag::access::host().write_port->equipment.put_jewel(equip, jewel, &result));
    g_active_module_use_token = previous_use_token;
    if (!put_jewel_succeeded) {
        ::qol::extension_bag::ReentryLock lock;
        (void)module_use_abort_locked(use_token);
        return extension_put_jewel_error("put jewel unavailable");
    }

    {
        ::qol::extension_bag::ReentryLock lock;
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG_ERROR("ERROR API put jewel token mismatch bag=%d slot=%d item=%p marker_cleared=%d",
                        bag, slot, jewel, isolated ? 1 : 0);
            return extension_put_jewel_error("extension jewel changed during native operation");
        }
        if (result != 0) {
            return extension_put_jewel_error(result == 2 ? "no socket" : "not jewel");
        }
        if (!consumed) {
            int current_bag = -1;
            int current_slot = -1;
            if (!module_slot_of_item_locked(jewel, &current_bag, &current_slot) ||
                current_bag != bag || current_slot != slot ||
                qol::extension_bag::ui_state().objects.module_objects[bag][slot] != jewel) {
                return extension_put_jewel_error("extension jewel changed during native operation");
            }
            if (!consume_extension_item_after_native_locked(
                    bag, slot, jewel, before_count, before_count, "api_put_jewel", nullptr)) {
                return extension_put_jewel_error("extension jewel state update failed");
            }
        }
        extension_bag_mark_dirty_locked();
        refresh_extension_item_operation_locked(bag);
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            return extension_put_jewel_error("extension jewel release failed");
        }
        ::qol::extension_bag::ReentryLock lock;
        if (!module_use_finalize_release_locked(release_plan)) {
            return extension_put_jewel_error("extension jewel release finalize failed");
        }
    }
    return qol::contracts::ok();
}

std::string extension_bag_api_put_jewel_impl(int role, int logical_bag, int slot,
                                             int equip_slot) {
    const QolResult result =
        extension_bag_api_put_jewel_typed(role, logical_bag, slot, equip_slot);
    return qol::contracts::succeeded(result) ? extension_bag_op_ok() : extension_bag_op_err(result.message);
}
