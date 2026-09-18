#include <cassert>
#include <cstddef>
#include <cstdint>

#include "contracts/panels.h"
#include "core/native/game_variant.h"
#include "game_access.h"
#include "game/access/attr_range_ui.h"
#include "game/access/char.h"
#include "game/access/craft.h"
#include "game/access/quest.h"
#include "game/access/save.h"
#include "game/access/symbols.h"
#include "game/access/world.h"
#include "game/clock.h"
#include "host/adapters.h"
#include "infra/capabilities.h"
#include "infra/events.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "infra/text_scope.h"

uintptr_t g_base = 0;

decltype(fn_get_rarity) fn_get_rarity = nullptr;
decltype(fn_item_is_no_sell) fn_item_is_no_sell = nullptr;
decltype(fn_is_dice) fn_is_dice = nullptr;
decltype(fn_get_cumulate_count) fn_get_cumulate_count = nullptr;
decltype(fn_get_bit) fn_get_bit = nullptr;
decltype(fn_inven_remove_item_data) fn_inven_remove_item_data = nullptr;
decltype(fn_remove_item) fn_remove_item = nullptr;
decltype(fn_item_create_perfect_item) fn_item_create_perfect_item = nullptr;
decltype(fn_create_item) fn_create_item = nullptr;
decltype(fn_inven_save_item) fn_inven_save_item = nullptr;
decltype(fn_itempool_free) fn_itempool_free = nullptr;
decltype(fn_sound_system_play) fn_sound_system_play = nullptr;
decltype(fn_uimix_init_mixing_state) fn_uimix_init_mixing_state = nullptr;
decltype(fn_uimix_reset_stuff_item_control) fn_uimix_reset_stuff_item_control = nullptr;
decltype(fn_ui_mix_refresh_inven_item) fn_ui_mix_refresh_inven_item = nullptr;
decltype(fn_uimix_set_type) fn_uimix_set_type = nullptr;
decltype(fn_popup_create_ok_from_textdata) fn_popup_create_ok_from_textdata = nullptr;
decltype(fn_ui_desc_set_off) fn_ui_desc_set_off = nullptr;
decltype(fn_popup_create_yesno_from_textdata) fn_popup_create_yesno_from_textdata = nullptr;
decltype(fn_control_object_get_cursor) fn_control_object_get_cursor = nullptr;
decltype(fn_control_object_get_data) fn_control_object_get_data = nullptr;
decltype(fn_control_item_get_item) fn_control_item_get_item = nullptr;

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
uint8_t craft_panel_state() { return 2; }
uint32_t craft_mix_type() { return 3; }
int64_t craft_selected_stuff_index() { return 0; }
void* craft_stuff_item(size_t) { return reinterpret_cast<void*>(0x1); }
void* craft_stuff_slot_control(size_t) { return reinterpret_cast<void*>(0x3); }
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
int world_state() { return 7; }
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

}  // namespace qol::game

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

bool panel_open() { return true; }
void panel_close() {}
bool panel_active() { return true; }
const char* panel_block_reason() { return nullptr; }

}  // namespace

int main() {
    const qol::contracts::QolHostSdk& sdk = qol::host::g_host_sdk;
    assert(sdk.abi.size == sizeof(qol::contracts::QolHostSdk));
    assert(sdk.abi.major == qol::contracts::kHostSdkMajor);
    assert(sdk.ui == nullptr);
    assert(sdk.config.get_bool != nullptr);
    assert(sdk.scheduler.add != nullptr);
    assert(sdk.events.subscribe != nullptr);

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
    assert(game.abi.size == sizeof(qol::contracts::QolGameHost));
    assert(game.target.query_variant != nullptr);
    qol::contracts::QolTargetVariant variant{};
    assert(game.target.query_variant(&variant).ok == kQolOk);
    assert(variant.id == static_cast<uint32_t>(qol::GameSeries::kOriginal));
    assert(variant.capabilities == qol::kCapWarehouse);
    assert(game.abi.minor == qol::contracts::kGameHostMinor);
    qol::host::game_host_mark_main_thread();
    assert(game.thread.is_main_thread() == 1);
    assert(game.thread.frame_count() == 99);
    assert(game.ui_primitive.insert_inline_color != nullptr);
    char bounded_text[] = "value";
    char* bounded_builder = bounded_text;
    assert(game.ui_primitive.insert_inline_color(&bounded_builder, 4, 'Y').ok ==
           kQolErrBlocked);
    assert(bounded_builder == bounded_text);
    int32_t character_type = -1;
    assert(game.read.character.type(reinterpret_cast<void*>(0x1), &character_type).ok == kQolOk);
    assert(character_type == 2);
    int32_t party_member = 0;
    assert(game.extra.character.is_party_member(reinterpret_cast<void*>(0x1), &party_member).ok ==
           kQolOk);
    assert(party_member == 1);
    int32_t max_hp = 0;
    assert(game.extra.character.max_hp(reinterpret_cast<void*>(0x1), &max_hp).ok == kQolOk);
    assert(max_hp == 100);
    int32_t item_level = 0;
    assert(game.extra.item.equip_level(reinterpret_cast<void*>(0x1), &item_level).ok == kQolOk);
    assert(item_level == 12);
    int32_t map_count = 0;
    assert(game.extra.world.map_record_count(&map_count).ok == kQolOk);
    assert(map_count == 3);
    const char* map_name = nullptr;
    assert(game.extra.world.map_name(1, &map_name).ok == kQolOk);
    assert(map_name != nullptr);
    int32_t hero_level = 0;
    assert(game.extra.save.hero_level(0, &hero_level).ok == kQolOk);
    assert(hero_level == 10);
    uint32_t quest_count = 0;
    assert(game.extra.quest.active_slot_count(&quest_count).ok == kQolOk);
    assert(quest_count == 1);
    assert(game.extra.quest_write.set_state(7, 2).ok == kQolOk);
    int32_t choice_state = -1;
    assert(game.extra.choice.state_id(&choice_state).ok == kQolOk);
    assert(choice_state == 42);
    assert(game.call.call_known(1, nullptr, 0, nullptr, 0).ok == kQolErrUnsupported);
    return 0;
}
