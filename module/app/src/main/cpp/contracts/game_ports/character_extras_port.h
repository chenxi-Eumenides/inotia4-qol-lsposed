#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolCharacterExtrasReadApi {
    QolResult (*party_index)(const void* character, int32_t* out_index);
    QolResult (*is_party_member)(const void* character, int32_t* out_member);
    QolResult (*summoner)(const void* character, void** out_summoner);
    QolResult (*level)(const void* character, int32_t* out_level);
    QolResult (*merc_slot)(const void* character, int32_t* out_slot);
    QolResult (*can_change_equip)(const void* character, int32_t* out_allowed);
    QolResult (*menu_character)(void** out_character);
    QolResult (*pool_slot)(const void* character, int32_t* out_slot);
    QolResult (*max_hp)(const void* character, int32_t* out_value);
    QolResult (*hp)(const void* character, int32_t* out_value);
    QolResult (*can_equip)(const void* character, const void* item, int32_t* out_allowed);
    QolResult (*find_equip_slot)(const void* character, const void* item, int32_t* out_slot);
    QolResult (*equipped_item)(const void* character, int32_t slot, void** out_item);
    QolResult (*has_empty_mercenary_slot)(int32_t* out_available);
    QolResult (*can_apply_stuff)(const void* target, const void* material, int32_t* out_allowed);
    QolResult (*party_member)(int32_t role, void** out_character);
};

struct QolCharacterExtrasPort {
    QolVersion abi_version;
    QolCharacterExtrasReadApi read;
};

}  // namespace qol::contracts
