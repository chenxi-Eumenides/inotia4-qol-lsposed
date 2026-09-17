#pragma once

#include <cstddef>
#include <cstdint>

// 延迟释放排空（drain）的纯规则：drain 实现与 host 测试共用同一组判据。
// 只依赖 STL，不引用游戏符号，故可被 host 测试直接编译。
//
// 旧实现把 `queue_size + custody_size` 当作环形队列的遍历上界，并让第一段与第二段
// 分别遍历 custody；custody 非空时队首 custody 项被二次访问（重复 free）。修复后的
// 契约是：第一段只遍历队列自身条目，custody 由第二段独立遍历；kept 缓冲必须同时
// 容纳队列与保管区的全部条目。
namespace deferred_free_rules {

// 第一段（环形队列）遍历上界：只与队列自身条目数有关，不吸收 custody 数量。
constexpr size_t queue_iteration_bound(size_t queue_size, size_t /*custody_size*/) {
    return queue_size;
}

// kept 回填缓冲容量：队列与 custody 的条目在一次 drain 中可能全部保留。
constexpr size_t kept_capacity(size_t queue_capacity, size_t custody_capacity) {
    return queue_capacity + custody_capacity;
}

// 一次 drain 的释放数不得超过本次输入条目总数。
constexpr bool freed_within_bound(size_t freed, size_t queue_size, size_t custody_size) {
    return freed <= queue_size + custody_size;
}

// 帧龄下限：帧号可用时要求当前帧严格大于入队帧（跨过帧边界）；帧号不可用
// （主菜单早期 / world 未就绪）时回退到墙钟下限 elapsed_ms >= min_ms。
constexpr bool residence_met(int64_t enqueued_frame, int64_t current_frame,
                             uint64_t elapsed_ms, uint64_t min_ms) {
    if (enqueued_frame > 0 && current_frame > 0) return current_frame > enqueued_frame;
    return elapsed_ms >= min_ms;
}

// ---- 缺陷回归证据：仅供 host 测试固化旧实现行为，生产 drain 不得引用 ----

// 旧实现：环形队列遍历上界 = queue_size + custody_size（queue 满时越界绕回）。
constexpr size_t legacy_iteration_bound(size_t queue_size, size_t custody_size) {
    return queue_size + custody_size;
}

// 旧实现重复访问次数：queue 满（=队列容量）时，第一段绕回重复访问队首 custody_size
// 个条目；custody 只在队列满时生成，故该值即重复访问数。
constexpr size_t legacy_repeat_visits(size_t queue_size, size_t custody_size) {
    (void)queue_size;
    return custody_size;
}

// 旧实现总 free 调用数：第一段遍历 queue_size + custody_size 次，第二段再遍历
// custody_size 次，无引用时合计 queue_size + 2 * custody_size。
constexpr size_t legacy_free_calls(size_t queue_size, size_t custody_size) {
    return queue_size + 2 * custody_size;
}

}  // namespace deferred_free_rules
