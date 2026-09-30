#include <cassert>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "contracts/panels.h"
#include "contracts/ui.h"
#include "game/variants/game_variant.h"
#include "game/access/game_access.h"
#include "game/access/item_handle_registry.h"
#include "game/access/attr_range_ui.h"
#include "game/access/char.h"
#include "game/access/craft.h"
#include "game/access/quest.h"
#include "game/access/save.h"
#include "game/access/symbols.h"
#include "game/access/world.h"
#include "game/adapter_ports/character_port.h"
#include "game/state/game_state.h"
#include "game/adapter_ports/character_extras_port.h"
#include "game/adapter_ports/craft_port.h"
#include "game/adapter_ports/choice_port.h"
#include "game/adapter_ports/extension_bag_write_port.h"
#include "game/adapter_ports/item_port.h"
#include "game/adapter_ports/inventory_extension_facade.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/adapter_ports/item_extras_port.h"
#include "game/adapter_ports/inventory_extras_port.h"
#include "game/adapter_ports/quest_port.h"
#include "game/adapter_ports/save_call_port.h"
#include "game/adapter_ports/save_port.h"
#include "game/adapter_ports/world_port.h"
#include "game/clock.h"
#include "host/adapters.h"
#include "infra/capabilities.h"
#include "infra/command_queue.h"
#include "infra/events.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "infra/store.h"
#include "infra/text_scope.h"

uintptr_t g_base = 0;
void* g_active_quest = nullptr;
qol::contracts::QolItemHandle g_inventory_item_handle{};
void* g_inven = reinterpret_cast<void*>(1);
std::array<uint8_t, 0x200> g_inventory_physical_item{};
std::array<uint8_t, 0x200> g_inventory_new_item{};
void* g_raw_inventory_item = reinterpret_cast<void*>(0x9);

std::string op_ok() { return "{\"ok\":true}"; }
std::string op_err(const char* message) {
    return std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
}

enum class FakeInventoryMode { kReady, kEmpty, kNotReady };
FakeInventoryMode g_inventory_mode = FakeInventoryMode::kReady;
int g_world_game_state_gate = 0;

decltype(fn_get_rarity) fn_get_rarity = nullptr;
decltype(fn_get_damage) fn_get_damage = nullptr;
decltype(fn_get_defense) fn_get_defense = nullptr;
decltype(fn_item_get_ability_level) fn_item_get_ability_level = nullptr;
decltype(fn_item_is_no_sell) fn_item_is_no_sell = nullptr;
decltype(fn_is_dice) fn_is_dice = nullptr;
decltype(fn_get_cumulate_count) fn_get_cumulate_count = nullptr;
decltype(fn_get_bit) fn_get_bit = nullptr;
decltype(fn_control_object_get_cursor_index) fn_control_object_get_cursor_index = nullptr;
decltype(fn_inven_remove_item_data) fn_inven_remove_item_data = nullptr;
decltype(fn_remove_item) fn_remove_item = nullptr;
decltype(fn_item_create_perfect_item) fn_item_create_perfect_item = nullptr;
decltype(fn_create_item) fn_create_item = nullptr;
decltype(fn_inven_save_item) fn_inven_save_item = nullptr;
decltype(fn_inven_save_item_on_empty) fn_inven_save_item_on_empty = nullptr;
decltype(fn_save_save_item) fn_save_save_item = nullptr;
decltype(fn_save_load_item) fn_save_load_item = nullptr;
decltype(fn_itempool_free) fn_itempool_free = nullptr;
decltype(fn_inven_move_item) fn_inven_move_item = nullptr;
decltype(fn_equip_item_from_inven_to_slot) fn_equip_item_from_inven_to_slot = nullptr;
decltype(fn_ui_equip_apply_stuff) fn_ui_equip_apply_stuff = nullptr;
decltype(fn_char_use_item_ex) fn_char_use_item_ex = nullptr;
decltype(fn_mercenary_make_mercenary) fn_mercenary_make_mercenary = nullptr;
decltype(fn_minus_money) fn_minus_money = nullptr;
decltype(fn_ui_equip_refresh_item_area) fn_ui_equip_refresh_item_area = nullptr;
decltype(fn_ui_equip_refresh_bag_area) fn_ui_equip_refresh_bag_area = nullptr;
decltype(fn_ui_store_refresh_inven_bag) fn_ui_store_refresh_inven_bag = nullptr;
decltype(fn_ui_store_refresh_inven_item) fn_ui_store_refresh_inven_item = nullptr;
decltype(fn_sound_system_play) fn_sound_system_play = nullptr;
decltype(fn_uimix_init_mixing_state) fn_uimix_init_mixing_state = nullptr;
decltype(fn_uimix_reset_stuff_item_control) fn_uimix_reset_stuff_item_control = nullptr;
decltype(fn_ui_mix_refresh_inven_item) fn_ui_mix_refresh_inven_item = nullptr;
decltype(fn_ui_equip_update_char_equip) fn_ui_equip_update_char_equip = nullptr;
decltype(fn_uimix_set_type) fn_uimix_set_type = nullptr;
decltype(fn_popup_create_ok_from_textdata) fn_popup_create_ok_from_textdata = nullptr;
decltype(fn_ui_desc_set_off) fn_ui_desc_set_off = nullptr;
decltype(fn_popup_create_yesno_from_textdata) fn_popup_create_yesno_from_textdata = nullptr;
decltype(fn_control_object_get_cursor) fn_control_object_get_cursor = nullptr;
decltype(fn_control_object_get_data) fn_control_object_get_data = nullptr;
decltype(fn_control_item_get_item) fn_control_item_get_item = nullptr;
decltype(fn_control_item_set_item) fn_control_item_set_item = nullptr;
decltype(fn_can_equip) fn_can_equip = nullptr;
decltype(fn_find_equip_slot) fn_find_equip_slot = nullptr;
decltype(fn_get_equip_item) fn_get_equip_item = nullptr;
decltype(fn_set_equip_item) fn_set_equip_item = nullptr;
decltype(fn_put_jewel) fn_put_jewel = nullptr;
decltype(fn_ui_desc_get_data) fn_ui_desc_get_data = nullptr;
decltype(fn_ui_equip_inven_item_control_event_proc)
    fn_ui_equip_inven_item_control_event_proc = nullptr;
decltype(fn_mercenary_is_empty_manager_slot) fn_mercenary_is_empty_manager_slot = nullptr;
decltype(fn_is_enchant_scroll) fn_is_enchant_scroll = nullptr;
decltype(fn_is_mercenary_seal) fn_is_mercenary_seal = nullptr;
decltype(fn_is_use) fn_is_use = nullptr;
decltype(fn_save_is_ok) fn_save_is_ok = nullptr;
decltype(fn_get_bag_size) fn_get_bag_size = nullptr;
decltype(fn_is_empty_bag) fn_is_empty_bag = nullptr;
decltype(fn_inven_find_save_slot) fn_inven_find_save_slot = nullptr;

