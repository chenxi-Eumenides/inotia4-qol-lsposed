#include "game/adapter_ports/craft_port.h"
#include "game/adapter_ports/craft_ui_write_port.h"
#include "game/adapter_ports/custom_recipe_write_port.h"

#include <climits>

#include "game/access/attr_range_ui.h"
#include "game/access/craft.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/game_host_port.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, "invalid argument", 16);
}

QolResult not_ready() { return contracts::error(kQolErrNotReady, "not ready", 9); }

QolResult not_found() { return contracts::error(kQolErrNotFound, "not found", 9); }

QolResult wrong_thread() { return contracts::error(kQolErrBlocked, "wrong thread", 12); }

QolResult handle_for_craft_item(void* item, contracts::QolItemHandle* out_handle) {
    if (item == nullptr || out_handle == nullptr) return invalid_argument();
    *out_handle = contracts::kInvalidItemHandle;
    if (!game_host_is_main_thread()) return wrong_thread();
    uint32_t owner = contracts::kQolItemHandleOwnerAny;
    const contracts::QolItemHandle registered = lookup_item(item, &owner);
    if (registered.slot != 0 && registered.generation != 0) {
        if (owner != contracts::kQolItemHandleOwnerGame &&
            owner != contracts::kQolItemHandleOwnerProvider) {
            return contracts::error(kQolErrConflict, "craft item owner unavailable", 29);
        }
        *out_handle = registered;
        return contracts::ok();
    }

    // An unregistered control item is registered as Game-owned only after a physical slot match.
    for (int32_t bag = 0; bag < 6; ++bag) {
        for (int32_t slot = 0; slot < 16; ++slot) {
            void* physical_item = nullptr;
            if (!contracts::succeeded(physical_item_at(bag, slot, &physical_item)) ||
                physical_item != item) continue;
            ItemHandleError error = ItemHandleError::kNone;
            const contracts::QolItemHandle handle = register_item(
                item, contracts::kQolItemHandleOwnerGame, &error);
            if (handle.slot == 0 || handle.generation == 0) return not_ready();
            *out_handle = handle;
            return contracts::ok();
        }
    }
    return not_ready();
}

QolResult resolve_craft_item(contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) {
        return invalid_argument();
    }
    *out_item = nullptr;
    const ItemHandleResolution resolved = item_handle_registry().resolve_item(
        handle, contracts::kQolItemHandleOwnerAny);
    if (resolved.error == ItemHandleError::kWrongThread) {
        return contracts::error(kQolErrBlocked, "wrong thread", 12);
    }
    if (resolved.error == ItemHandleError::kType) {
        return contracts::error(kQolErrConflict, "craft item owner mismatch", 25);
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) return not_found();
    *out_item = resolved.item;
    return contracts::ok();
}

QolResult resolve_craft_game_item(contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) {
        return invalid_argument();
    }
    *out_item = nullptr;
    const ItemHandleResolution resolved = item_handle_registry().resolve_item(
        handle, contracts::kQolItemHandleOwnerGame);
    if (resolved.error == ItemHandleError::kWrongThread) return wrong_thread();
    if (resolved.error == ItemHandleError::kType) {
        return contracts::error(kQolErrConflict, "craft item is not game-owned", 29);
    }
    if (resolved.error != ItemHandleError::kNone || resolved.item == nullptr) return not_found();
    *out_item = resolved.item;
    return contracts::ok();
}

QolResult created_item_handle(void* item, contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return invalid_argument();
    *out_item = contracts::kInvalidItemHandle;
    if (item == nullptr) return not_ready();
    ItemHandleError error = ItemHandleError::kNone;
    const contracts::QolItemHandle handle = register_item(
        item, contracts::kQolItemHandleOwnerGame, &error);
    if (handle.slot == 0 || handle.generation == 0) {
        if (fn_itempool_free != nullptr) fn_itempool_free(item);
        return not_ready();
    }
    *out_item = handle;
    return contracts::ok();
}

