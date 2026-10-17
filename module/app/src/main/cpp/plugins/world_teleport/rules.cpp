#include "rules.h"

#include <algorithm>
#include <cstdint>

namespace world_teleport {

int max_map_id_from_record_count(int record_count) {
    if (record_count <= 0) return kMaxMapId;
    return std::min(record_count - 1, kMaxMapId);
}

int target_map_id(int current_id, int delta, int max_map_id) {
    const int upper = max_map_id < 0 ? kMaxMapId : max_map_id;
    if (upper > kMaxMapId || current_id < 0 || current_id > upper) return -1;
    const int64_t result = static_cast<int64_t>(current_id) + static_cast<int64_t>(delta);
    if (result > upper) return 0;
    if (result < 0) return upper;
    return static_cast<int>(result);
}

int target_map_id(int current_id, int delta) {
    return target_map_id(current_id, delta, kMaxMapId);
}

}  // namespace world_teleport
