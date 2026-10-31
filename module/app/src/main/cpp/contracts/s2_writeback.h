#pragma once

// ---------------------------------------------------------------------------
// S2 写侧进位/借位纯函数（R-45/R-49：写侧无统一 setter，各写点调用方完成类别
// 门控与 `count=128a+b` 回写；进位（b 溢出 127 → a+1）与借位（a>0 时 b 回绕
// → a-1、b+128）由 s2_write_count 编码内建）。
// 仅做位域算术、不触内存，供 wrapper 与后续 Host 断言使用。
// core.item 与 core.inventory 共用的单一来源；header-only inline，无跨翻译
// 单元链接符号，两个插件各自 include 本文件，禁止插件目录互相 include。
// ---------------------------------------------------------------------------
#include <cstdint>

#include "game_host.h"

namespace qol::contracts {
inline namespace s2_writeback {

// S2 writeback is owned by the applied 999 feature and encoded item classes only.
inline bool allowed(bool extended_applied, bool count_encoded) {
    return extended_applied && count_encoded;
}

// original-only 写后确认：post 与 pre 不同、且恰好等于原版对 new_count 的 b 写
// 结果时，才认定「原版确实执行了该次数量写」。post == pre（含 delta≡0 mod 128
// 的退化与原版拒绝合并）一律 fail-closed 跳过，不得凭相等推断写发生。
// delta 是 128 的整数倍时原版只写 b 段，b 字段前后不变；此时只有“源槽已不再
// 指向被移动对象”能证明原版确实完成了合并。不能仅凭 post==pre 推断未合并。
constexpr bool source_removed_confirms_merge(uint32_t delta, bool source_found,
                                             bool source_still_present) {
    return delta > 0 && (delta % 128u) == 0 && source_found && !source_still_present;
}

inline bool original_count_write_confirmed(const QolItemCountApi* api,
                                           uint32_t pre_field, uint32_t post_field,
                                           uint32_t new_count) {
    if (api == nullptr || api->canonical_from_field == nullptr ||
        api->field_from_canonical == nullptr || post_field == pre_field) {
        return false;
    }
    uint32_t pre_canonical = 0;
    uint32_t post_canonical = 0;
    uint32_t pre_without_b = 0;
    uint32_t post_without_b = 0;
    if (!succeeded(api->canonical_from_field(pre_field, &pre_canonical)) ||
        !succeeded(api->canonical_from_field(post_field, &post_canonical)) ||
        !succeeded(api->field_from_canonical(pre_field, 0, 0, &pre_without_b)) ||
        !succeeded(api->field_from_canonical(post_field, 0, 0,
                                             &post_without_b))) {
        return false;
    }
    // field_from_canonical(..., 0, false) preserves the non-B field bits. The
    // canonical remainder identifies the original setter's seven-bit value
    // without duplicating its bit layout here.
    return pre_without_b == post_without_b &&
           (post_canonical % 128u) == (new_count % 128u);
}

// 加法回写全量：old_full + delta 后按模式上限收敛（启用 999 / 关闭 99，
// R-47 决策 b：old_full 必须是模式视图数量——启用态 128a+b、关闭态 b；
// 关闭态结果落在 b 域；facade 负责当前视图的上限收敛，delta 先按
// 编码上限收敛，避免极端入参溢出。
inline uint32_t added_count(const QolItemCountApi* api,
                            uint32_t old_full, uint32_t delta) {
    if (api == nullptr || api->view_limit == nullptr) return old_full;
    uint32_t limit = 0;
    if (!succeeded(api->view_limit(1, &limit))) return old_full;
    const uint32_t bounded = delta > limit ? limit : delta;
    return old_full + bounded;
}

// 减法回写全量：old_full - sub（调用方须保证 old_full > sub；old_full 为模式
// 视图数量，关闭态结果同样落在 b 域）。
inline uint32_t subtracted_count(uint32_t old_full, uint32_t sub) {
    return old_full > sub ? old_full - sub : 0u;
}

// 减 1 借位回写全量（old_full ≥ 2 时调用）。
inline uint32_t decremented_count(uint32_t old_full) {
    return old_full - 1u;
}

}  // namespace s2_writeback
}  // namespace qol::contracts
