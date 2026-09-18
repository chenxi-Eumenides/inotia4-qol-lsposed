#include "host/adapters.h"

#include <atomic>
#include <cstring>
#include <pthread.h>

#include "game/access/attr_range_ui.h"
#include "game/access/char.h"
#include "game/access/craft.h"
#include "game/access/item_class.h"
#include "game/access/quest.h"
#include "game/access/save.h"
#include "game/access/symbols.h"
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

QolResult read_attr_range(int32_t type, const void* item, int32_t option_index, int32_t* out_min,
                          int32_t* out_max) {
    if (item == nullptr || out_min == nullptr || out_max == nullptr || option_index < 0) {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    int minimum = 0;
    int maximum = 0;
    bool ok = false;
    if (type == 0) {
        ok = qol::game::attr_probe_option_range(option_index, qol::game::attr_ability_level(item), 1,
                                                const_cast<void*>(item), &minimum, &maximum);
    } else if (type == 1) {
        ok = qol::game::attr_probe_jewel_range(option_index, const_cast<void*>(item), &minimum,
                                               &maximum);
    } else {
        return error_result(kQolErrInvalidArgument, kInvalidArgument);
    }
    if (!ok) return not_ready();
    *out_min = minimum;
    *out_max = maximum;
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
    host.read.attr_range = {&read_attr_range};
    host.write.craft = {&write_craft_stuff_need, &write_craft_stuff_item,
                        &write_craft_target_item, &write_craft_cost_zero};
    host.call = {&game_call_known};
    host.ui_primitive = {};
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