int tutorial_state() { return 0; }
void tutorial_cancel() {}

int fake_item_rarity(void*) { return 9; }
int fake_item_no_sell(int32_t) { return 0; }
int fake_item_dice(int32_t) { return 0; }
int fake_item_cumulate_count(void*) { return 6; }
bool g_item_count_probe = false;
uint32_t g_adapter_canonical_count = 0;
int32_t g_adapter_canonical_error = kQolOk;
bool adapter_item_count_probe() { return g_item_count_probe; }
bool overflow_result(void*, char*, uint32_t capacity, uint32_t* out_size) {
    *out_size = capacity + 1;
    return false;
}
QolResult adapter_item_count_canonical(const void*, uint32_t* out_count) {
    if (g_adapter_canonical_error != kQolOk) {
        return qol::contracts::error(g_adapter_canonical_error, "provider error", 14);
    }
    *out_count = g_adapter_canonical_count;
    return qol::contracts::ok();
}
QolResult adapter_item_count_effective(const void*, uint32_t* out_count) {
    *out_count = 88;
    return qol::contracts::ok();
}
int fake_can_equip(void*, void*) { return 1; }
int fake_find_equip_slot(void*, void*) { return 2; }
void* fake_get_equip_item(void*, int32_t) { return reinterpret_cast<void*>(0xA); }
int fake_empty_mercenary_slot() { return 1; }
int fake_is_enchant_scroll(int32_t) { return 1; }
int fake_is_mercenary_seal(int32_t) { return 1; }
int fake_is_use(int32_t) { return 1; }
int fake_save_is_ok() { return 1; }
int fake_get_bag_size(int bag) { return bag + 4; }
int fake_is_empty_bag(int) { return 0; }
int fake_find_save_slot(void*, int8_t* out_slot) {
    if (out_slot != nullptr) *out_slot = 3;
    return 1;
}

uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

bool game_memory_accessible(const void*, size_t, char) { return false; }

namespace qol {
namespace {
GameVariant g_test_variant{GameSeries::kOriginal, "test", kCapWarehouse, {}, true};
}

bool game_variant_init() { return true; }
const GameVariant& game_variant() { return g_test_variant; }
}  // namespace qol