QolResult read_item_handle(void* item, contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return invalid_argument();
    *out_item = contracts::kInvalidItemHandle;
    if (item == nullptr) return not_ready();
    return handle_for_craft_item(item, out_item);
}

QolResult read_ui_type(uint32_t* out) {
    if (out == nullptr) return invalid_argument();
    const int64_t value = craft_ui_type();
    if (value < 0) return not_ready();
    *out = static_cast<uint32_t>(value);
    return contracts::ok();
}

QolResult read_get_ui_type(uint32_t* out) {
    if (out == nullptr) return invalid_argument();
    const int64_t value = craft_get_ui_type();
    if (value < 0) return not_ready();
    *out = static_cast<uint32_t>(value);
    return contracts::ok();
}

QolResult read_panel_state(int32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = static_cast<int32_t>(craft_panel_state());
    return contracts::ok();
}

QolResult read_mix_type(uint32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_mix_type();
    return contracts::ok();
}

QolResult read_selected_stuff_index(int64_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_selected_stuff_index();
    return contracts::ok();
}

QolResult read_selected_stuff_item(contracts::QolItemHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = contracts::kInvalidItemHandle;
    const int64_t index = craft_selected_stuff_index();
    if (index < 0 || index >= static_cast<int64_t>(kCraftStuffSlotCount)) return not_ready();
    return read_item_handle(craft_stuff_item(static_cast<size_t>(index)), out);
}

// 严格身份语义：控件存在但槽内无物品是正常空槽 → OK + 无效 handle；控件缺失 → NotReady；
// 非空对象仍走登记/resolve，保留原错误。不得把 read_item_handle(nullptr) 整体改成 OK：
// selected/target 等调用点的 null 表示未就绪。
QolResult read_stuff_item(uint32_t index, contracts::QolItemHandle* out) {
    if (out == nullptr || index >= kCraftStuffSlotCount) return invalid_argument();
    *out = contracts::kInvalidItemHandle;
    if (!game_host_is_main_thread()) return wrong_thread();
    if (craft_stuff_slot_control(index) == nullptr) return not_ready();
    void* item = craft_stuff_item(index);
    if (item == nullptr) return contracts::ok();
    return handle_for_craft_item(item, out);
}

QolResult read_category_held_count(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) return invalid_argument();
    *out = craft_category_held_count(static_cast<uint16_t>(category));
    return contracts::ok();
}

QolResult read_character_level(int32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_character_level();
    return contracts::ok();
}

QolResult read_stuff_slot_available(uint32_t index, int32_t* out_available) {
    if (out_available == nullptr || index >= kCraftStuffSlotCount) return invalid_argument();
    if (!game_host_is_main_thread()) return wrong_thread();
    *out_available = craft_stuff_slot_control(index) != nullptr ? 1 : 0;
    return *out_available != 0 ? contracts::ok() : not_ready();
}

QolResult read_target_slot_item(contracts::QolItemHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = contracts::kInvalidItemHandle;
    return read_item_handle(craft_target_slot_item(), out);
}

QolResult read_selected_inventory_item(contracts::QolItemHandle* out) {
    if (out == nullptr) return invalid_argument();
    *out = contracts::kInvalidItemHandle;
    return read_item_handle(craft_selected_inven_item(), out);
}

QolResult read_selected_recipe(int32_t type, int64_t* out) {
    if (type < 0 || type > 4 || out == nullptr) return invalid_argument();
    *out = craft_selected_recipe_at(type);
    return contracts::ok();
}

QolResult read_stuff_slot_count(uint32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_stuff_slot_count();
    return contracts::ok();
}

QolResult call_init_mixing_state() {
    if (fn_uimix_init_mixing_state == nullptr) return not_ready();
    craft_init_mixing_state();
    return contracts::ok();
}

QolResult call_reset_stuff_item_control() {
    if (fn_uimix_reset_stuff_item_control == nullptr) return not_ready();
    craft_reset_stuff_item_control();
    return contracts::ok();
}

