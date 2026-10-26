#include "bag_catalog.h"

namespace qol::features::core_item::model {
namespace {

constexpr int32_t kInt32Max = 2147483647;

bool add_count(int32_t count, int32_t* total) {
    if (total == nullptr || count < 0 || *total < 0 || *total > kInt32Max - count) {
        return false;
    }
    *total += count;
    return true;
}

}  // namespace

int32_t space_count(const BagSpaceCounts& counts, int32_t space_id) {
    if (space_id == contracts::kQolBagSpaceOriginal) {
        return counts.original_count >= 0 ? counts.original_count : 0;
    }
    if (space_id == contracts::kQolBagSpaceVariant) {
        return counts.c18_count >= 0 ? counts.c18_count : 0;
    }
    if (space_id < contracts::kQolBagSpaceFirstRegistered) return 0;
    for (const RegisteredBagSpaceCount& registered : counts.registered) {
        if (registered.space_id == space_id) {
            return registered.count >= 0 ? registered.count : 0;
        }
    }
    return 0;
}

bool valid_bag(const BagRef& ref, const BagSpaceCounts& counts) {
    if (ref.bag < 0) return false;
    const int32_t count = space_count(counts, ref.space_id);
    return count > 0 && ref.bag < count;
}

bool to_flat(const BagRef& ref, const BagSpaceCounts& counts, int32_t* out_flat) {
    if (out_flat == nullptr || !valid_bag(ref, counts)) return false;

    int32_t offset = 0;
    if (ref.space_id == contracts::kQolBagSpaceOriginal) {
        *out_flat = ref.bag;
        return true;
    }
    if (!add_count(space_count(counts, contracts::kQolBagSpaceOriginal), &offset)) return false;
    if (ref.space_id == contracts::kQolBagSpaceVariant) {
        if (ref.bag > kInt32Max - offset) return false;
        *out_flat = offset + ref.bag;
        return true;
    }
    if (!add_count(space_count(counts, contracts::kQolBagSpaceVariant), &offset)) return false;

    for (const RegisteredBagSpaceCount& registered : counts.registered) {
        if (registered.space_id < contracts::kQolBagSpaceFirstRegistered) continue;
        const int32_t count = registered.count >= 0 ? registered.count : 0;
        if (registered.space_id == ref.space_id) {
            if (ref.bag > kInt32Max - offset) return false;
            *out_flat = offset + ref.bag;
            return true;
        }
        if (!add_count(count, &offset)) return false;
    }
    return false;
}

bool from_flat(int32_t flat, const BagSpaceCounts& counts, BagRef* out) {
    if (flat < 0 || out == nullptr) return false;
    int32_t remaining = flat;
    const int32_t original_count = space_count(counts, contracts::kQolBagSpaceOriginal);
    if (remaining < original_count) {
        *out = BagRef{contracts::kQolBagSpaceOriginal, remaining};
        return true;
    }
    remaining -= original_count;

    const int32_t c18_count = space_count(counts, contracts::kQolBagSpaceVariant);
    if (remaining < c18_count) {
        *out = BagRef{contracts::kQolBagSpaceVariant, remaining};
        return true;
    }
    remaining -= c18_count;

    for (const RegisteredBagSpaceCount& registered : counts.registered) {
        if (registered.space_id < contracts::kQolBagSpaceFirstRegistered ||
            registered.count < 0) {
            continue;
        }
        if (remaining < registered.count) {
            *out = BagRef{registered.space_id, remaining};
            return true;
        }
        remaining -= registered.count;
    }
    return false;
}

bool is_task_bag(const BagRef& ref) {
    return ref.space_id == contracts::kQolBagSpaceOriginal &&
           ref.bag == contracts::kQolTaskBagIndex;
}

bool is_movable_source(const BagRef& ref) {
    return ref.space_id >= contracts::kQolBagSpaceOriginal && ref.bag >= 0 &&
           !is_task_bag(ref);
}

bool is_valid_target(const BagRef& ref) {
    return ref.space_id >= contracts::kQolBagSpaceOriginal && ref.bag >= 0 &&
           !is_task_bag(ref);
}

}  // namespace qol::features::core_item::model
