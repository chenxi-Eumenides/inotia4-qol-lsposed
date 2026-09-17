#include "game/access/char.h"

#include <cstddef>

#include "game_access.h"

namespace qol::game {

bool char_attr_is_max_hp(int32_t attr_id) {
    return attr_id == ATTR_MAX_HP;
}

int char_type(const void* ch) {
    if (ch == nullptr) return -1;
    return *reinterpret_cast<const int8_t*>(reinterpret_cast<const uint8_t*>(ch) + C_TYPE);
}

bool char_read_max_hp(const void* ch, int32_t* out) {
    if (ch == nullptr || out == nullptr) return false;
    *out = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(ch) + C_MAX_HP);
    return true;
}

bool char_write_max_hp(void* ch, int32_t value) {
    if (ch == nullptr) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_MAX_HP) = value;
    return true;
}

bool char_read_hp(const void* ch, int32_t* out) {
    if (ch == nullptr || out == nullptr) return false;
    *out = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(ch) + C_HP);
    return true;
}

bool char_write_hp(void* ch, int32_t value) {
    if (ch == nullptr) return false;
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(ch) + C_HP) = value;
    return true;
}

bool char_pool_slot(const void* ch, int* out_slot) {
    if (g_base == 0 || ch == nullptr || out_slot == nullptr) return false;
    const uint8_t* pool = *reinterpret_cast<const uint8_t* const*>(g_base + G_CHAR_POOL_VMA);
    if (pool == nullptr) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ch);
    if (p < pool) return false;
    const ptrdiff_t delta = p - pool;
    if (delta % static_cast<ptrdiff_t>(C_OBJ_SIZE) != 0) return false;
    const ptrdiff_t slot = delta / static_cast<ptrdiff_t>(C_OBJ_SIZE);
    if (slot < 0 || slot >= C_CHARSYSTEM_POOL_SLOTS) return false;
    *out_slot = static_cast<int>(slot);
    return true;
}

bool char_faction_deps_ready() {
    return char_party_index_resolved() && char_active_group_resolved() &&
           char_summoner_resolved();
}

bool char_party_index_resolved() {
    return fn_char_get_party_index != nullptr;
}

bool char_active_group_resolved() {
    return fn_char_is_active_player_group != nullptr;
}

bool char_summoner_resolved() {
    return fn_char_get_summoner != nullptr;
}

int char_party_index(const void* ch) {
    if (ch == nullptr || fn_char_get_party_index == nullptr) return -1;
    return fn_char_get_party_index(const_cast<void*>(ch));
}

bool char_is_party_member(void* ch) {
    return fn_char_get_party_index != nullptr && fn_char_get_party_index(ch) != -1;
}

bool char_is_active_player_group(void* ch) {
    return fn_char_is_active_player_group != nullptr && fn_char_is_active_player_group(ch) != 0;
}

void* char_get_summoner(void* ch) {
    return fn_char_get_summoner != nullptr ? fn_char_get_summoner(ch) : nullptr;
}

int char_level(const void* ch) {
    if (ch == nullptr) return -1;
    return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(ch) + C_LEVEL);
}

int char_merc_slot(const void* ch) {
    if (ch == nullptr) return -1;
    return *reinterpret_cast<const int8_t*>(reinterpret_cast<const uint8_t*>(ch) + C_MERC_SLOT);
}

bool char_can_change_equip(const void* ch) {
    return fn_char_can_change_equip != nullptr && ch != nullptr &&
           fn_char_can_change_equip(const_cast<void*>(ch)) != 0;
}

void* char_menu_character() {
    return fn_get_menu_character != nullptr ? fn_get_menu_character() : nullptr;
}

bool char_can_change_equip_resolved() {
    return fn_char_can_change_equip != nullptr;
}

bool char_menu_character_resolved() {
    return fn_get_menu_character != nullptr;
}

}  // namespace qol::game
