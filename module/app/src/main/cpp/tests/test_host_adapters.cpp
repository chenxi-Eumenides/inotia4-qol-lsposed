#include <cassert>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

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
#include "infra/store.h"
#include "infra/text_scope.h"

uintptr_t g_base = 0;

std::string op_ok() { return "{\"ok\":true}"; }
std::string op_err(const char* message) {
    return std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
}

enum class FakeInventoryMode { kReady, kEmpty, kNotReady };
FakeInventoryMode g_inventory_mode = FakeInventoryMode::kReady;
int g_world_game_state_gate = 0;

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

int fake_item_rarity(void*) { return 9; }
int fake_item_no_sell(int32_t) { return 0; }
int fake_item_dice(int32_t) { return 0; }
int fake_item_cumulate_count(void*) { return 6; }

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
            reinterpret_cast<void*>(0x9)};
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
        reinterpret_cast<void*>(0x9)};
    (void)callback(&ref, ctx);
    return contracts::ok();
}

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

int32_t inventory_callback(const qol::contracts::QolInventoryItemRef* ref, void* ctx) {
    assert(ref != nullptr);
    assert(ref->kind == static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kOriginal));
    assert(ref->category == 28 && ref->count == 4);
    ++*static_cast<int*>(ctx);
    return 0;
}

bool panel_open() { return true; }
void panel_close() {}
bool panel_active() { return true; }
const char* panel_block_reason() { return nullptr; }

}  // namespace