QolResult call_refresh_inventory_items() {
    if (fn_ui_mix_refresh_inven_item == nullptr) return not_ready();
    craft_refresh_inven_items();
    return contracts::ok();
}

QolResult call_select_first_empty_stuff_slot(int32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_select_first_empty_stuff_slot();
    return contracts::ok();
}

QolResult call_set_ui_type(int64_t type) {
    if (type < 0 || type > 4) return invalid_argument();
    if (fn_uimix_set_type == nullptr) return not_ready();
    craft_set_ui_type(type);
    return contracts::ok();
}

QolResult call_set_selected_recipe(int32_t type, int64_t recipe) {
    if (type < 0 || type > 4 || recipe < -1) return invalid_argument();
    if (craft_ui_type() < 0) return not_ready();
    craft_set_selected_recipe_at(type, recipe);
    return contracts::ok();
}

QolResult call_play_sound(int32_t sound_id) {
    if (sound_id < INT16_MIN || sound_id > INT16_MAX) return invalid_argument();
    if (fn_sound_system_play == nullptr) return not_ready();
    craft_play_sound(static_cast<int16_t>(sound_id));
    return contracts::ok();
}

QolResult call_show_text(uint32_t text_id) {
    if (fn_popup_create_ok_from_textdata == nullptr) return not_ready();
    craft_show_text(text_id);
    return contracts::ok();
}

QolResult call_show_yesno_from_text(uint32_t text_id, contracts::QolCraftPopupCallback ok,
                                    contracts::QolCraftPopupCallback cancel) {
    if (ok == nullptr || cancel == nullptr) return invalid_argument();
    return craft_show_yesno_from_textdata(text_id, reinterpret_cast<void*>(ok),
                                          reinterpret_cast<void*>(cancel))
               ? contracts::ok()
               : not_ready();
}

QolResult call_set_target_slot_item(contracts::QolItemHandle handle) {
    if (craft_target_slot_control() == nullptr || fn_control_item_set_item == nullptr) {
        return not_ready();
    }
    void* item = nullptr;
    if (handle.slot != 0 || handle.generation != 0) {
        const QolResult resolved = resolve_craft_item(handle, &item);
        if (!contracts::succeeded(resolved)) return resolved;
    }
    craft_set_target_slot_item(item);
    return contracts::ok();
}

QolResult call_set_cost_zero() {
    craft_set_cost_zero();
    return contracts::ok();
}

QolResult call_create_perfect_item(int32_t category, contracts::QolItemHandle* out_item) {
    if (category < 0 || out_item == nullptr) return invalid_argument();
    *out_item = contracts::kInvalidItemHandle;
    if (!game_host_is_main_thread()) return wrong_thread();
    if (fn_item_create_perfect_item == nullptr) return not_ready();
    return created_item_handle(fn_item_create_perfect_item(category), out_item);
}

QolResult call_create_item(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                           contracts::QolItemHandle* out_item) {
    if (category < 0 || out_item == nullptr) return invalid_argument();
    *out_item = contracts::kInvalidItemHandle;
    if (!game_host_is_main_thread()) return wrong_thread();
    if (fn_create_item == nullptr) return not_ready();
    return created_item_handle(fn_create_item(category, arg1, arg2, arg3), out_item);
}

QolResult call_save_item(contracts::QolItemHandle handle, int32_t* out_saved) {
    if (out_saved == nullptr) return invalid_argument();
    void* item = nullptr;
    const QolResult resolved = resolve_craft_item(handle, &item);
    if (!contracts::succeeded(resolved)) return resolved;
    uint32_t owner = contracts::kQolItemHandleOwnerAny;
    if (lookup_item(item, &owner).slot == 0 || owner != contracts::kQolItemHandleOwnerGame) {
        return contracts::error(kQolErrConflict, "craft product is not game-owned", 32);
    }
    if (fn_inven_save_item == nullptr) return not_ready();
    *out_saved = fn_inven_save_item(item, nullptr);
    return contracts::ok();
}