namespace qol::game {

bool g_item_class_table_ready = true;

int char_type(const void*) { return 2; }
bool char_faction_deps_ready() { return true; }
bool char_active_group_resolved() { return true; }
bool char_party_index_resolved() { return true; }
bool char_summoner_resolved() { return true; }
bool char_is_active_player_group(void*) { return true; }
bool char_is_party_member(void*) { return true; }
void* char_get_summoner(void*) { return nullptr; }
int char_party_index(const void*) { return 0; }
int char_level(const void*) { return 10; }
int char_merc_slot(const void*) { return 0; }
bool char_can_change_equip_resolved() { return true; }
bool char_can_change_equip(const void*) { return true; }
bool char_can_equip(const void*, const void*) { return true; }
bool char_can_equip_resolved() { return true; }
int char_find_equip_slot(const void*, const void*) { return 2; }
bool char_find_equip_slot_resolved() { return true; }
void* char_equipped_item(const void*, int) { return reinterpret_cast<void*>(0xA); }
bool char_equipped_item_resolved() { return true; }
int char_has_empty_mercenary_slot() { return 1; }
bool char_has_empty_mercenary_slot_resolved() { return true; }
bool char_menu_character_resolved() { return true; }
void* char_menu_character() { return reinterpret_cast<void*>(0x2); }
bool char_pool_slot(const void*, int* out) {
    if (out != nullptr) *out = 0;
    return out != nullptr;
}
bool char_read_max_hp(const void*, int32_t* out) {
    if (out != nullptr) *out = 100;
    return out != nullptr;
}
bool char_read_hp(const void*, int32_t* out) {
    if (out != nullptr) *out = 80;
    return out != nullptr;
}
bool char_write_max_hp(void*, int32_t) { return true; }
bool char_write_hp(void*, int32_t) { return true; }
int attr_item_category(const void*) { return 4; }
bool attr_is_jewel_category(int) { return true; }
int attr_ability_level(const void*) { return 3; }
int item_enchant_remaining(const void*) { return g_item_class_table_ready ? 7 : -1; }
int item_socket_filled(const void*) { return g_item_class_table_ready ? 2 : -1; }
int item_socket_total(const void*) { return g_item_class_table_ready ? 5 : -1; }
int item_jewel_value(const void*) { return 321; }
int item_jewel_attribute_type(const void*) { return 17; }
int item_is_enchant_scroll(int) { return 1; }
int item_is_mercenary_seal(int) { return 1; }
int item_is_usable(int) { return 1; }
bool item_class_table_ready() { return g_item_class_table_ready; }
bool item_is_backpack_category(int category) {
    return g_item_class_table_ready && category == 4;
}
bool attr_call_option_value(int, int, int, void*, int* out_value) {
    *out_value = 3;
    return true;
}
bool attr_call_jewel_value(int, void*, int* out_value) {
    *out_value = 4;
    return true;
}
bool attr_item_uses_fixed_option_table(int) { return false; }
bool attr_fixed_option_percent_pair(int, int, int*, int*) { return false; }
bool attr_insert_value_color(char*, char*, char) { return true; }
bool item_equip_primitives_ready() { return true; }
bool item_is_real_equip_resolved() { return true; }
bool item_equip_level_resolved() { return true; }
bool item_is_real_equip(const void*) { return true; }
int item_equip_level(const void*) { return 12; }
void* item_class_record(uint16_t) { return reinterpret_cast<void*>(0x8); }
bool item_class_no_equip_bit(const void*) { return false; }
int equip_desc_role_type() { return 0; }
int64_t craft_ui_type() { return 1; }
int64_t craft_get_ui_type() { return 1; }
uint8_t craft_panel_state() { return 2; }
uint32_t craft_mix_type() { return 3; }
int64_t craft_selected_stuff_index() { return 0; }
void* craft_stuff_item(size_t) { return reinterpret_cast<void*>(0x1); }
void* craft_stuff_slot_control(size_t) { return reinterpret_cast<void*>(0x3); }
void* craft_target_slot_control() { return reinterpret_cast<void*>(0x4); }
void* craft_target_slot_item() { return reinterpret_cast<void*>(0x4); }
void* craft_selected_inven_item() { return reinterpret_cast<void*>(0x7); }
int64_t craft_selected_recipe_at(int64_t) { return 0; }
uint32_t craft_stuff_slot_count() { return 3; }
int craft_category_held_count(uint16_t) { return 5; }
int craft_character_level() { return 10; }
void craft_write_stuff_need(size_t, uint16_t) {}
void craft_set_stuff_item(size_t, void*) {}
void craft_set_target_slot_item(void*) {}
void craft_set_cost_zero() {}
void craft_set_selected_recipe_at(int64_t, int64_t) {}
void craft_init_mixing_state() {}
void craft_reset_stuff_item_control() {}
void craft_refresh_inven_items() {}
int craft_select_first_empty_stuff_slot() { return 1; }
void craft_set_ui_type(int64_t) {}
void craft_play_sound(int16_t) {}
void craft_show_text(uint32_t) {}
bool craft_show_yesno_from_textdata(uint32_t, void*, void*) { return true; }
void craft_desc_set_off() {}
void* save_slot_struct(int32_t) { return reinterpret_cast<void*>(0x1); }
bool save_slot_exists(const void*) { return true; }
bool save_slot_meta_deps_ready() { return true; }
void save_slots_refresh_if_stale() {}
int save_slot_hero_index(const void*) { return 0; }
void* save_slot_hero(void*) { return reinterpret_cast<void*>(0x5); }
int save_slot_map_id(const void*) { return 1; }
int save_hero_level(const void*) { return 10; }
int save_hero_class(const void*) { return 2; }
bool save_load_deps_ready() { return true; }
int save_load_data(int32_t, void** out_buf, int* out_len) {
    if (out_buf != nullptr) *out_buf = reinterpret_cast<void*>(0x6);
    if (out_len != nullptr) *out_len = 16;
    return out_buf != nullptr && out_len != nullptr ? 1 : 0;
}
void save_mem_free(void*) {}
const char* save_hub_get_key() { return "test-key"; }
bool save_encrypt_available() { return true; }
int save_encrypt_process2(void*, int, int, const char*) { return 1; }
void save_slots_mark_stale() {}
int save_current_slot() { return -1; }
int save_is_ok() { return 1; }
int world_state() { return 7; }
int world_game_state_gate() { return g_world_game_state_gate; }
bool world_ready() { return true; }
int world_map_record_count() { return 3; }
int world_current_map_id() { return 1; }
const char* world_map_name(int) { return "test-map"; }
int world_active_player_direction() { return 2; }
bool world_create_yesno_ready() { return true; }
bool world_create_yesno(const char*, uint32_t, void*, void*, void*) { return true; }
bool world_money_ready() { return true; }
int64_t world_money() { return 1000; }
bool world_minus_money(int64_t) { return true; }
bool world_map_change_ready() { return true; }
void world_request_map_change(int, int, int, int) {}
bool world_close_popup_process() { return true; }
void world_restore_hud_gate() {}
bool world_show_banner(const char*) { return true; }
uint32_t quest_active_slot_count() { return 1; }
bool quest_active_slot_id(uint32_t, uint16_t* out) {
    if (out != nullptr) *out = 7;
    return out != nullptr;
}
uint16_t quest_total_count() { return 10; }
int32_t quest_state(uint16_t) { return 1; }
bool quest_is_complete(uint16_t) { return true; }
bool quest_set_state(uint16_t, int32_t) { return true; }
int ui_choice_state_id() { return 42; }
bool ui_choice_write_items(const char* const*, uint32_t) { return true; }
bool ui_choice_write_title(const char*) { return true; }
bool ui_choice_write_focus(int32_t) { return true; }
bool ui_choice_push() { return true; }
bool ui_choice_close_popup() { return true; }
bool ui_choice_play_sound(int32_t) { return true; }
bool ui_choice_selected_index(int32_t* out) {
    if (out != nullptr) *out = 0;
    return out != nullptr;
}
bool ui_choice_text_owns(const char* const*, uint32_t) { return true; }
int64_t frame_count() { return 99; }

#ifndef QOL_HOST_ADAPTER_REAL_INVENTORY
QolResult inventory_item_ref_at(int32_t bag, int32_t slot,
                                contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag > 10 || slot < 0 || slot >= 16) {
        return contracts::error(kQolErrInvalidArgument, "invalid argument", 17);
    }
    if (g_inventory_mode == FakeInventoryMode::kNotReady) {
        return contracts::error(kQolErrNotReady, "inventory not ready", 20);
    }
    if (g_inventory_mode == FakeInventoryMode::kEmpty) {
        return contracts::error(kQolErrNotFound, "inventory slot is empty", 24);
    }
    *out = {static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal), bag, slot, 28, 4,
            g_inventory_item_handle};
    return contracts::ok();
}

QolResult for_each_bag_slot(contracts::QolInventoryBagSlotFn callback, void* ctx) {
    if (callback == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid argument", 17);
    if (g_inventory_mode == FakeInventoryMode::kNotReady) {
        return contracts::error(kQolErrNotReady, "inventory not ready", 20);
    }
    if (g_inventory_mode == FakeInventoryMode::kEmpty) return contracts::ok();
    const contracts::QolInventoryItemRef ref{
        static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal), 1, 2, 28, 4,
        g_inventory_item_handle};
    (void)callback(&ref, ctx);
    return contracts::ok();
}

QolResult bag_size(int32_t bag, int32_t* out_size) {
    if (out_size == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid argument", 17);
    *out_size = bag + 4;
    return contracts::ok();
}

QolResult is_empty_bag(int32_t, int32_t* out_empty) {
    if (out_empty == nullptr) return contracts::error(kQolErrInvalidArgument, "invalid argument", 17);
    *out_empty = 0;
    return contracts::ok();
}

QolResult find_save_slot(const void* item, int32_t* out_slot) {
    if (item == nullptr || out_slot == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid argument", 17);
    }
    *out_slot = 3;
    return contracts::ok();
}
#endif

}  // namespace qol::game

// T2h-B3#2：本 target 链接 game/read/item_class.cpp，其 debug raw 原语引用全局
// inventory_item_ref_at（与 qol::game 下的 port 版同名不同签名）；本测试不触发该路径，
// stub 仅满足链接。
bool inventory_item_ref_at(int bag, int slot, InventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag > 10 || slot < 0 || slot >= 16 ||
        g_inventory_mode != FakeInventoryMode::kReady) return false;
    *out = {InventoryItemKind::kOriginal, bag, slot, 28, 4, g_raw_inventory_item};
    return true;
}

void for_each_bag_slot(BagSlotFn callback, void* ctx) {
    if (callback == nullptr || g_inven == nullptr ||
        g_inventory_mode != FakeInventoryMode::kReady) return;
    (void)callback(g_raw_inventory_item, 1, 2, ctx);
}

