#pragma once

#include <cstdint>

// 面板注册表能力契约（ARCHITECTURE.md §3.4；D1-b0 前置）。
//
// 定位：面板插件（设置页/存档备份页/自动出售页等）把「面板 provider」与「设置页条目」
// 声明式登记到宿主的纯数据注册表（infra/panels），由出口面 UI 容器统一列举与打开；
// provider 的能力通过本契约注册（id=`ui_panels`），消费者只经能力接口打开/查询面板。
//
// 契约类型限制同 contracts/plugin.h：POD + 固定宽度 C 类型 + 函数指针；禁止 STL。
// 字符串均为静态存储（生命周期 ≥ 进程），由注册方持有。

namespace qol::contracts {

constexpr const char* kUiPanelsCapabilityId = "ui_panels";
constexpr int32_t kUiPanelsVersionMajor = 1;
constexpr int32_t kUiPanelsVersionMinor = 2;

// 设置页条目类型：开关（读 config_key 门禁）/ 动作（点击打开 panel_id）。
enum class PanelItemKind : uint8_t {
    kToggle = 0,
    kAction = 1,
};

// 设置页条目声明（纯数据，注册方持有静态字符串）。
struct PanelItemSpec {
    const char* id;          // 全局唯一条目 id
    const char* owner;       // 注册插件 id（一 id 一 owner）
    const char* label;       // 展示文本
    PanelItemKind kind;      // kToggle / kAction
    const char* config_key;  // kToggle 的配置键；kAction 为 nullptr
    const char* panel_id;    // kAction 的目标面板 id；kToggle 为 nullptr
    const char* page_hint;   // 归页提示（可 nullptr = 默认页）
};

// Optional callbacks routed from the shared native PopupState owner to the current panel.
struct PanelCallbacks {
    void (*enter)();
    void (*process)();
    void (*f3)();
    void (*f4)();
    uint32_t (*event)(uint64_t, uint64_t, uint64_t);
};

// 面板 provider 声明：由面板插件注册，容器按 panel_id 打开/查询。
struct PanelProviderSpec {
    const char* panel_id;          // 全局唯一面板 id
    const char* owner;             // 注册插件 id（一 panel 一 owner）
    bool (*open)();                // 打开面板；false = 拒绝（原因见 block_reason）
    void (*close)();               // 关闭面板（幂等）
    bool (*active)();              // 面板当前是否处于打开态
    const char* (*block_reason)(); // 拒绝原因（UTF-8，可 nullptr）；无原因返回 nullptr
    const PanelCallbacks* callbacks; // 可选；nullptr 表示不接管 PopupState 回调
};

// 出口面容器经此接口分发（provider 由注册表按 panel_id 查找）。
struct UiPanelsApi {
    bool (*open)(const char* panel_id);
    void (*close)(const char* panel_id);
    bool (*active)(const char* panel_id);
    const char* (*block_reason)();
    bool (*deactivate_owner)(const char* owner);
};

}  // namespace qol::contracts
