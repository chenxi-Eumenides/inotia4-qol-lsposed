#include "stage4.h"

namespace qol::features::core_inventory::stage4 {

bool stage4_consume_item(void* item, Stage4IdentifyItem identify,
                         Stage4ConsumeExtension extension_consume,
                         Stage4ConsumeBackup backup, bool& recursive_guard) {
    int32_t bag = -1;
    int32_t slot = -1;
    if (!recursive_guard && identify != nullptr && identify(item, &bag, &slot)) {
        return extension_consume != nullptr && extension_consume(item);
    }
    if (backup == nullptr) return false;
    if (recursive_guard) {
        backup(item);
        return true;
    }
    recursive_guard = true;
    backup(item);
    recursive_guard = false;
    return true;
}

int stage4_remove_item(void* item, Stage4IdentifyItem identify,
                       Stage4RemoveExtension extension_remove,
                       Stage4RemoveBackup backup, bool& recursive_guard) {
    int32_t bag = -1;
    int32_t slot = -1;
    if (!recursive_guard && identify != nullptr && identify(item, &bag, &slot)) {
        if (extension_remove == nullptr) return 0;
        // 扩展对象的释放失败必须停在扩展分支，不能再落入原版 backup
        // （确认使用期间 backup 不认识逻辑对象且可能破坏失败语义）。
        return extension_remove(item) ? 1 : 0;
    }
    if (backup == nullptr) return 0;
    if (recursive_guard) return backup(item);
    recursive_guard = true;
    const int result = backup(item);
    recursive_guard = false;
    return result;
}

Stage4RemoveDataPlan stage4_remove_item_data_plan(
    const qol::contracts::QolItemCountApi* item_count, const Stage4RemoveDataEntry* pre,
    const Stage4RemoveDataPost* post, int entry_count, int32_t count) {
    Stage4RemoveDataPlan plan;
    if (pre == nullptr || post == nullptr || entry_count <= 0 || count <= 0) return plan;
    if (item_count == nullptr || item_count->view_limit == nullptr ||
        item_count->field_from_canonical == nullptr) return plan;
    uint32_t count_limit = 0;
    if (!qol::contracts::succeeded(item_count->view_limit(1, &count_limit))) return plan;

    uint32_t sum_vanished = 0;
    for (int index = 0; index < entry_count; ++index) {
        if (post[index].item != pre[index].item && post[index].item != nullptr) {
            return Stage4RemoveDataPlan{};
        }
        if (post[index].item == nullptr) {
            sum_vanished += pre[index].pre_full;
        }
    }
    if (sum_vanished > static_cast<uint32_t>(count)) return Stage4RemoveDataPlan{};
    const uint32_t deleted_here = static_cast<uint32_t>(count) - sum_vanished;

    if (deleted_here == 0) return plan;

    int candidate_index = -1;
    for (int index = 0; index < entry_count; ++index) {
        if (post[index].item == nullptr) continue;

        const uint32_t pre_full = pre[index].pre_full;
        if (deleted_here >= pre_full) {
            if (post[index].post_raw != pre[index].pre_raw) return Stage4RemoveDataPlan{};
            continue;
        }

        const uint32_t remain = pre_full - deleted_here;
        const uint32_t pre_a_block = (pre_full / 128u) * 128u;
        const uint32_t expected_view = pre_a_block + (remain % 128u);
        bool b_only_shape = false;
        if (expected_view <= 999u) {
            uint32_t expected_raw = 0;
            const QolResult result = item_count->field_from_canonical(
                pre[index].pre_raw, expected_view, 1, &expected_raw);
            if (!qol::contracts::succeeded(result)) return Stage4RemoveDataPlan{};
            b_only_shape = post[index].post_raw == expected_raw;
        } else {
            // field_from_canonical 与 effective 读视图都在 count_limit 处 clamp；该窄带
            // 只有 pre_a=7 且 b 段 >103 时出现，故以 clamp 后的视图相等作等价判定。
            b_only_shape = post[index].post_view == count_limit;
        }

        if (b_only_shape) {
            if (deleted_here >= pre_full || remain >= pre_full || remain > count_limit) {
                return Stage4RemoveDataPlan{};
            }
            if (candidate_index >= 0) return Stage4RemoveDataPlan{};
            candidate_index = index;
        } else if (post[index].post_raw != pre[index].pre_raw) {
            return Stage4RemoveDataPlan{};
        }
    }

    if (candidate_index < 0) return plan;
    const uint32_t remain = pre[candidate_index].pre_full - deleted_here;
    if (remain == post[candidate_index].post_view) return plan;
    plan.correct = true;
    plan.entry_index = candidate_index;
    plan.remain = remain;
    return plan;
}

int stage4_remove_data_extension_shortfall(int total_before, int total_after,
                                           int requested) {
    if (requested <= 0) return 0;
    const int physical_removed = total_before - total_after;
    if (physical_removed <= 0) return requested;  // 原版未扣到任何物理堆
    if (physical_removed >= requested) return 0;  // 物理已足额
    return requested - physical_removed;
}

}  // namespace qol::features::core_inventory::stage4