namespace {

int64_t g_frame = 1;
int g_event_calls = 0;

int64_t fake_frame_count() { return g_frame; }

void event_handler(void*, const void*) { ++g_event_calls; }

int32_t scheduler_handler(int64_t frame, void* ctx) {
    assert(frame == g_frame);
    ++*static_cast<int*>(ctx);
    return 0;
}

int32_t inventory_callback(const qol::contracts::QolInventoryItemRef* ref, void* ctx) {
    assert(ref != nullptr);
    assert(ref->kind == static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kOriginal));
    assert(ref->category == 28 && ref->count == 6);
    ++*static_cast<int*>(ctx);
    return 0;
}

bool panel_open() { return true; }
void panel_close() {}
bool panel_active() { return true; }
const char* panel_block_reason() { return nullptr; }

int g_find_item_mode = 0;
int g_find_item_category = 28;
int g_find_item_backup_calls = 0;
int g_find_item_item = 0;
qol::contracts::QolItemHandle g_find_item_handle{};

void* fake_find_item_backup(int32_t category) {
    ++g_find_item_backup_calls;
    return g_find_item_mode == 1 && category == g_find_item_category ? &g_find_item_item : nullptr;
}

int fake_find_item_slot(void* item, int8_t* out_slot) {
    if (item != &g_find_item_item || out_slot == nullptr) return 0;
    *out_slot = static_cast<int8_t>((2 << 5) | 7);
    return 1;
}

QolResult find_item_provider_for_each(qol::contracts::QolInventoryExtensionItemFn callback,
                                      void* ctx) {
    if (callback == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "null", 4);
    const qol::contracts::QolInventoryExtensionItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 3,
        g_find_item_category, 1, g_find_item_handle};
    (void)callback(&ref, ctx);
    return qol::contracts::ok();
}

bool find_item_provider_available() { return true; }

}  // namespace

