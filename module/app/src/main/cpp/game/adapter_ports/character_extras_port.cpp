#include "game/adapter_ports/character_extras_port.h"

#include <cstring>

#include "contracts/game_ports/ports.h"
#include "game/access/char.h"
#include "game/access/craft.h"

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_party_index(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_party_index_resolved()) return not_ready();
    const int value = char_party_index(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_is_party_member(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_faction_deps_ready()) return not_ready();
    *out = char_is_party_member(const_cast<void*>(character)) ? 1 : 0;
    return contracts::ok();
}

QolResult read_summoner(const void* character, void** out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_summoner_resolved()) return not_ready();
    *out = char_get_summoner(const_cast<void*>(character));
    return contracts::ok();
}

QolResult read_level(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = char_level(character);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_merc_slot(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = char_merc_slot(character);
    return contracts::ok();
}

QolResult read_can_change_equip(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_can_change_equip_resolved()) return not_ready();
    *out = char_can_change_equip(character) ? 1 : 0;
    return contracts::ok();
}

QolResult read_menu_character(void** out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    if (!char_menu_character_resolved()) return not_ready();
    *out = char_menu_character();
    return *out == nullptr ? not_ready() : contracts::ok();
}

QolResult read_pool_slot(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    int slot = 0;
    if (!char_pool_slot(character, &slot)) return not_ready();
    *out = slot;
    return contracts::ok();
}

QolResult read_max_hp(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return char_read_max_hp(character, out) ? contracts::ok() : not_ready();
}

QolResult read_hp(const void* character, int32_t* out) {
    if (character == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return char_read_hp(character, out) ? contracts::ok() : not_ready();
}

QolResult read_can_equip(const void* character, const void* item, int32_t* out) {
    if (character == nullptr || item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_can_equip_resolved()) return not_ready();
    *out = char_can_equip(character, item) ? 1 : 0;
    return contracts::ok();
}

QolResult read_find_equip_slot(const void* character, const void* item, int32_t* out) {
    if (character == nullptr || item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_find_equip_slot_resolved()) return not_ready();
    *out = char_find_equip_slot(character, item);
    return *out < 0 ? error_result(kQolErrNotFound, "equip slot not found") : contracts::ok();
}

QolResult read_equipped_item(const void* character, int32_t slot, void** out) {
    if (character == nullptr || out == nullptr || slot < 0 || slot >= 10) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!char_equipped_item_resolved()) return not_ready();
    *out = char_equipped_item(character, slot);
    return contracts::ok();
}

QolResult read_has_empty_mercenary_slot(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    const int value = char_has_empty_mercenary_slot();
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_can_apply_stuff(const void* target, const void* material, int32_t* out) {
    if (target == nullptr || material == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int32_t allowed = craft_is_apply_stuff(const_cast<void*>(target), const_cast<void*>(material));
    if (allowed < 0) return not_ready();
    *out = allowed;
    return contracts::ok();
}

QolResult read_party_member(int32_t role, void** out) {
    if (out == nullptr || role < 0 || role >= 3) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = char_party_member(role);
    return *out == nullptr ? not_ready() : contracts::ok();
}

const contracts::QolCharacterExtrasPort kPort{
    contracts::kQolPortCharacterExtrasVersion,
    {&read_party_index, &read_is_party_member, &read_summoner, &read_level, &read_merc_slot,
     &read_can_change_equip, &read_menu_character, &read_pool_slot, &read_max_hp, &read_hp,
     &read_can_equip, &read_find_equip_slot, &read_equipped_item, &read_has_empty_mercenary_slot,
     &read_can_apply_stuff, &read_party_member},
};

}  // namespace

const contracts::QolCharacterExtrasPort& character_extras_port() { return kPort; }

}  // namespace qol::game
