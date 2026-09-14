#include "feature/world_teleport/world_teleport_rules.h"

namespace world_teleport {

int target_map_id(int current_id, int delta) {
    int result = current_id + delta;
    if (result > kMaxMapId) return 0;
    if (result < 0) return kMaxMapId;
    return result;
}

}  // namespace world_teleport
