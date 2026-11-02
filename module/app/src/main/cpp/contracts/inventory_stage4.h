#pragma once

// core.item 与 core.inventory 共用的 storage 纯函数单一来源（S2/stage4 系列）。
// header-only inline：无跨翻译单元链接符号，两个插件各自 include 本文件，
// 禁止插件目录互相 include。只依赖 contracts 头与固定宽度类型。

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "game_host.h"
#include "result.h"

namespace qol::contracts::inventory_stage {

using HaveBackup = int (*)(int32_t);
using CountBackup = int (*)(int32_t);
using CountExtension = int (*)(int32_t);

inline int have_item(int32_t category, HaveBackup backup,
                     CountExtension extension_count, bool& recursive_guard) {
    if (recursive_guard) return backup == nullptr ? 0 : backup(category);
    recursive_guard = true;
    const int original = backup == nullptr ? 0 : backup(category);
    const int result = original != 0
        ? original
        : (extension_count == nullptr ? 0 : (extension_count(category) > 0 ? 1 : 0));
    recursive_guard = false;
    return result;
}

inline int get_item_count(int32_t category, CountBackup backup,
                          CountExtension extension_count, bool& recursive_guard) {
    if (recursive_guard) return backup == nullptr ? 0 : backup(category);
    recursive_guard = true;
    const int original = backup == nullptr ? 0 : backup(category);
    const int extension = extension_count == nullptr ? 0 : extension_count(category);
    recursive_guard = false;
    return original + extension;
}

inline int have_item_original_only(int32_t category, HaveBackup backup,
                                   bool& recursive_guard) {
    return have_item(category, backup, nullptr, recursive_guard);
}

inline int get_item_count_original_only(int32_t category, CountBackup backup,
                                        bool& recursive_guard) {
    return get_item_count(category, backup, nullptr, recursive_guard);
}

using CumulateBackup = int (*)(void*);
// S2 读侧统一入口（R-49）的纯逻辑分流：通过插件上下文注入的 item_count facade 读取
// effective；facade 未驻留/未就绪或返回 unsupported 时 original-first 走 backup，
// backup 缺失按 0。插件侧不经 contracts/item_count.h 的宿主访问器。
inline int get_cumulate_count(void* item, const QolItemCountApi* item_count,
                              CumulateBackup backup) {
    if (item != nullptr && item_count != nullptr && item_count->effective != nullptr) {
        uint32_t count = 0;
        const QolResult result = item_count->effective(item, &count);
        assert(result.code != kQolErrInvalidArgument);
        if (succeeded(result)) return static_cast<int>(count);
    }
    return backup == nullptr ? 0 : backup(item);
}

using IdentifyItem = bool (*)(void* item, int32_t* bag, int32_t* slot);
using ConsumeExtension = bool (*)(void* item);
using ConsumeBackup = void (*)(void* item);
using RemoveExtension = bool (*)(void* item);
using RemoveBackup = int (*)(void* item);

inline bool consume_item(void* item, IdentifyItem identify,
                         ConsumeExtension extension_consume,
                         ConsumeBackup backup, bool& recursive_guard) {
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

inline int remove_item(void* item, IdentifyItem identify,
                       RemoveExtension extension_remove,
                       RemoveBackup backup, bool& recursive_guard) {
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

// ---------------------------------------------------------------------------
// S2 写侧 INVEN_RemoveItemData 修正计划（R-45/R-49，纯函数供 Host 断言）。
// 原版（0x1040a8）顺序遍历 6 袋 × 槽，对匹配类别的可堆叠堆：整堆删除累计
// w22，最后一堆部分删除时写 `b = cum(Getter 全量) + w22 - count`。启用态下
// cum 已由 H-17 getter 解码为 S2 全量（算术正确），但 b 写对 remain > 127
// 或旧 a 残留的情形丢失进位 → 由调用方快照 + 重扫后按本函数计算修正。
// ---------------------------------------------------------------------------
struct RemoveDataEntry {
    void* item;        // 快照槽内对象指针
    uint32_t pre_raw;  // 快照时 I_COUNT 原始字段
    uint32_t pre_full; // 快照时启用态 S2 全量
};

struct RemoveDataPost {
    void* item;        // 重扫槽内对象指针（nullptr = 槽已清）
    uint32_t post_raw; // 重扫时 I_COUNT 原始字段
    uint32_t post_view; // 重扫时启用态 S2 解码值
};

struct RemoveDataPlan {
    bool correct = false;  // true = 必须对 entry_index 槽回写 remain（a+b）
    int entry_index = -1;  // 需修正的快照条目下标
    uint32_t remain = 0;   // 正确剩余全量（0..999）
};

// 输入：pre（快照数组）、post（与 pre 等长、按同一槽序重扫）、count（原版入参）。
// 只接受唯一的原版 b-only 写回形态；对象替换、其它同对象槽原始字段变化或 API
// 缺失/失败均 fail-closed。关闭态（R-47）原版 b 写天然正确，调用方不应以启用态
// 数据调用本函数。
inline RemoveDataPlan remove_item_data_plan(
    const QolItemCountApi* item_count, const RemoveDataEntry* pre,
    const RemoveDataPost* post, int entry_count, int32_t count) {
    RemoveDataPlan plan;
    if (pre == nullptr || post == nullptr || entry_count <= 0 || count <= 0) return plan;
    if (item_count == nullptr || item_count->view_limit == nullptr ||
        item_count->field_from_canonical == nullptr) return plan;
    uint32_t count_limit = 0;
    if (!succeeded(item_count->view_limit(1, &count_limit))) return plan;

    uint32_t sum_vanished = 0;
    for (int index = 0; index < entry_count; ++index) {
        if (post[index].item != pre[index].item && post[index].item != nullptr) {
            return RemoveDataPlan{};
        }
        if (post[index].item == nullptr) {
            sum_vanished += pre[index].pre_full;
        }
    }
    if (sum_vanished > static_cast<uint32_t>(count)) return RemoveDataPlan{};
    const uint32_t deleted_here = static_cast<uint32_t>(count) - sum_vanished;

    if (deleted_here == 0) return plan;

    int candidate_index = -1;
    for (int index = 0; index < entry_count; ++index) {
        if (post[index].item == nullptr) continue;

        const uint32_t pre_full = pre[index].pre_full;
        if (deleted_here >= pre_full) {
            if (post[index].post_raw != pre[index].pre_raw) return RemoveDataPlan{};
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
            if (!succeeded(result)) return RemoveDataPlan{};
            b_only_shape = post[index].post_raw == expected_raw;
        } else {
            // field_from_canonical 与 effective 读视图都在 count_limit 处 clamp；该窄带
            // 只有 pre_a=7 且 b 段 >103 时出现，故以 clamp 后的视图相等作等价判定。
            b_only_shape = post[index].post_view == count_limit;
        }

        if (b_only_shape) {
            if (deleted_here >= pre_full || remain >= pre_full || remain > count_limit) {
                return RemoveDataPlan{};
            }
            if (candidate_index >= 0) return RemoveDataPlan{};
            candidate_index = index;
        } else if (post[index].post_raw != pre[index].pre_raw) {
            return RemoveDataPlan{};
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

// H-21 扩展桥接（R-56）物理实扣量 → 扩展补扣量（纯函数供 Host 断言）。
// 原版 INVEN_RemoveItemData 只遍历物理袋 0..5；物理实扣 = 调用前后 H-03 总数差
// （H-03 = 物理 + 扩展，原版不动扩展，故差值即物理实扣）。返回需从扩展袋补扣的
// 数量 = max(0, requested - physical_removed)，requested<=0 或数据反向（after>before）
// 时按 0 处理，避免负补扣。
inline int remove_data_extension_shortfall(int total_before, int total_after,
                                           int requested) {
    if (requested <= 0) return 0;
    const int physical_removed = total_before - total_after;
    if (physical_removed <= 0) return requested;  // 原版未扣到任何物理堆
    if (physical_removed >= requested) return 0;  // 物理已足额
    return requested - physical_removed;
}

}  // namespace qol::contracts::inventory_stage