QolResult call_free_item(contracts::QolItemHandle handle) {
    if (fn_itempool_free == nullptr) return not_ready();
    void* item = nullptr;
    const QolResult resolved = resolve_craft_item(handle, &item);
    if (!contracts::succeeded(resolved)) return resolved;
    uint32_t owner = contracts::kQolItemHandleOwnerAny;
    if (lookup_item(item, &owner).slot == 0 || owner != contracts::kQolItemHandleOwnerGame) {
        return contracts::error(kQolErrConflict, "craft product is not game-owned", 32);
    }
    if (!invalidate_item(handle)) return not_ready();
    fn_itempool_free(item);
    return contracts::ok();
}

QolResult call_read_jewel_word(contracts::QolItemHandle handle, uint32_t* out_word) {
    if (out_word == nullptr) return invalid_argument();
    void* item = nullptr;
    const QolResult resolved = resolve_craft_item(handle, &item);
    if (!contracts::succeeded(resolved)) return resolved;
    auto* word = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(item) + I_COUNT);
    if (!game_memory_accessible(word, sizeof(*word), 'r')) return not_ready();
    *out_word = *word;
    return contracts::ok();
}

QolResult call_write_jewel_word(contracts::QolItemHandle handle, uint32_t value) {
    void* item = nullptr;
    const QolResult resolved = resolve_craft_game_item(handle, &item);
    if (!contracts::succeeded(resolved)) return resolved;
    auto* word = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(item) + I_COUNT);
    if (!game_memory_accessible(word, sizeof(*word), 'w')) return not_ready();
    *word = value;
    return contracts::ok();
}

QolResult call_set_max_socket_enchant(contracts::QolItemHandle handle) {
    void* item = nullptr;
    const QolResult resolved = resolve_craft_game_item(handle, &item);
    if (!contracts::succeeded(resolved)) return resolved;
    auto* bytes = static_cast<uint8_t*>(item);
    uint8_t* socket = bytes + I_SOCKET;
    uint16_t* enchant = reinterpret_cast<uint16_t*>(bytes + I_ENCHANT);
    if (!game_memory_accessible(socket, sizeof(*socket), 'r') ||
        !game_memory_accessible(enchant, sizeof(*enchant), 'r')) return not_ready();
    const uint8_t socket_value = static_cast<uint8_t>((*socket & 0x0Fu) | (0x0Fu << 4));
    const uint16_t enchant_value = static_cast<uint16_t>((*enchant & ~(0x0Fu << 2)) |
                                                         (0x0Fu << 2));
    if (!game_memory_accessible(socket, sizeof(*socket), 'w') ||
        !game_memory_accessible(enchant, sizeof(*enchant), 'w')) return not_ready();
    *socket = socket_value;
    *enchant = enchant_value;
    return contracts::ok();
}

QolResult call_random_inclusive(int32_t lo, int32_t hi, int32_t* out) {
    if (out == nullptr) return invalid_argument();
    *out = 0;
    if (!game_host_is_main_thread()) return wrong_thread();
    if (lo > hi) return invalid_argument();
    if (fn_math_get_random == nullptr) return not_ready();

    const int64_t span = static_cast<int64_t>(hi) - lo;
    if (span > INT_MAX) {
        static constexpr char kSpanTooWide[] = "random range span exceeds INT_MAX";
        return contracts::error(kQolErrInvalidArgument, kSpanTooWide,
                                sizeof(kSpanTooWide) - 1);
    }
    const int value = fn_math_get_random(static_cast<int>(lo), static_cast<int>(hi));
    if (value < lo || value > hi) {
        static constexpr char kRandomOutOfRange[] = "random result out of range";
        return contracts::error(kQolErrInternal, kRandomOutOfRange,
                                sizeof(kRandomOutOfRange) - 1);
    }
    *out = static_cast<int32_t>(value);
    return contracts::ok();
}