int main() {
    fn_get_rarity = &fake_item_rarity;
    fn_item_is_no_sell = &fake_item_no_sell;
    fn_is_dice = &fake_item_dice;
    fn_get_cumulate_count = &fake_item_cumulate_count;
    fn_can_equip = &fake_can_equip;
    fn_find_equip_slot = &fake_find_equip_slot;
    fn_get_equip_item = &fake_get_equip_item;
    fn_mercenary_is_empty_manager_slot = &fake_empty_mercenary_slot;
    fn_is_enchant_scroll = &fake_is_enchant_scroll;
    fn_is_mercenary_seal = &fake_is_mercenary_seal;
    fn_is_use = &fake_is_use;
    fn_save_is_ok = &fake_save_is_ok;
    fn_get_bag_size = &fake_get_bag_size;
    fn_is_empty_bag = &fake_is_empty_bag;
    fn_inven_find_save_slot = &fake_find_save_slot;
    qol::infra::command_queue_reset_for_test();
    const qol::infra::CommandTicket overflow_ticket = qol::infra::post(
        qol::infra::Command{"host_adapter_overflow", nullptr, nullptr, false,
                            &overflow_result});
    assert(overflow_ticket != 0 && qol::infra::drain() == 1);
    assert(qol::infra::ticket_state(overflow_ticket) == qol::infra::CommandState::kFailed);
    assert(qol::infra::ticket_result_overflowed(overflow_ticket));
    qol::infra::command_queue_reset_for_test();
    const qol::contracts::QolHostSdk& sdk = qol::host::g_host_sdk;
    assert(sdk.abi.size == sizeof(qol::contracts::QolHostSdk));
    assert(sdk.abi.major == qol::contracts::kHostSdkMajor);
    assert(sdk.ui == nullptr);
    assert(sdk.config.get_bool != nullptr);
    assert(sdk.scheduler.add != nullptr);
    assert(sdk.events.subscribe != nullptr);
    assert(sdk.hooks.intercept_blocks != nullptr);
    assert(sdk.store.read_container != nullptr);
    assert(sdk.store.import_container != nullptr);
    assert(sdk.store.delete_slot != nullptr);
    assert(sdk.store.register_participant != nullptr);
    assert(sdk.store.save_game != nullptr);
    assert(sdk.item_handle.register_item != nullptr);
    assert(sdk.item_handle.lookup_item != nullptr);
    static_assert(sizeof(qol::contracts::QolItemHandleApi) == sizeof(void*) * 6,
                  "plugin handle API must not expose object resolution");
    assert(sdk.item_handle.invalidate_item != nullptr);
    assert(sdk.item_handle.rebind_item != nullptr);
    assert(sdk.item_handle.begin_invalidation != nullptr);
    assert(sdk.item_handle.finish_invalidation != nullptr);
    assert(sdk.desc_button.bind != nullptr);
    assert(sdk.desc_button.unbind != nullptr);
    assert(sdk.desc_button.current_proc != nullptr);
    assert(sdk.desc_button.original_matches != nullptr);
    assert(sdk.desc_button.call_original != nullptr);
    void* desc_original = nullptr;
    assert(sdk.desc_button.bind(11, nullptr, &desc_original).code == kQolErrInvalidArgument);
    assert(sdk.desc_button.unbind(11).code == kQolErrInvalidArgument);
    assert(sdk.desc_button.current_proc(0, nullptr).code == kQolErrInvalidArgument);
    assert(sdk.desc_button.original_matches(nullptr, "symbol", nullptr).code ==
           kQolErrInvalidArgument);
    assert(sdk.desc_button.call_original(nullptr, nullptr).code == kQolErrInvalidArgument);

    static_assert(sizeof(qol::contracts::QolStoreApi) == sizeof(void*) * 9,
                  "store ABI member count changed");

    uint32_t container_size = 0;
    assert(sdk.store.read_container(0, nullptr, 0, &container_size).code == kQolErrNotReady);
    assert(sdk.store.read_container(-1, nullptr, 0, &container_size).code ==
           kQolErrInvalidArgument);
    assert(sdk.store.read_container(0, nullptr, 0, nullptr).code == kQolErrInvalidArgument);
    assert(sdk.store.import_container(0, nullptr, 1).code == kQolErrInvalidArgument);
    assert(sdk.store.delete_slot(-1).code == kQolErrInvalidArgument);
    assert(sdk.store.register_participant(nullptr).code == kQolErrInvalidArgument);
    assert(sdk.store.save_game().code == kQolErrNotReady);

    qol::infra::store::init(".");
    assert(sdk.store.delete_slot(0).ok == kQolOk);
    assert(sdk.store.delete_slot(1).ok == kQolOk);

    const uint8_t section_data[] = {0x11, 0x22, 0x33, 0x44};
    assert(sdk.store.write(0, "adapter", 1, section_data, sizeof(section_data)).ok == kQolOk);

    uint8_t tiny_container[1] = {};
    container_size = 0;
    assert(sdk.store.read_container(0, tiny_container, sizeof(tiny_container), &container_size).code ==
           kQolErrInvalidArgument);
    assert(container_size > sizeof(tiny_container));

    std::vector<uint8_t> container(container_size);
    assert(sdk.store.read_container(0, container.data(), static_cast<uint32_t>(container.size()),
                                    &container_size)
               .ok == kQolOk);
    assert(container_size == container.size());
    assert(sdk.store.import_container(1, container.data(), container_size).ok == kQolOk);

    uint8_t section_out[sizeof(section_data)] = {};
    qol::contracts::QolStoreBuffer section_buffer{section_out, 0, sizeof(section_out)};
    uint16_t section_version = 0;
    assert(sdk.store.read(1, "adapter", &section_buffer, &section_version).ok == kQolOk);
    assert(section_version == 1 && section_buffer.size == sizeof(section_data));
    assert(std::equal(std::begin(section_data), std::end(section_data), section_out));
    assert(sdk.store.delete_slot(1).ok == kQolOk);
    assert(sdk.store.read(1, "adapter", &section_buffer, &section_version).code == kQolErrNotFound);

    int32_t value = 0;
    assert(sdk.config.set_bool("adapter.test", 1).ok == kQolOk);
    assert(sdk.config.get_bool("adapter.test", 0, &value).ok == kQolOk);
    assert(value == 1);

    uint64_t subscription = 0;
    assert(sdk.events.subscribe(1, &event_handler, nullptr, &subscription).ok == kQolOk);
    assert(subscription != 0);
    assert(qol::infra::publish(1, nullptr) == 1);
    assert(g_event_calls == 1);
    assert(sdk.events.unsubscribe(subscription).ok == kQolOk);

    static int capability = 7;
    assert(qol::infra::register_capability(
               qol::infra::CapabilityProvision{"adapter.capability", QolVersion{1, 0},
                                                &capability}));
    const void* iface = nullptr;
    assert(sdk.capabilities.require("adapter.capability", QolVersionRange{1, 0}, &iface).ok ==
           kQolOk);
    assert(iface == &capability);
    uint64_t lease = 0;
    assert(sdk.capabilities.lease_enter("adapter.capability", QolVersionRange{1, 0}, &lease).ok ==
           kQolOk);
    assert(sdk.capabilities.lease_leave(lease).ok == kQolOk);
    assert(qol::infra::unregister_capability("adapter.capability"));

    qol::infra::runtime_init(qol::infra::Clock{&fake_frame_count});
    int scheduler_calls = 0;
    uint64_t task = 0;
    assert(sdk.scheduler.add(0, &scheduler_handler, &scheduler_calls, 1, 1, &task).ok == kQolOk);
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kRenderPre);
    assert(scheduler_calls == 1);
    assert(sdk.scheduler.remove(task).ok != kQolOk);

    assert(sdk.text_scope.push(static_cast<int32_t>(qol::infra::TextScope::kCharacterPanel)).ok ==
           kQolOk);
    assert(qol::infra::active_text_scope() == qol::infra::TextScope::kCharacterPanel);
    assert(sdk.text_scope.pop().ok == kQolOk);
    assert(qol::infra::active_text_scope() == qol::infra::TextScope::kNone);

    static const qol::contracts::PanelProviderSpec provider{
        "adapter.panel", "adapter.test", &panel_open, &panel_close, &panel_active,
        &panel_block_reason};
    assert(sdk.panels.register_provider(&provider).ok == kQolOk);
    int32_t active = 0;
    assert(sdk.panels.open("adapter.panel").ok == kQolOk);
    assert(sdk.panels.active("adapter.panel", &active).ok == kQolOk);
    assert(active == 1);
    assert(sdk.panels.close("adapter.panel").ok == kQolOk);
    assert(sdk.panels.unregister_owner("adapter.test").ok == kQolOk);

    const qol::contracts::QolGameHost& game = qol::host::g_game_host;
    const auto& character_port_api = qol::game::character_port();
    const auto& character_extras_api = qol::game::character_extras_port();
    const auto& item_port_api = qol::game::item_port();
    const auto& item_extras_api = qol::game::item_extras_port();
    const auto& inventory_extras_api = qol::game::inventory_extras_port();
    const auto& world_port_api = qol::game::world_port();
    const auto& save_port_api = qol::game::save_port();
    const auto& save_call_port_api = qol::game::save_call_port();
    const auto& craft_port_api = qol::game::craft_port();
    const auto& choice = qol::game::choice_port();
    assert(game.abi.size == sizeof(qol::contracts::QolGameHost));
    assert(game.abi.major == 2);
    assert(game.target.query_variant != nullptr);
    qol::contracts::QolTargetVariant variant{};
    assert(game.target.query_variant(&variant).ok == kQolOk);
    assert(variant.id == static_cast<uint32_t>(qol::GameSeries::kOriginal));
    assert(variant.capabilities == qol::kCapWarehouse);
    assert(game.abi.minor == qol::contracts::kGameHostMinor);
    static_assert(std::is_standard_layout<qol::contracts::QolReadOnlyApi>::value,
                  "read-only face must remain standard layout");
    static_assert(offsetof(qol::contracts::QolGameHost, read_only) ==
                      offsetof(qol::contracts::QolGameHost, item_count) +
                          sizeof(qol::contracts::QolItemCountApi),
                  "read-only face must follow item-count face");
    qol::host::game_host_mark_main_thread();
    assert(game.thread.is_main_thread() == 1);
    assert(game.item_abi.find_item_wrapper != nullptr);
    assert(game.item_abi.bind_find_item_backup != nullptr);
    assert(game.item_abi.original_find_item_slot != nullptr);
    assert(game.item_abi.item_identity_at != nullptr);
    assert(game.item_abi.native_item_at != nullptr);
    assert(game.item_abi.native_item_for_handle != nullptr);
    assert(game.item_abi.bind_find_item_backup(&fake_find_item_backup).ok == kQolOk);
    qol::game::inventory_extension_bind_host();
    qol::contracts::QolInventoryExtensionProvider find_item_provider{};
    find_item_provider.for_each_item = &find_item_provider_for_each;
    assert(qol::game::inventory_extension_set_provider(&find_item_provider,
                                                        &find_item_provider_available));
    g_find_item_handle = qol::game::register_item(
        &g_find_item_item, qol::contracts::kQolItemHandleOwnerProvider);
    assert(g_find_item_handle.slot != 0 && g_find_item_handle.generation != 0);
    g_find_item_mode = 1;
    g_find_item_backup_calls = 0;
    assert(game.item_abi.find_item_wrapper(g_find_item_category) == &g_find_item_item);
    assert(g_find_item_backup_calls == 1);
    g_find_item_mode = 0;
    assert(game.item_abi.find_item_wrapper(g_find_item_category) == &g_find_item_item);
    game.item_abi.begin_original_only();
    assert(game.item_abi.find_item_wrapper(g_find_item_category) == nullptr);
    game.item_abi.end_original_only();
    fn_inven_find_item_slot = &fake_find_item_slot;
    int32_t found_bag = -1;
    int32_t found_slot = -1;
    assert(game.item_abi.original_find_item_slot(g_find_item_handle, &found_bag,
                                                  &found_slot).ok == kQolOk);
    assert(found_bag == 2 && found_slot == 7);
    qol::contracts::QolItemHandle slot_identity{};
    g_raw_inventory_item = g_inventory_physical_item.data();
    assert(game.item_abi.item_identity_at(1, 2, &slot_identity).ok == kQolOk);
    assert(slot_identity.slot != 0 && slot_identity.generation != 0);
    void* native_item = nullptr;
    assert(game.item_abi.native_item_for_handle(slot_identity, &native_item).ok == kQolOk);
    assert(native_item == g_inventory_physical_item.data());
    g_raw_inventory_item = reinterpret_cast<void*>(0x9);
    fn_inven_find_item_slot = nullptr;
    int handle_item = 0;
    qol::contracts::QolItemHandle injected_handle{};
    assert(sdk.item_handle.register_item(
               &handle_item, qol::contracts::kQolItemHandleOwnerGame,
               &injected_handle) == qol::contracts::kQolItemHandleStatusOk);
    uint32_t registered_provider = UINT32_MAX;
    qol::contracts::QolItemHandle looked_up{};
    assert(sdk.item_handle.lookup_item(&handle_item, &looked_up, &registered_provider) ==
           qol::contracts::kQolItemHandleStatusOk);
    assert(looked_up.slot == injected_handle.slot &&
           registered_provider == qol::contracts::kQolItemHandleOwnerGame);
    assert(sdk.item_handle.rebind_item(
               &handle_item, qol::contracts::kQolItemHandleOwnerProvider) ==
           qol::contracts::kQolItemHandleStatusOk);
    qol::contracts::QolItemHandle removal_checkpoint{};
    assert(sdk.item_handle.begin_invalidation(
               &handle_item, qol::contracts::kQolItemHandleOwnerAny,
               &removal_checkpoint) == qol::contracts::kQolItemHandleStatusOk);
    assert(sdk.item_handle.finish_invalidation(
               removal_checkpoint, 0) == qol::contracts::kQolItemHandleStatusOk);
    assert(sdk.item_handle.begin_invalidation(
               &handle_item, qol::contracts::kQolItemHandleOwnerAny,
               &removal_checkpoint) == qol::contracts::kQolItemHandleStatusOk);
    assert(sdk.item_handle.finish_invalidation(
               removal_checkpoint, 1) == qol::contracts::kQolItemHandleStatusOk);
    assert(game.thread.frame_count() == 99);
    const auto& extension_bag_write = qol::game::extension_bag_write_port();
    assert(extension_bag_write.inventory.save_item_on_empty != nullptr);
    assert(extension_bag_write.inventory.serialize_item != nullptr);
    assert(extension_bag_write.inventory.load_item != nullptr);
    assert(extension_bag_write.equipment.equip_from_inventory != nullptr);
    assert(extension_bag_write.equipment.add_money != nullptr);
    assert(extension_bag_write.ui.refresh_item_area != nullptr);
    int32_t write_result = 0;
    assert(extension_bag_write.inventory.save_item_on_empty(nullptr, 0, &write_result).code ==
           kQolErrInvalidArgument);
    assert(extension_bag_write.equipment.add_money(-1, &write_result).code ==
           kQolErrInvalidArgument);
    void* created_item = nullptr;
    assert(extension_bag_write.inventory.create_item(-1, 0, 0, 0, &created_item).code ==
           kQolErrInvalidArgument);
    assert(game.character_write.max_hp != nullptr);
    assert(game.inventory_write.create_item != nullptr);
    assert(game.world_action.request_map_change != nullptr);
    assert(game.character_write.max_hp(nullptr, 1).code == kQolErrInvalidArgument);
    assert(game.inventory_write.remove_item(qol::contracts::kInvalidItemHandle).code ==
           kQolErrInvalidArgument);
    assert(game.world_action.minus_money(-1).code == kQolErrInvalidArgument);
    int32_t character_type = -1;
    assert(character_port_api.read.type(reinterpret_cast<void*>(0x1), &character_type).ok == kQolOk);
    assert(character_type == 2);
    int32_t party_member = 0;
    assert(character_extras_api.read.is_party_member(reinterpret_cast<void*>(0x1), &party_member).ok ==
           kQolOk);
    assert(party_member == 1);
    int32_t max_hp = 0;
    assert(character_extras_api.read.max_hp(reinterpret_cast<void*>(0x1), &max_hp).ok == kQolOk);
    assert(max_hp == 100);
    int32_t item_level = 0;
    assert(game.read_only.item.equip_level(reinterpret_cast<void*>(0x1), &item_level).ok == kQolOk);
    assert(item_level == 12);
    int32_t map_count = 0;
    assert(world_port_api.read.map_record_count(&map_count).ok == kQolOk);
    assert(map_count == 3);
    const char* map_name = nullptr;
    assert(world_port_api.read.map_name(1, &map_name).ok == kQolOk);
    assert(map_name != nullptr);
    int32_t hero_level = 0;
    assert(save_port_api.read.hero_level(0, &hero_level).ok == kQolOk);
    assert(hero_level == 10);
    const auto& quest = qol::game::quest_port();
    uint32_t quest_count = 0;
    assert(quest.read.active_slot_count(&quest_count).ok == kQolOk);
    assert(quest_count == 1);
    uint16_t active_quest_storage = 0x1234;
    g_active_quest = &active_quest_storage;
    int32_t active_quest_id = -1;
    assert(quest.read.active_quest_id(&active_quest_id).ok == kQolOk);
    assert(active_quest_id == 0x1234);
    g_active_quest = nullptr;
    assert(quest.read.active_quest_id(&active_quest_id).ok == kQolOk);
    assert(active_quest_id == -1);
    assert(quest.write.set_state(7, 2).ok == kQolOk);
    int32_t choice_state = -1;
    assert(choice.read.state_id(&choice_state).ok == kQolOk);
    assert(choice_state == 42);
    assert(choice.read.state_id != nullptr);
    assert(choice.read.selected_index != nullptr);
    assert(choice.read.text_owns != nullptr);
    assert(choice.write.write_items != nullptr);
    assert(choice.write.write_title != nullptr);
    assert(choice.write.write_focus != nullptr);
    assert(choice.write.push != nullptr);
    assert(choice.write.close_popup != nullptr);
    assert(choice.write.play_sound != nullptr);
    assert(character_port_api.read.type(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 2);
    assert(character_port_api.read.is_active_player_group(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 1);
    assert(character_extras_api.read.party_index(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 0);
    assert(character_extras_api.read.is_party_member(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 1);
    void* pointer_value = nullptr;
    assert(character_extras_api.read.summoner(reinterpret_cast<void*>(0x1), &pointer_value).ok ==
           kQolOk);
    assert(character_extras_api.read.level(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 10);
    assert(character_extras_api.read.merc_slot(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(character_extras_api.read.can_change_equip(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 1);
    assert(character_extras_api.read.menu_character(&pointer_value).ok == kQolOk);
    assert(pointer_value == reinterpret_cast<void*>(0x2));
    assert(character_extras_api.read.pool_slot(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(character_extras_api.read.max_hp(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 100);
    assert(character_extras_api.read.hp(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 80);
    assert(character_extras_api.read.can_equip(reinterpret_cast<void*>(0x1),
                                              reinterpret_cast<void*>(0x2), &value)
               .ok == kQolOk);
    assert(value == 1);
    assert(character_extras_api.read.find_equip_slot(reinterpret_cast<void*>(0x1),
                                                    reinterpret_cast<void*>(0x2), &value)
               .ok == kQolOk);
    assert(value == 2);
    assert(character_extras_api.read.equipped_item(reinterpret_cast<void*>(0x1), 2,
                                                  &pointer_value)
               .ok == kQolOk);
    assert(pointer_value == reinterpret_cast<void*>(0xA));
    assert(character_extras_api.read.has_empty_mercenary_slot(&value).ok == kQolOk);
    assert(value == 1);

    g_inventory_item_handle = qol::game::register_item(
        reinterpret_cast<void*>(0x9), qol::contracts::kQolItemHandleOwnerGame);
    assert(g_inventory_item_handle.slot != 0 && g_inventory_item_handle.generation != 0);
    assert(item_port_api.read.category(g_inventory_item_handle, &value).ok == kQolOk);
    assert(value == 4);
    assert(item_port_api.read.is_jewel_category(4, &value).ok == kQolOk && value == 1);
    assert(item_extras_api.read.ability_level(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 3);
    assert(game.read_only.item.rarity(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(item_port_api.read.is_real_equip(g_inventory_item_handle, &value).ok == kQolOk);
    assert(game.read_only.item.equip_level(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 12);
    assert(item_port_api.read.is_no_sell(4, &value).ok == kQolOk);
    assert(game.read_only.item.is_dice(4, &value).ok == kQolOk);
    assert(item_port_api.read.cumulate_count(g_inventory_item_handle, &value).ok == kQolOk);
    static const qol::contracts::QolItemCountApi item_count_provider{
        &adapter_item_count_canonical, &adapter_item_count_effective};
    qol::game::item_count_set_provider(&item_count_provider);
    qol::game::item_count_set_active_probe(&adapter_item_count_probe);
    g_item_count_probe = false;
    value = -1;
    assert(item_port_api.read.cumulate_count(g_inventory_item_handle, &value).ok == kQolOk);
    assert(value == 6);
    g_item_count_probe = true;
    g_adapter_canonical_count = 1023;
    value = -1;
    assert(item_port_api.read.cumulate_count(g_inventory_item_handle, &value).ok == kQolOk);
    assert(value == 1023);
    g_adapter_canonical_error = kQolErrUnsupported;
    assert(item_port_api.read.cumulate_count(g_inventory_item_handle, &value).code ==
           kQolErrUnsupported);
    g_adapter_canonical_error = kQolOk;
    value = -1;
    assert(item_port_api.read.effective_cumulate_count(g_inventory_item_handle, &value).ok ==
           kQolOk);
    assert(value == 88);
    qol::game::item_count_set_provider(nullptr);
    g_item_count_probe = false;
    assert(game.read_only.item.class_no_equip_bit(4, &value).ok == kQolOk);
    assert(game.read_only.item.equip_desc_role_type(&value).ok == kQolOk);
    assert(game.read_only.item.enchant_remaining(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 7);
    assert(game.read_only.item.socket_filled(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 2);
    assert(game.read_only.item.socket_total(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 5);
    assert(game.read_only.item.is_backpack_category(4, &value).ok == kQolOk && value == 1);
    assert(game.read_only.item.jewel_value(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 321);
    assert(game.read_only.item.jewel_attribute_type(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 17);
    assert(game.read_only.item.is_enchant_scroll(4, &value).ok == kQolOk && value == 1);
    assert(game.read_only.item.is_mercenary_seal(4, &value).ok == kQolOk && value == 1);
    assert(game.read_only.item.is_usable(4, &value).ok == kQolOk && value == 1);
    assert(game.read_only.item.jewel_value(nullptr, &value).code == kQolErrInvalidArgument);
    assert(game.read_only.item.is_backpack_category(0x400, &value).code ==
           kQolErrInvalidArgument);
    assert(game.read_only.item.enchant_remaining(nullptr, &value).code == kQolErrInvalidArgument);
    qol::game::g_item_class_table_ready = false;
    assert(game.read_only.item.enchant_remaining(reinterpret_cast<void*>(0x1), &value).code ==
           kQolErrNotReady);
    assert(game.read_only.item.is_backpack_category(4, &value).code == kQolErrNotReady);
    qol::game::g_item_class_table_ready = true;

    assert(world_port_api.read.state(&value).ok == kQolOk && value == 7);
    assert(world_port_api.read.map_record_count(&value).ok == kQolOk && value == 3);
    assert(world_port_api.read.current_map_id(&value).ok == kQolOk && value == 1);
    assert(world_port_api.read.map_name(1, &map_name).ok == kQolOk);
    assert(world_port_api.read.active_player_direction(&value).ok == kQolOk && value == 2);
    assert(world_port_api.read.game_state_gate(&value).ok == kQolOk && value == 1);

    uint32_t u32_value = 0;
    int64_t i64_value = 0;
    assert(craft_port_api.read.ui_type(&u32_value).ok == kQolOk && u32_value == 1);
    assert(craft_port_api.read.panel_state(&value).ok == kQolOk && value == 2);
    assert(craft_port_api.read.mix_type(&u32_value).ok == kQolOk && u32_value == 3);
    assert(craft_port_api.read.selected_stuff_index(&i64_value).ok == kQolOk && i64_value == 0);
    assert(craft_port_api.read.selected_stuff_item(&pointer_value).ok == kQolOk);
    assert(craft_port_api.read.stuff_item(0, &pointer_value).ok == kQolOk);
    assert(craft_port_api.read.category_held_count(4, &value).ok == kQolOk && value == 5);
    assert(craft_port_api.read.character_level(&value).ok == kQolOk && value == 10);
    assert(craft_port_api.read_only.get_ui_type(&u32_value).ok == kQolOk && u32_value == 1);
    assert(craft_port_api.read_only.stuff_slot_control(0, &pointer_value).ok == kQolOk);
    assert(craft_port_api.read_only.target_slot_item(&pointer_value).ok == kQolOk);
    assert(craft_port_api.read_only.selected_inventory_item(&pointer_value).ok == kQolOk);
    assert(craft_port_api.read_only.selected_recipe(0, &i64_value).ok == kQolOk);
    assert(craft_port_api.read_only.stuff_slot_count(&u32_value).ok == kQolOk && u32_value == 3);

    assert(save_port_api.read.slot_state(0, &value).ok == kQolOk && value == 1);
    assert(save_port_api.read.slot_exists(0, &value).ok == kQolOk && value == 1);
    assert(save_port_api.read.slot_hero_index(0, &value).ok == kQolOk && value == 0);
    assert(save_port_api.read.hero_level(0, &value).ok == kQolOk && value == 10);
    assert(save_port_api.read.hero_class(0, &value).ok == kQolOk && value == 2);
    int32_t data_size = 0;
    assert(save_port_api.read.load_data(0, &pointer_value, &data_size).ok == kQolOk);
    assert(save_port_api.read.hub_key(&map_name).ok == kQolOk);
    assert(save_port_api.read.is_ok(&value).ok == kQolOk && value == 1);
    assert(save_port_api.read.current_slot(&value).code == kQolErrNotReady);
    assert(save_port_api.read.map_id(0, &value).ok == kQolOk && value == 1);
    assert(save_call_port_api.write.free_data(nullptr).code == kQolErrInvalidArgument);
    assert(save_call_port_api.write.encrypt(nullptr, 1, 0, "key", &value).code ==
           kQolErrInvalidArgument);
    assert(save_call_port_api.write.mark_stale().ok == kQolOk);

    assert(quest.read.state(&value).code == kQolErrUnsupported);
    assert(quest.read.active_slot_count(&u32_value).ok == kQolOk && u32_value == 1);
    assert(quest.read.active_slot_id(0, &u32_value).ok == kQolOk && u32_value == 7);
    assert(quest.read.total_count(&u32_value).ok == kQolOk && u32_value == 10);
    assert(quest.read.quest_state(7, &value).ok == kQolOk && value == 1);
    assert(quest.read.is_complete(7, &value).ok == kQolOk && value == 1);
    assert(choice.read.state_id(&value).ok == kQolOk && value == 42);
    assert(choice.read.selected_index(&value).ok == kQolOk && value == 0);
    const char* choice_items[] = {"a"};
    assert(choice.read.text_owns(choice_items, 1, &value).ok == kQolOk && value == 1);
    assert(choice.read.selected_index(nullptr).code == kQolErrInvalidArgument);
    assert(choice.write.write_items(nullptr, 1).code == kQolErrInvalidArgument);
    assert(choice.write.write_title(nullptr).code == kQolErrInvalidArgument);
    assert(choice.write.write_focus(-1).code == kQolErrInvalidArgument);
    qol::contracts::QolInventoryItemRef inventory_ref{};
    assert(item_port_api.read.item_ref_at(1, 2, &inventory_ref).ok == kQolOk);
    assert(inventory_ref.bag == 1 && inventory_ref.slot == 2 &&
           inventory_ref.item.slot != 0 && inventory_ref.item.generation != 0);
    const qol::contracts::QolItemHandle saved_inventory_handle = g_inventory_item_handle;
    g_inventory_item_handle = qol::contracts::kInvalidItemHandle;
    g_raw_inventory_item = g_inventory_new_item.data();
    qol::contracts::QolInventoryItemRef newly_registered_ref{};
    assert(item_port_api.read.item_ref_at(4, 7, &newly_registered_ref).ok == kQolOk);
    assert(newly_registered_ref.item.slot != 0 && newly_registered_ref.item.generation != 0);
    qol::contracts::QolItemInventoryView physical_view{};
    assert(item_port_api.read.inventory_item_view(newly_registered_ref.item, &physical_view).ok ==
           kQolOk);
    g_raw_inventory_item = g_inventory_physical_item.data();
    QolResult off_thread_ref_result{};
    std::thread off_thread_reader([&]() {
        qol::contracts::QolInventoryItemRef off_thread_ref{};
        off_thread_ref_result = item_port_api.read.item_ref_at(4, 8, &off_thread_ref);
    });
    off_thread_reader.join();
    assert(off_thread_ref_result.code == kQolErrBlocked);
    g_raw_inventory_item = reinterpret_cast<void*>(0x9);
    g_inventory_item_handle = saved_inventory_handle;
    assert(item_port_api.read.item_ref_at(1, 2, nullptr).code == kQolErrInvalidArgument);
    g_inventory_mode = FakeInventoryMode::kEmpty;
    assert(item_port_api.read.item_ref_at(1, 2, &inventory_ref).code == kQolErrNotFound);
    g_inventory_mode = FakeInventoryMode::kNotReady;
    g_inven = nullptr;
    assert(item_port_api.read.item_ref_at(1, 2, &inventory_ref).code == kQolErrNotReady);
    g_inven = reinterpret_cast<void*>(1);
    int inventory_callback_calls = 0;
    g_inventory_mode = FakeInventoryMode::kReady;
    assert(inventory_extras_api.read.for_each_bag_slot(&inventory_callback,
                                                      &inventory_callback_calls)
               .ok == kQolOk);
    assert(inventory_callback_calls == 1);
    assert(inventory_extras_api.read.for_each_bag_slot(nullptr, nullptr).code ==
           kQolErrInvalidArgument);
    assert(inventory_extras_api.read.bag_size(2, &value).ok == kQolOk && value == 6);
    assert(inventory_extras_api.read.is_empty_bag(2, &value).ok == kQolOk && value == 0);
    assert(inventory_extras_api.read.find_save_slot(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 3);
    g_inventory_mode = FakeInventoryMode::kNotReady;
    g_inven = nullptr;
    assert(inventory_extras_api.read.for_each_bag_slot(&inventory_callback,
                                                      &inventory_callback_calls)
                .code == kQolErrNotReady);
    g_inventory_mode = FakeInventoryMode::kReady;
    g_inven = reinterpret_cast<void*>(1);
    assert(world_port_api.read.game_state_gate(nullptr).code == kQolErrInvalidArgument);
    g_world_game_state_gate = -1;
    assert(world_port_api.read.game_state_gate(&value).code == kQolErrNotReady);
    g_world_game_state_gate = 0;
    return 0;
}
