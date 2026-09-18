#pragma once

#include <cstdint>

// 原生选择框能力契约（ARCHITECTURE.md §3.4；D1-b1）。
//
// 定位：把 `plugins/native_choice` 的公开函数（打开/查询/关闭原生 UICHOICE 面板）暴露为
// 该 provider 的能力接口，供 world_teleport 等消费者经能力注册表获取，
// 消除消费者对 native_choice 源码/头的直接依赖。
//
// 语义与迁移前 native_choice_open/active/close 逐字一致：
//   * 规格校验：items 非空、count ∈ [1,6]、close_index ∈ [-1,count)、每个 item 非空；
//   * 同一时刻仅一个选择框：active 时 open 返回 false（消费者按打开失败降级）；
//   * close_index 命中时走原版完整清理链（不改 popup 状态）；
//   * 标题/选项文本由 provider 内部缓冲持有，调用方传入的字符串只需在 open 返回前有效。
//
// 契约类型限制同 contracts/plugin.h：POD + 固定宽度 C 类型 + 函数指针；禁止 STL。

namespace qol::contracts {

constexpr const char* kUiChoiceCapabilityId = "ui_choice";
constexpr int32_t kUiChoiceVersionMajor = 1;
constexpr int32_t kUiChoiceVersionMinor = 0;

// 选择框规格（与迁移前 NativeChoiceSpec 同字段/同顺序/同语义）。
struct UiChoiceSpec {
    const char* const* items;                  // 选项文本（count 项）
    int count;                                 // 1..6
    const char* title;                         // 可为 nullptr/空串（不改写标题）
    int close_index;                           // -1 = 无关闭项
    void (*on_select)(int index, void* user);  // 命中非关闭项时回调（主线程）
    void* user;
};

struct UiChoiceApi {  // POD：只读查询 + 打开/关闭（§3.4）
    bool (*open)(const UiChoiceSpec& spec);
    bool (*active)();
    void (*close)();
};

}  // namespace qol::contracts