QolResult call_write_stuff_need(uint32_t index, int32_t category, int32_t count) {
    if (category < 0 || count < 0 || count > UINT16_MAX) return invalid_argument();
    void* item = craft_stuff_item(index);
    if (item != nullptr && attr_item_category(item) != category) {
        return contracts::error(kQolErrConflict, "category mismatch", 17);
    }
    craft_write_stuff_need(index, static_cast<uint16_t>(count));
    return contracts::ok();
}

QolResult call_set_stuff_item(uint32_t index, contracts::QolItemHandle handle) {
    if (index >= kCraftStuffSlotCount) return invalid_argument();
    if (craft_stuff_slot_control(index) == nullptr || fn_control_item_set_item == nullptr) {
        return not_ready();
    }
    void* item = nullptr;
    if (handle.slot != 0 || handle.generation != 0) {
        const QolResult resolved = resolve_craft_item(handle, &item);
        if (!contracts::succeeded(resolved)) return resolved;
    }
    craft_set_stuff_item(index, item);
    return contracts::ok();
}

QolResult call_description_set_off() {
    if (fn_ui_desc_set_off == nullptr) return not_ready();
    craft_desc_set_off();
    return contracts::ok();
}

const contracts::QolCraftPort kCraftPort{
    contracts::kQolPortCraftVersion,
    {&read_ui_type, &read_panel_state, &read_mix_type, &read_selected_stuff_index,
     &read_selected_stuff_item, &read_stuff_item, &read_category_held_count, &read_character_level},
     {&read_ui_type, &read_get_ui_type, &read_panel_state, &read_mix_type,
      &read_selected_stuff_index, &read_selected_stuff_item, &read_stuff_item,
      &read_category_held_count, &read_character_level, &read_stuff_slot_available,
      &read_target_slot_item, &read_selected_inventory_item, &read_selected_recipe,
     &read_stuff_slot_count},
};

const contracts::QolCraftUiWritePort kCraftUiWritePort{
    contracts::kQolPortCraftUiWriteVersion,
    &call_init_mixing_state,
    &call_reset_stuff_item_control,
    &call_refresh_inventory_items,
    &call_select_first_empty_stuff_slot,
    &call_set_ui_type,
    &call_set_selected_recipe,
    &call_play_sound,
    &call_show_text,
    &call_show_yesno_from_text,
    &call_set_target_slot_item,
    &call_set_cost_zero,
};

const contracts::QolCustomRecipeWritePort kCustomRecipeWritePort{
    contracts::kQolPortCustomRecipeWriteVersion,
    &call_write_stuff_need,
    &call_set_stuff_item,
    &call_description_set_off,
    &call_create_perfect_item,
    &call_create_item,
    &call_save_item,
    &call_free_item,
    &call_read_jewel_word,
    &call_write_jewel_word,
    &call_set_max_socket_enchant,
    &call_random_inclusive,
};

QolResult craft_stuff_slot_index_impl(void* transient_control, int32_t* out_index) {
    if (transient_control == nullptr || out_index == nullptr) return invalid_argument();
    *out_index = -1;
    if (!game_host_is_main_thread()) return wrong_thread();
    // Compare against the current panel children on every call; no native control address is cached.
    if (craft_panel_state() != 1) return not_found();
    for (uint32_t index = 0; index < kCraftStuffSlotCount; ++index) {
        const void* current = craft_stuff_slot_control(index);
        if (current != nullptr && current == transient_control) {
            *out_index = static_cast<int32_t>(index);
            return contracts::ok();
        }
    }
    return not_found();
}

}  // namespace

const contracts::QolCraftPort& craft_port() { return kCraftPort; }

const contracts::QolCraftUiWritePort& craft_ui_write_port() { return kCraftUiWritePort; }

const contracts::QolCustomRecipeWritePort& custom_recipe_write_port() {
    return kCustomRecipeWritePort;
}

QolResult craft_stuff_slot_index(void* transient_control, int32_t* out_index) {
    return craft_stuff_slot_index_impl(transient_control, out_index);
}

}  // namespace qol::game
