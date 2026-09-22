#include "game/adapter_ports/craft_port.h"
#include "game/adapter_ports/craft_ui_write_port.h"
#include "game/adapter_ports/custom_recipe_write_port.h"

#include "game/access/attr_range_ui.h"
#include "game/access/craft.h"
#include "game/access/game_access.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, "invalid argument", 16);
}

QolResult not_ready() { return contracts::error(kQolErrNotReady, "not ready", 9); }

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

QolResult read_selected_stuff_item(void** out) {
    if (out == nullptr) return invalid_argument();
    const int64_t index = craft_selected_stuff_index();
    *out = index >= 0 && index < static_cast<int64_t>(kCraftStuffSlotCount)
               ? craft_stuff_item(static_cast<size_t>(index))
               : nullptr;
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_stuff_item(uint32_t index, void** out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_stuff_item(index);
    return *out == nullptr ? not_ready() : contracts::ok();
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

QolResult read_stuff_slot_control(uint32_t index, void** out) {
    if (out == nullptr || index >= kCraftStuffSlotCount) return invalid_argument();
    *out = craft_stuff_slot_control(index);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_target_slot_item(void** out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_target_slot_item();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_selected_inventory_item(void** out) {
    if (out == nullptr) return invalid_argument();
    *out = craft_selected_inven_item();
    return *out == nullptr ? not_ready() : contracts::ok();
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

QolResult call_show_yesno_from_text(uint32_t text_id, void* ok, void* cancel) {
    if (ok == nullptr || cancel == nullptr) return invalid_argument();
    return craft_show_yesno_from_textdata(text_id, ok, cancel) ? contracts::ok() : not_ready();
}

QolResult call_set_target_slot_item(void* item) {
    if (craft_target_slot_control() == nullptr || fn_control_item_set_item == nullptr) {
        return not_ready();
    }
    craft_set_target_slot_item(item);
    return contracts::ok();
}

QolResult call_set_cost_zero() {
    craft_set_cost_zero();
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

QolResult call_set_stuff_item(uint32_t index, void* item) {
    if (index >= kCraftStuffSlotCount) return invalid_argument();
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
     &read_category_held_count, &read_character_level, &read_stuff_slot_control,
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
};

}  // namespace

const contracts::QolCraftPort& craft_port() { return kCraftPort; }

const contracts::QolCraftUiWritePort& craft_ui_write_port() { return kCraftUiWritePort; }

const contracts::QolCustomRecipeWritePort& custom_recipe_write_port() {
    return kCustomRecipeWritePort;
}

}  // namespace qol::game