int main() {
    fn_get_rarity = &fake_item_rarity;
    fn_item_is_no_sell = &fake_item_no_sell;
    fn_is_dice = &fake_item_dice;
    fn_get_cumulate_count = &fake_item_cumulate_count;
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

    static_assert(sizeof(qol::contracts::QolStoreApi) == sizeof(void*) * 7,
                  "store ABI member count changed");

    uint32_t container_size = 0;
    assert(sdk.store.read_container(0, nullptr, 0, &container_size).code == kQolErrNotReady);
    assert(sdk.store.read_container(-1, nullptr, 0, &container_size).code ==
           kQolErrInvalidArgument);
    assert(sdk.store.read_container(0, nullptr, 0, nullptr).code == kQolErrInvalidArgument);
    assert(sdk.store.import_container(0, nullptr, 1).code == kQolErrInvalidArgument);
    assert(sdk.store.delete_slot(-1).code == kQolErrInvalidArgument);

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
    assert(game.abi.size == sizeof(qol::contracts::QolGameHost));
    assert(game.target.query_variant != nullptr);
    qol::contracts::QolTargetVariant variant{};
    assert(game.target.query_variant(&variant).ok == kQolOk);
    assert(variant.id == static_cast<uint32_t>(qol::GameSeries::kOriginal));
    assert(variant.capabilities == qol::kCapWarehouse);
    assert(game.abi.minor == qol::contracts::kGameHostMinor);
    static_assert(std::is_standard_layout<qol::contracts::QolReadOnlyApi>::value,
                  "read-only face must remain standard layout");
    static_assert(offsetof(qol::contracts::QolGameHost, read_only) ==
                      offsetof(qol::contracts::QolGameHost, extra) +
                          sizeof(qol::contracts::QolGameHostExtraApi),
                  "read-only face must be append-only");
    assert(reinterpret_cast<const char*>(&game.read_only) ==
           reinterpret_cast<const char*>(&game.extra) + sizeof(game.extra));
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
    assert(game.read_only.character.type(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 2);
    assert(game.read_only.character.is_active_player_group(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 1);
    assert(game.read_only.character.party_index(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 0);
    assert(game.read_only.character.is_party_member(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 1);
    void* pointer_value = nullptr;
    assert(game.read_only.character.summoner(reinterpret_cast<void*>(0x1), &pointer_value).ok ==
           kQolOk);
    assert(game.read_only.character.level(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 10);
    assert(game.read_only.character.merc_slot(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(game.read_only.character.can_change_equip(reinterpret_cast<void*>(0x1), &value).ok ==
           kQolOk);
    assert(value == 1);
    assert(game.read_only.character.menu_character(&pointer_value).ok == kQolOk);
    assert(pointer_value == reinterpret_cast<void*>(0x2));
    assert(game.read_only.character.pool_slot(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(game.read_only.character.max_hp(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 100);
    assert(game.read_only.character.hp(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 80);

    assert(game.read_only.item.category(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 4);
    assert(game.read_only.item.is_jewel_category(4, &value).ok == kQolOk && value == 1);
    assert(game.read_only.item.ability_level(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 3);
    assert(game.read_only.item.rarity(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(game.read_only.item.is_real_equip(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(game.read_only.item.equip_level(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
    assert(value == 12);
    assert(game.read_only.item.is_no_sell(4, &value).ok == kQolOk);
    assert(game.read_only.item.is_dice(4, &value).ok == kQolOk);
    assert(game.read_only.item.cumulate_count(reinterpret_cast<void*>(0x1), &value).ok == kQolOk);
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
    assert(game.read_only.item.jewel_value(nullptr, &value).code == kQolErrInvalidArgument);
    assert(game.read_only.item.is_backpack_category(0x400, &value).code ==
           kQolErrInvalidArgument);
    assert(game.read_only.item.enchant_remaining(nullptr, &value).code == kQolErrInvalidArgument);
    qol::game::g_item_class_table_ready = false;
    assert(game.read_only.item.enchant_remaining(reinterpret_cast<void*>(0x1), &value).code ==
           kQolErrNotReady);
    assert(game.read_only.item.is_backpack_category(4, &value).code == kQolErrNotReady);
    qol::game::g_item_class_table_ready = true;

    int32_t x = 0;
    int32_t y = 0;
    assert(game.read_only.world.position(reinterpret_cast<void*>(0x1), &x, &y).code ==
           kQolErrUnsupported);
    assert(game.read_only.world.state(&value).ok == kQolOk && value == 7);
    assert(game.read_only.world.map_record_count(&value).ok == kQolOk && value == 3);
    assert(game.read_only.world.current_map_id(&value).ok == kQolOk && value == 1);
    assert(game.read_only.world.map_name(1, &map_name).ok == kQolOk);
    assert(game.read_only.world.active_player_direction(&value).ok == kQolOk && value == 2);
    int64_t money = 0;
    assert(game.read_only.world.money(&money).ok == kQolOk && money == 1000);
    assert(game.read_only.world.game_state_gate(&value).ok == kQolOk && value == 1);

    uint32_t u32_value = 0;
    int64_t i64_value = 0;
    assert(game.read_only.craft.ui_type(&u32_value).ok == kQolOk && u32_value == 1);
    assert(game.read_only.craft.panel_state(&value).ok == kQolOk && value == 2);
    assert(game.read_only.craft.mix_type(&u32_value).ok == kQolOk && u32_value == 3);
    assert(game.read_only.craft.selected_stuff_index(&i64_value).ok == kQolOk && i64_value == 0);
    assert(game.read_only.craft.selected_stuff_item(&pointer_value).ok == kQolOk);
    assert(game.read_only.craft.stuff_item(0, &pointer_value).ok == kQolOk);
    assert(game.read_only.craft.category_held_count(4, &value).ok == kQolOk && value == 5);
    assert(game.read_only.craft.character_level(&value).ok == kQolOk && value == 10);
    assert(game.read_only.craft.stuff_slot_control(0, &pointer_value).ok == kQolOk);
    assert(game.read_only.craft.target_slot_item(&pointer_value).ok == kQolOk);
    assert(game.read_only.craft.selected_inventory_item(&pointer_value).ok == kQolOk);
    assert(game.read_only.craft.selected_recipe(0, &i64_value).ok == kQolOk);
    assert(game.read_only.craft.stuff_slot_count(&u32_value).ok == kQolOk && u32_value == 3);

    assert(game.read_only.save.slot_state(0, &value).ok == kQolOk && value == 1);
    assert(game.read_only.save.slot_exists(0, &value).ok == kQolOk && value == 1);
    assert(game.read_only.save.slot_hero_index(0, &value).ok == kQolOk && value == 0);
    assert(game.read_only.save.hero_level(0, &value).ok == kQolOk && value == 10);
    assert(game.read_only.save.hero_class(0, &value).ok == kQolOk && value == 2);
    int32_t data_size = 0;
    assert(game.read_only.save.load_data(0, &pointer_value, &data_size).ok == kQolOk);
    assert(game.read_only.save.hub_key(&map_name).ok == kQolOk);

    assert(game.read_only.quest.state(&value).code == kQolErrUnsupported);
    assert(game.read_only.quest.active_slot_count(&u32_value).ok == kQolOk && u32_value == 1);
    assert(game.read_only.quest.active_slot_id(0, &u32_value).ok == kQolOk && u32_value == 7);
    assert(game.read_only.quest.total_count(&u32_value).ok == kQolOk && u32_value == 10);
    assert(game.read_only.quest.quest_state(7, &value).ok == kQolOk && value == 1);
    assert(game.read_only.quest.is_complete(7, &value).ok == kQolOk && value == 1);
    const char* choice_items[] = {"a"};
    assert(game.read_only.choice.state_id(&value).ok == kQolOk && value == 42);
    assert(game.read_only.choice.selected_index(&value).ok == kQolOk && value == 0);
    assert(game.read_only.choice.text_owns(choice_items, 1, &value).ok == kQolOk && value == 1);
    assert(game.read_only.choice.selected_index(nullptr).code == kQolErrInvalidArgument);
    qol::contracts::QolInventoryItemRef inventory_ref{};
    assert(game.read_only.inventory.item_ref_at(1, 2, &inventory_ref).ok == kQolOk);
    assert(inventory_ref.bag == 1 && inventory_ref.slot == 2 && inventory_ref.native_item != nullptr);
    assert(game.read_only.inventory.item_ref_at(1, 2, nullptr).code == kQolErrInvalidArgument);
    g_inventory_mode = FakeInventoryMode::kEmpty;
    assert(game.read_only.inventory.item_ref_at(1, 2, &inventory_ref).code == kQolErrNotFound);
    g_inventory_mode = FakeInventoryMode::kNotReady;
    assert(game.read_only.inventory.item_ref_at(1, 2, &inventory_ref).code == kQolErrNotReady);
    int inventory_callback_calls = 0;
    g_inventory_mode = FakeInventoryMode::kReady;
    assert(game.read_only.inventory.for_each_bag_slot(&inventory_callback,
                                                      &inventory_callback_calls)
               .ok == kQolOk);
    assert(inventory_callback_calls == 1);
    assert(game.read_only.inventory.for_each_bag_slot(nullptr, nullptr).code ==
           kQolErrInvalidArgument);
    g_inventory_mode = FakeInventoryMode::kNotReady;
    assert(game.read_only.inventory.for_each_bag_slot(&inventory_callback,
                                                      &inventory_callback_calls)
               .code == kQolErrNotReady);
    g_inventory_mode = FakeInventoryMode::kReady;
    assert(game.read_only.world.game_state_gate(nullptr).code == kQolErrInvalidArgument);
    g_world_game_state_gate = -1;
    assert(game.read_only.world.game_state_gate(&value).code == kQolErrNotReady);
    g_world_game_state_gate = 0;
    assert(game.call.call_known(1, nullptr, 0, nullptr, 0).ok == kQolErrUnsupported);
    return 0;
}
