#include "game/adapter_ports/character_write_port.h"

#include <cstdint>

#include "contracts/game_ports/ports.h"
#include "game/ops/character_write.h"
#include "game/state/character_write_http.h"

namespace qol::game {
namespace {

QolResult character_write_adapter(const contracts::QolCharacterWriteArgs* args, char* out,
                                  uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (args == nullptr || out == nullptr || out_size == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "invalid character write args", 29);
    }
    const int32_t code = build_character_write_result_json(*args, out, capacity, out_size);
    if (code == kCharacterWriteOk) return contracts::ok();
    return contracts::error(kQolErrInternal, "character write failed", 22);
}

const contracts::QolCharacterWritePortReadApi kRead{&character_write_adapter};
const contracts::QolCharacterWritePort kPort{contracts::kQolPortCharacterWriteVersion, kRead};

const contracts::QolCharacterWriteOpsPort kOpsPort{
    contracts::kQolPortCharacterWriteOpsVersion,
    &ops::set_hp,
    &ops::set_mp,
    &ops::set_level,
    &ops::set_experience,
    &ops::set_attr,
    &ops::set_status_point,
    &ops::party_swap,
    &ops::set_auto_attack,
    &ops::cast_skill,
    &ops::attack_target,
    &ops::switch_player,
    &ops::stop_combat,
    &ops::learn_action,
    &ops::add_stat,
    &ops::stat_reset,
    &ops::skill_reset,
    &ops::party_include,
    &ops::party_exclude,
    &ops::party_discharge,
    &ops::party_withdraw,
};

}  // namespace

const contracts::QolCharacterWritePort& character_write_port() { return kPort; }

const contracts::QolCharacterWriteOpsPort& character_write_ops_port() { return kOpsPort; }

}  // namespace qol::game
