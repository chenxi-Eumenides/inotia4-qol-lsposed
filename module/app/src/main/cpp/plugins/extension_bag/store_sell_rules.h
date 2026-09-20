#pragma once

#include <cstdint>

namespace qol::store_sell {

enum class PlanKind { kBackup, kWhole, kPartial, kReject };

struct Plan {
    PlanKind kind;
    int32_t count;
};

inline Plan plan(bool count_encoded, int32_t view, int64_t unit, int64_t displayed) {
    if (!count_encoded || view <= 1) return Plan{PlanKind::kWhole, 1};
    if (unit <= 0 || displayed <= 0 || displayed % unit != 0) {
        return Plan{PlanKind::kReject, 0};
    }
    const int64_t count = displayed / unit;
    if (count < 1 || count > static_cast<int64_t>(view)) {
        return Plan{PlanKind::kReject, 0};
    }
    return Plan{PlanKind::kPartial, static_cast<int32_t>(count)};
}

}  // namespace qol::store_sell
