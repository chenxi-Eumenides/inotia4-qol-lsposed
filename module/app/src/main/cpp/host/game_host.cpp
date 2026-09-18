#include "host/adapters.h"

#include <atomic>
#include <cstring>
#include <pthread.h>

#include "game_access.h"
#include "game/access/attr_range_ui.h"
#include "game/access/char.h"
#include "game/access/craft.h"
#include "game/access/item_class.h"
#include "game/access/quest.h"
#include "game/access/save.h"
#include "game/access/symbols.h"
#include "game/access/ui_choice.h"
#include "game/access/world.h"
#include "game/clock.h"
#include "core/native/game_variant.h"

namespace qol::host {
namespace {

using QolResult = ::QolResult;

constexpr const char* kInvalidArgument = "invalid argument";
constexpr const char* kNotReady = "not ready";
constexpr const char* kUnsupported = "unsupported";

pthread_t g_main_thread{};
std::atomic<bool> g_main_thread_set{false};

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, kNotReady); }
QolResult unsupported() { return error_result(kQolErrUnsupported, kUnsupported); }

QolResult game_query_variant(contracts::QolTargetVariant* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game_variant_init()) return not_ready();
    const qol::GameVariant& variant = qol::game_variant();
    out->id = static_cast<uint32_t>(variant.series);
    out->capabilities = variant.capabilities;
    return contracts::ok();
}

int32_t game_has_capability(uint64_t bit) {
    if (!qol::game_variant_init() || bit > UINT32_MAX) return 0;
    return (qol::game_variant().capabilities & static_cast<uint32_t>(bit)) != 0 ? 1 : 0;
}

int32_t game_symbol_available(const char* symbol_id) {
    if (symbol_id == nullptr) return 0;
    bool followed = false;
    return qol::game::resolve_hook_entry(symbol_id, &followed) != 0 ? 1 : 0;
}

int32_t game_is_main_thread() {
    if (!g_main_thread_set.load(std::memory_order_acquire)) return 0;
    return pthread_equal(g_main_thread, pthread_self()) != 0 ? 1 : 0;
}

int64_t game_frame_count() { return qol::game::frame_count(); }

