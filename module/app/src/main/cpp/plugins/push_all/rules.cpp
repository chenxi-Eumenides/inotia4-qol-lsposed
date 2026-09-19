#include "rules.h"

namespace qol::features::push_all {

int party_decision(uint8_t type, bool is_active_player_group, int party_index) {
    // 怪物：本功能新增语义，直接视为队伍成员。
    if (type == 1) {
        return 0;
    }
    // 主控及主控召唤物群体：放开。
    if (is_active_player_group) {
        return 0;
    }
    // 其余交原版判定：队友 >=0 可推，场景设施 -1 不可推。
    return party_index;
}

}  // namespace qol::features::push_all
