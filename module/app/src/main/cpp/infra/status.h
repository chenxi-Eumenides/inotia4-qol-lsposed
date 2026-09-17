#pragma once

// 插件/功能状态登记表（ARCHITECTURE.md §3.2 状态机、§9.1 最小诊断接口）。
// 结构为纯数据；P1 最小诊断接口输出 id/state/enabled/error 四项。

#include <cstdint>

namespace qol::infra {

// 与 §3.2 状态机一一对应。
enum class FeatureState : int32_t {
    kRegistered = 0,
    kResolving = 1,
    kBlocked = 2,
    kUnsupported = 3,
    kInvalid = 4,
    kInstalledDisabled = 5,
    kEnabled = 6,
    kDeactivating = 7,
    kInactive = 8,
    kFailed = 9,
    kDegraded = 10,
};

const char* feature_state_name(FeatureState state);

struct FeatureStatus {
    const char* id = nullptr;      // 静态字符串
    const char* version = nullptr; // 静态字符串，可为 nullptr
    bool enabled = false;
    FeatureState state = FeatureState::kRegistered;
    const char* error = nullptr;   // 静态字符串或持久错误；nullptr 表示无错误
    uint32_t resource_count = 0;
};

// 注册（幂等：同 id 重复注册返回 false）；id/version 必须为静态存储。
bool status_register(const char* id, const char* version);

// 更新状态；未注册 id 返回 false。error 必须为静态字符串或 nullptr。
bool status_update(const char* id, FeatureState state, bool enabled, const char* error);

uint32_t status_count();

// 按 id 读取单条状态；未注册返回 false（不改写 out）。
// 供宿主门禁监督（host/plugin_registry 的 sync_gates）比较当前状态与目标状态。
bool status_get(const char* id, FeatureStatus* out);

// 按注册顺序写出至多 capacity 条；written 为实际写出条数。
bool status_snapshot(FeatureStatus* out, uint32_t capacity, uint32_t* written);

// 测试用。
void status_reset_for_test();

}  // namespace qol::infra