QolResult read_char_type(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::char_type(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_char_active(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::char_active_group_resolved()) return not_ready();
    *out = qol::game::char_is_active_player_group(const_cast<void*>(character)) ? 1 : 0;
    return contracts::ok();
}

QolResult read_char_party(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::char_party_index_resolved()) return not_ready();
    const int value = qol::game::char_party_index(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_category(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::attr_item_category(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_item_jewel(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::attr_is_jewel_category(category) ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_ability(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::attr_ability_level(item);
    return contracts::ok();
}

QolResult read_world_position(const void*, int32_t*, int32_t*) { return unsupported(); }

QolResult read_world_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::world_state();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_craft_ui_type(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int64_t value = qol::game::craft_ui_type();
    if (value < 0) return not_ready();
    *out = static_cast<uint32_t>(value);
    return contracts::ok();
}

QolResult read_craft_panel_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = static_cast<int32_t>(qol::game::craft_panel_state());
    return contracts::ok();
}

QolResult read_craft_mix_type(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_mix_type();
    return contracts::ok();
}

QolResult read_craft_selected_index(int64_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_selected_stuff_index();
    return contracts::ok();
}

QolResult read_craft_selected_item(void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int64_t index = qol::game::craft_selected_stuff_index();
    *out = index >= 0 && index < static_cast<int64_t>(qol::game::kCraftStuffSlotCount)
               ? qol::game::craft_stuff_item(static_cast<uint32_t>(index))
               : nullptr;
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_craft_stuff_item(uint32_t index, void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_stuff_item(index);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_craft_category_count(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_category_held_count(static_cast<uint16_t>(category));
    return contracts::ok();
}

QolResult read_craft_character_level(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_character_level();
    return contracts::ok();
}

QolResult read_save_slot_state(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    void* save = qol::game::save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = qol::game::save_slot_exists(save) ? 1 : 0;
    return contracts::ok();
}

QolResult read_quest_state(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return unsupported();
}

QolResult read_char_party_member(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_faction_deps_ready()) return not_ready();
    *out = qol::game::char_is_party_member(const_cast<void*>(character)) ? 1 : 0;
    return contracts::ok();
}

QolResult read_char_summoner(const void* character, void** out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_summoner_resolved()) return not_ready();
    *out = qol::game::char_get_summoner(const_cast<void*>(character));
    return contracts::ok();
}

QolResult read_char_level(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::char_level(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_char_merc_slot(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    const int value = qol::game::char_merc_slot(character);
    *out = value;
    return contracts::ok();
}

QolResult read_char_can_change_equip(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::char_can_change_equip_resolved()) return not_ready();
    *out = qol::game::char_can_change_equip(character) ? 1 : 0;
    return contracts::ok();
}

QolResult read_char_menu_character(void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::char_menu_character_resolved()) return not_ready();
    *out = qol::game::char_menu_character();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_char_pool_slot(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    int slot = 0;
    if (!qol::game::char_pool_slot(character, &slot)) return not_ready();
    *out = slot;
    return contracts::ok();
}

QolResult read_char_max_hp(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_read_max_hp(character, out) ? contracts::ok() : not_ready();
}

QolResult read_char_hp(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_read_hp(character, out) ? contracts::ok() : not_ready();
}

QolResult write_char_max_hp(void* character, int32_t value) {
    if (character == nullptr || value < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_write_max_hp(character, value) ? contracts::ok() : not_ready();
}

QolResult write_char_hp(void* character, int32_t value) {
    if (character == nullptr || value < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::char_write_hp(character, value) ? contracts::ok() : not_ready();
}

QolResult read_item_rarity(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_get_rarity == nullptr) return not_ready();
    *out = fn_get_rarity(const_cast<void*>(item));
    return contracts::ok();
}

QolResult read_item_real_equip(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::item_equip_primitives_ready()) return not_ready();
    *out = qol::game::item_is_real_equip(item) ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_equip_level(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::item_equip_primitives_ready()) return not_ready();
    *out = qol::game::item_equip_level(item);
    return contracts::ok();
}

QolResult read_item_no_sell(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_item_is_no_sell == nullptr) return not_ready();
    *out = fn_item_is_no_sell(category) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_dice(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_is_dice == nullptr) return not_ready();
    *out = fn_is_dice(category) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_cumulate_count(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_get_cumulate_count == nullptr) return not_ready();
    *out = fn_get_cumulate_count(const_cast<void*>(item));
    return contracts::ok();
}

QolResult read_item_class_no_equip_bit(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    void* record = qol::game::item_class_record(static_cast<uint16_t>(category));
    if (record == nullptr) return not_ready();
    *out = qol::game::item_class_no_equip_bit(record) ? 1 : 0;
    return contracts::ok();
}

QolResult read_item_equip_desc_role_type(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::equip_desc_role_type();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_inventory_bit_range(int32_t value, int32_t high_bit, int32_t low_bit,
                                   int32_t* out) {
    if (out == nullptr || high_bit < 0 || high_bit > 31 || low_bit < 0 || low_bit > high_bit) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_get_bit == nullptr) return not_ready();
    *out = fn_get_bit(value, high_bit, low_bit);
    return contracts::ok();
}

QolResult write_inventory_remove_item_data(int32_t category, int32_t count) {
    if (category < 0 || count < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_remove_item_data == nullptr) return not_ready();
    fn_inven_remove_item_data(category, count);
    return contracts::ok();
}

QolResult write_inventory_remove_item(void* item) {
    if (item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_remove_item == nullptr) return not_ready();
    return fn_remove_item(item) != 0 ? contracts::ok() : not_ready();
}

QolResult write_inventory_create_perfect_item(int32_t category, void** out_item) {
    if (category < 0 || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (fn_item_create_perfect_item == nullptr) return not_ready();
    *out_item = fn_item_create_perfect_item(category);
    return *out_item == nullptr ? not_ready() : contracts::ok();
}

QolResult write_inventory_create_item(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                                      void** out_item) {
    if (category < 0 || out_item == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_item = nullptr;
    if (fn_create_item == nullptr) return not_ready();
    *out_item = fn_create_item(category, arg1, arg2, arg3);
    return *out_item == nullptr ? not_ready() : contracts::ok();
}

QolResult write_inventory_save_item(void* item, void* param, int32_t* out_saved) {
    if (item == nullptr || out_saved == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_inven_save_item == nullptr) return not_ready();
    *out_saved = fn_inven_save_item(item, param);
    return contracts::ok();
}

QolResult write_inventory_free_item(void* item) {
    if (item == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_itempool_free == nullptr) return not_ready();
    fn_itempool_free(item);
    return contracts::ok();
}

QolResult read_craft_slot_control(uint32_t index, void** out) {
    if (out == nullptr || index >= qol::game::kCraftStuffSlotCount) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = qol::game::craft_stuff_slot_control(index);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_craft_target_item(void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_target_slot_item();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_craft_selected_inventory_item(void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_selected_inven_item();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_craft_selected_recipe(int32_t type, int64_t* out) {
    if (type < 0 || type > 4 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = qol::game::craft_selected_recipe_at(type);
    return contracts::ok();
}

QolResult read_craft_slot_count(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_stuff_slot_count();
    return contracts::ok();
}

QolResult call_craft_init() {
    if (fn_uimix_init_mixing_state == nullptr) return not_ready();
    qol::game::craft_init_mixing_state();
    return contracts::ok();
}

QolResult call_craft_reset() {
    if (fn_uimix_reset_stuff_item_control == nullptr) return not_ready();
    qol::game::craft_reset_stuff_item_control();
    return contracts::ok();
}

QolResult call_craft_refresh() {
    if (fn_ui_mix_refresh_inven_item == nullptr) return not_ready();
    qol::game::craft_refresh_inven_items();
    return contracts::ok();
}

QolResult call_craft_select_first_empty(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::craft_select_first_empty_stuff_slot();
    return contracts::ok();
}

QolResult call_craft_set_type(int64_t type) {
    if (type < 0 || type > 4) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (fn_uimix_set_type == nullptr) return not_ready();
    qol::game::craft_set_ui_type(type);
    return contracts::ok();
}

QolResult call_craft_set_selected_recipe(int32_t type, int64_t recipe) {
    if (type < 0 || type > 4 || recipe < -1) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (qol::game::craft_ui_type() < 0) return not_ready();
    qol::game::craft_set_selected_recipe_at(type, recipe);
    return contracts::ok();
}

QolResult call_craft_play_sound(int32_t sound_id) {
    if (sound_id < INT16_MIN || sound_id > INT16_MAX) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (fn_sound_system_play == nullptr) return not_ready();
    qol::game::craft_play_sound(static_cast<int16_t>(sound_id));
    return contracts::ok();
}

QolResult call_craft_show_text(uint32_t text_id) {
    if (fn_popup_create_ok_from_textdata == nullptr) return not_ready();
    qol::game::craft_show_text(text_id);
    return contracts::ok();
}

QolResult call_craft_show_yesno(uint32_t text_id, void* ok, void* cancel) {
    if (ok == nullptr || cancel == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::craft_show_yesno_from_textdata(text_id, ok, cancel)
               ? contracts::ok()
               : not_ready();
}

QolResult call_craft_description_set_off() {
    if (fn_ui_desc_set_off == nullptr) return not_ready();
    qol::game::craft_desc_set_off();
    return contracts::ok();
}

QolResult read_world_map_record_count(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::world_map_record_count();
    if (value <= 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_world_current_map_id(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::world_ready()) return not_ready();
    *out = qol::game::world_current_map_id();
    return contracts::ok();
}

QolResult read_world_map_name(int32_t map_id, const char** out) {
    if (map_id < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = qol::game::world_map_name(map_id);
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_world_direction(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::world_active_player_direction();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_world_money(int64_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::world_money_ready()) return not_ready();
    *out = qol::game::world_money();
    return contracts::ok();
}

QolResult call_world_create_yesno(const char* text, uint32_t text_len, void* ok, void* cancel,
                                  void* param) {
    if (text == nullptr || text_len == 0 || text_len > 4096 || ok == nullptr || cancel == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::world_create_yesno_ready()) return not_ready();
    return qol::game::world_create_yesno(text, text_len, ok, cancel, param)
               ? contracts::ok()
               : not_ready();
}

QolResult call_world_minus_money(int64_t amount) {
    if (amount < 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::world_money_ready()) return not_ready();
    return qol::game::world_minus_money(amount) ? contracts::ok() : not_ready();
}

QolResult call_world_map_change(int32_t map_id, int32_t x, int32_t y, int32_t direction) {
    if (map_id < 0) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::world_map_change_ready()) return not_ready();
    qol::game::world_request_map_change(map_id, x, y, direction);
    return contracts::ok();
}

QolResult call_world_close_popup() {
    return qol::game::world_close_popup_process() ? contracts::ok() : not_ready();
}

QolResult call_world_restore_hud() {
    qol::game::world_restore_hud_gate();
    return contracts::ok();
}

QolResult call_world_show_banner(const char* text) {
    if (text == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::world_show_banner(text) ? contracts::ok() : not_ready();
}

QolResult read_save_slot_exists(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::save_slot_meta_deps_ready()) return not_ready();
    qol::game::save_slots_refresh_if_stale();
    void* save = qol::game::save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = qol::game::save_slot_exists(save) ? 1 : 0;
    return contracts::ok();
}

QolResult read_save_slot_hero_index(int32_t slot, int32_t* out) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::save_slot_meta_deps_ready()) return not_ready();
    qol::game::save_slots_refresh_if_stale();
    void* save = qol::game::save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    *out = qol::game::save_slot_hero_index(save);
    return contracts::ok();
}

QolResult read_save_hero_value(int32_t slot, int32_t* out, bool level) {
    if (slot < 0 || out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    if (!qol::game::save_slot_meta_deps_ready()) return not_ready();
    qol::game::save_slots_refresh_if_stale();
    void* save = qol::game::save_slot_struct(slot);
    if (save == nullptr) return not_ready();
    void* hero = qol::game::save_slot_hero(save);
    if (hero == nullptr) return not_ready();
    *out = level ? qol::game::save_hero_level(hero) : qol::game::save_hero_class(hero);
    return contracts::ok();
}

QolResult read_save_load_data(int32_t slot, void** out_data, int32_t* out_size) {
    if (slot < 0 || out_data == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_data = nullptr;
    *out_size = 0;
    if (!qol::game::save_load_deps_ready()) return not_ready();
    if (qol::game::save_load_data(slot, out_data, out_size) != 1 || *out_data == nullptr) {
        return error_result(kQolErrInternal, "save load failed");
    }
    return contracts::ok();
}

QolResult read_save_hub_key(const char** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::save_hub_get_key();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult call_save_free_data(void* data) {
    if (data == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    qol::game::save_mem_free(data);
    return contracts::ok();
}

QolResult call_save_encrypt(void* data, int32_t size, int32_t mode, const char* key,
                            int32_t* out_result) {
    if (data == nullptr || size < 0 || (mode != 0 && mode != 1) || key == nullptr ||
        out_result == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::save_encrypt_available()) return not_ready();
    *out_result = qol::game::save_encrypt_process2(data, size, mode, key);
    return contracts::ok();
}

QolResult call_save_mark_stale() {
    qol::game::save_slots_mark_stale();
    return contracts::ok();
}

QolResult read_quest_active_slot_count(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::quest_active_slot_count();
    return contracts::ok();
}

QolResult read_quest_active_slot_id(uint32_t index, uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    uint16_t id = 0;
    if (!qol::game::quest_active_slot_id(index, &id)) return not_ready();
    *out = id;
    return contracts::ok();
}

QolResult read_quest_total_count(uint32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    *out = qol::game::quest_total_count();
    return contracts::ok();
}

QolResult read_quest_value(uint32_t quest_id, int32_t* out, bool complete) {
    if (out == nullptr || quest_id > UINT16_MAX) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (complete) {
        *out = qol::game::quest_is_complete(static_cast<uint16_t>(quest_id)) ? 1 : 0;
        return contracts::ok();
    }
    const int32_t value = qol::game::quest_state(static_cast<uint16_t>(quest_id));
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult write_quest_state(uint32_t quest_id, int32_t state) {
    if (quest_id > UINT16_MAX) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::quest_set_state(static_cast<uint16_t>(quest_id), state)
               ? contracts::ok()
               : not_ready();
}

QolResult read_choice_state_id(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const int value = qol::game::ui_choice_state_id();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_choice_selected_index(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::ui_choice_selected_index(out) ? contracts::ok() : not_ready();
}

QolResult read_choice_text_owns(const char* const* items, uint32_t count, int32_t* out) {
    if (items == nullptr || out == nullptr || count == 0 || count > 6) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out = qol::game::ui_choice_text_owns(items, count) ? 1 : 0;
    return contracts::ok();
}

QolResult call_choice_write_items(const char* const* items, uint32_t count) {
    if (items == nullptr || count == 0 || count > 6) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    return qol::game::ui_choice_write_items(items, count) ? contracts::ok() : not_ready();
}

QolResult call_choice_write_title(const char* title) {
    if (title == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::ui_choice_write_title(title) ? contracts::ok() : not_ready();
}

QolResult call_choice_write_focus(int32_t index) {
    if (index < 0 || index > 255) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    return qol::game::ui_choice_write_focus(index) ? contracts::ok() : not_ready();
}

QolResult call_choice_push() {
    return qol::game::ui_choice_push() ? contracts::ok() : not_ready();
}

QolResult call_choice_close_popup() {
    return qol::game::ui_choice_close_popup() ? contracts::ok() : not_ready();
}

QolResult call_choice_play_sound(int32_t sound_id) {
    return qol::game::ui_choice_play_sound(sound_id) ? contracts::ok() : not_ready();
}

QolResult read_attr_option_value(int32_t option_index, int32_t level, int32_t flag,
                                 const void* item, int32_t* out_value) {
    if (item == nullptr || out_value == nullptr || option_index < 0 || level < 0 || flag < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::attr_call_option_value(option_index, level, flag, const_cast<void*>(item),
                                           out_value)) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult read_attr_jewel_value(int32_t attribute_type, const void* item, int32_t* out_value) {
    if (item == nullptr || out_value == nullptr || attribute_type < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::attr_call_jewel_value(attribute_type, const_cast<void*>(item), out_value)) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult read_attr_uses_fixed_option_table(int32_t category, int32_t* out_value) {
    if (category < 0 || out_value == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    *out_value = qol::game::attr_item_uses_fixed_option_table(category) ? 1 : 0;
    return contracts::ok();
}

QolResult read_attr_fixed_option_percent_pair(int32_t category, int32_t option_index,
                                              int32_t* out_p1, int32_t* out_p2) {
    if (category < 0 || option_index < 0 || out_p1 == nullptr || out_p2 == nullptr) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!qol::game::attr_fixed_option_percent_pair(category, option_index, out_p1, out_p2)) {
        return not_ready();
    }
    return contracts::ok();
}

QolResult ui_insert_inline_color(void* builder, uint32_t max_text_bytes, uint8_t color_code) {
    constexpr uint32_t kMaxTextBytes = 4096;
    if (builder == nullptr || max_text_bytes == 0 || max_text_bytes > kMaxTextBytes ||
        color_code == 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    auto** text_slot = reinterpret_cast<char**>(builder);
    char* text = *text_slot;
    if (text == nullptr) return error_result(kQolErrInvalidArgument, kInvalidArgument);
    const size_t length = strnlen(text, static_cast<size_t>(max_text_bytes) + 1);
    if (length > max_text_bytes) return error_result(kQolErrBlocked, "text exceeds bound");
    if (!qol::game::attr_insert_value_color(text, text + length,
                                            static_cast<char>(color_code))) {
        return not_ready();
    }
    *text_slot = text + length + 4;
    return contracts::ok();
}

QolResult write_craft_stuff_need(uint32_t index, int32_t category, int32_t count) {
    if (category < 0 || count < 0 || count > UINT16_MAX) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    void* item = qol::game::craft_stuff_item(index);
    if (item != nullptr && qol::game::attr_item_category(item) != category) {
        return error_result(kQolErrConflict, "category mismatch");
    }
    qol::game::craft_write_stuff_need(index, static_cast<uint16_t>(count));
    return contracts::ok();
}

QolResult write_craft_stuff_item(uint32_t index, void* item) {
    if (index >= qol::game::kCraftStuffSlotCount) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    qol::game::craft_set_stuff_item(index, item);
    return contracts::ok();
}

QolResult write_craft_target_item(void* item) {
    qol::game::craft_set_target_slot_item(item);
    return contracts::ok();
}

QolResult write_craft_cost_zero() {
    qol::game::craft_set_cost_zero();
    return contracts::ok();
}

QolResult game_call_known(uint32_t, const void*, uint32_t, void*, uint32_t) {
    return unsupported();
}

contracts::QolGameHost make_game_host() {
    contracts::QolGameHost host{};
    host.abi = {sizeof(contracts::QolGameHost), contracts::kGameHostMajor,
                contracts::kGameHostMinor};
    host.target = {&game_query_variant, &game_has_capability, &game_symbol_available};
    host.thread = {&game_is_main_thread, &game_frame_count};
    host.read.character = {&read_char_type, &read_char_active, &read_char_party};
    host.read.item = {&read_item_category, &read_item_jewel, &read_item_ability};
    host.read.world = {&read_world_position, &read_world_state};
    host.read.craft = {&read_craft_ui_type, &read_craft_panel_state, &read_craft_mix_type,
                       &read_craft_selected_index, &read_craft_selected_item,
                       &read_craft_stuff_item, &read_craft_category_count,
                       &read_craft_character_level};
    host.read.save = {&read_save_slot_state};
    host.read.quest = {&read_quest_state};
    host.read.attr_range = {&read_attr_option_value, &read_attr_jewel_value,
                            &read_attr_uses_fixed_option_table,
                            &read_attr_fixed_option_percent_pair};
    host.write.craft = {&write_craft_stuff_need, &write_craft_stuff_item,
                        &write_craft_target_item, &write_craft_cost_zero};
    host.call = {&game_call_known};
    host.ui_primitive = {};
    host.ui_primitive.insert_inline_color = &ui_insert_inline_color;
    host.extra.character = {&read_char_party_member, &read_char_summoner, &read_char_level,
                            &read_char_merc_slot, &read_char_can_change_equip,
                            &read_char_menu_character, &read_char_pool_slot, &read_char_max_hp,
                            &read_char_hp};
    host.extra.character_write = {&write_char_max_hp, &write_char_hp};
    host.extra.item = {&read_item_rarity, &read_item_real_equip, &read_item_equip_level,
                       &read_item_no_sell, &read_item_dice, &read_item_cumulate_count,
                       &read_item_class_no_equip_bit, &read_item_equip_desc_role_type};
    host.extra.inventory = {&read_inventory_bit_range};
    host.extra.inventory_write = {&write_inventory_remove_item_data,
                                  &write_inventory_remove_item,
                                  &write_inventory_create_perfect_item,
                                  &write_inventory_create_item,
                                  &write_inventory_save_item,
                                  &write_inventory_free_item};
    host.extra.craft = {&read_craft_slot_control, &read_craft_target_item,
                        &read_craft_selected_inventory_item, &read_craft_selected_recipe,
                        &read_craft_slot_count};
    host.extra.craft_call = {&call_craft_init, &call_craft_reset, &call_craft_refresh,
                             &call_craft_select_first_empty, &call_craft_set_type,
                             &call_craft_set_selected_recipe, &call_craft_play_sound,
                             &call_craft_show_text, &call_craft_show_yesno,
                             &call_craft_description_set_off};
    host.extra.world = {&read_world_map_record_count, &read_world_current_map_id,
                        &read_world_map_name, &read_world_direction, &read_world_money};
    host.extra.world_call = {&call_world_create_yesno, &call_world_minus_money, &call_world_map_change,
                             &call_world_close_popup, &call_world_restore_hud,
                             &call_world_show_banner};
    host.extra.save = {&read_save_slot_exists, &read_save_slot_hero_index,
                       [](int32_t slot, int32_t* out) {
                           return read_save_hero_value(slot, out, true);
                       },
                       [](int32_t slot, int32_t* out) {
                           return read_save_hero_value(slot, out, false);
                       },
                       &read_save_load_data, &read_save_hub_key};
    host.extra.save_call = {&call_save_free_data, &call_save_encrypt, &call_save_mark_stale};
    host.extra.quest = {&read_quest_active_slot_count, &read_quest_active_slot_id,
                        &read_quest_total_count,
                        [](uint32_t quest_id, int32_t* out) {
                            return read_quest_value(quest_id, out, false);
                        },
                        [](uint32_t quest_id, int32_t* out) {
                            return read_quest_value(quest_id, out, true);
                        }};
    host.extra.quest_write = {&write_quest_state};
    host.extra.choice = {&read_choice_state_id, &read_choice_selected_index,
                         &read_choice_text_owns};
    host.extra.choice_call = {&call_choice_write_items, &call_choice_write_title,
                              &call_choice_write_focus, &call_choice_push,
                              &call_choice_close_popup, &call_choice_play_sound};
    return host;
}

}  // namespace

void game_host_mark_main_thread() {
    if (g_main_thread_set.load(std::memory_order_acquire)) return;
    g_main_thread = pthread_self();
    g_main_thread_set.store(true, std::memory_order_release);
}

extern const contracts::QolGameHost g_game_host = make_game_host();

}  // namespace qol::host
