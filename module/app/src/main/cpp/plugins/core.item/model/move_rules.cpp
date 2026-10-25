#include "move_rules.h"

namespace qol::features::core_item::model {

MoveRuleError validate_move(const MoveRequest& request, const BagSpaceCounts& counts) {
    if (!valid_bag(request.source, counts) || !is_movable_source(request.source)) {
        return is_task_bag(request.source) ? MoveRuleError::kTaskBag
                                           : MoveRuleError::kInvalidSource;
    }
    if (!valid_bag(request.target, counts) || !is_valid_target(request.target)) {
        return is_task_bag(request.target) ? MoveRuleError::kTaskBag
                                           : MoveRuleError::kInvalidTarget;
    }
    if (request.source_slot < 0 || request.target_slot < 0 ||
        request.target_capacity <= 0 || request.target_slot >= request.target_capacity) {
        return MoveRuleError::kInvalidSlot;
    }
    if (request.source_count <= 0 || request.move_count <= 0 ||
        request.move_count > request.source_count) {
        return MoveRuleError::kInvalidCount;
    }
    if (request.source.space_id == request.target.space_id &&
        request.source.bag == request.target.bag &&
        request.source_slot == request.target_slot) {
        return MoveRuleError::kSameSlot;
    }
    return MoveRuleError::kNone;
}

}  // namespace qol::features::core_item::model
