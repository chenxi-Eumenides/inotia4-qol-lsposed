#include "host/character_state_port.h"

#include "contracts/character_attributes.h"
#include "contracts/game_ports/ports.h"
#include "game/adapter_ports/game_host_port.h"
#include "game/ops/character_attributes_read.h"
#include "game/ops/character_state_read.h"

namespace qol::host {
namespace {

QolResult invalid_argument() {
    return contracts::error(kQolErrInvalidArgument, "invalid argument", 17);
}

QolResult blocked_thread() {
    return contracts::error(kQolErrBlocked, "wrong thread", 12);
}

QolResult not_found() {
    return contracts::error(kQolErrNotFound, "character not found", 21);
}

QolResult party(contracts::QolCharacterStateParty* out) {
    return qol::game::ops::read_character_party(out);
}

QolResult player(contracts::QolCharacterStatePlayer* out) {
    return qol::game::ops::read_character_player(out);
}

QolResult skills(contracts::QolCharacterStateSkills* out) {
    return qol::game::ops::read_character_skills(out);
}

QolResult attributes(contracts::QolCharacterRef ref,
                     contracts::QolCharacterAttributeSnapshot* out) {
    if (!qol::game::ops::character_ref_valid(ref) || out == nullptr) return invalid_argument();
    if (!qol::game::game_host_is_main_thread()) return blocked_thread();
    if (!qol::game::ops::read_character_attribute_snapshot(ref, out)) return not_found();
    return contracts::ok();
}

QolResult derived(contracts::QolCharacterRef ref, int32_t kind, int32_t* out) {
    if (!qol::game::ops::character_ref_valid(ref) || out == nullptr ||
        kind < contracts::kQolCharacterDerivedNormalDamage ||
        kind > contracts::kQolCharacterDerivedDisplayDamageReduceRate) {
        return invalid_argument();
    }
    if (!qol::game::game_host_is_main_thread()) return blocked_thread();
    if (!qol::game::ops::read_character_derived(ref, kind, out)) return not_found();
    return contracts::ok();
}

QolResult attr(contracts::QolCharacterRef ref, int32_t attr_id, int32_t* out_value) {
    if (!qol::game::ops::character_ref_valid(ref) || out_value == nullptr || attr_id < 0 ||
        attr_id >= contracts::kQolCharacterAttrSlotCount) {
        return invalid_argument();
    }
    if (!qol::game::game_host_is_main_thread()) return blocked_thread();
    if (!qol::game::ops::read_character_attr_value(ref, attr_id, out_value)) return not_found();
    return contracts::ok();
}

QolResult main_stat(contracts::QolCharacterRef ref, int32_t index, int32_t* out_value) {
    if (!qol::game::ops::character_ref_valid(ref) || out_value == nullptr || index < 0 ||
        index >= contracts::kQolCharacterMainStatCount) {
        return invalid_argument();
    }
    if (!qol::game::game_host_is_main_thread()) return blocked_thread();
    if (!qol::game::ops::read_character_main_stat_value(ref, index, out_value)) return not_found();
    return contracts::ok();
}

QolResult present(contracts::QolCharacterRef ref, int32_t* out_present) {
    if (!qol::game::ops::character_ref_valid(ref) || out_present == nullptr) {
        return invalid_argument();
    }
    if (!qol::game::game_host_is_main_thread()) return blocked_thread();
    *out_present = qol::game::ops::character_attributes_present(ref) ? 1 : 0;
    return contracts::ok();
}

const contracts::QolCharacterStatePort kPort{
    contracts::kQolPortCharacterStateVersion,
    {&party, &player, &skills, &attributes, &derived, &attr, &main_stat, &present},
};

}  // namespace

const contracts::QolCharacterStatePort& character_state_port() { return kPort; }

}  // namespace qol::host
