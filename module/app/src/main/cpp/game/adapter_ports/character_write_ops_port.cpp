#include "game/adapter_ports/character_write_ops_port.h"

#include "contracts/game_ports/ports.h"
#include "game/ops/character_write.h"

namespace qol::game {
namespace {

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

const contracts::QolCharacterWriteOpsPort& character_write_ops_port() { return kOpsPort; }

}  // namespace qol::game
