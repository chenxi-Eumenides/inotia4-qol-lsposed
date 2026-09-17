#pragma once

// 命令端口契约（ARCHITECTURE.md §3.4.3）。
// 命令为 POD；post 在主线程执行，结果写回票据；取消语义由命令声明。

#include <cstdint>

#include "contracts/result.h"

namespace qol::contracts {

using QolCommandTicket = uint64_t; // 0 = 无效票据

enum : uint32_t {
    kCommandSetConfigBool = 1,
    kCommandFeatureLifecycle = 2,
};

// 生命周期命令动作（kind == kCommandFeatureLifecycle 时有效）；key = 插件 id。
enum : int32_t {
    kFeatureLifecycleEnable = 1,
    kFeatureLifecycleDisable = 2,
    kFeatureLifecycleDeactivate = 3,
    kFeatureLifecycleActivate = 4,
};

struct QolCommand {
    uint32_t kind;
    const char* key;
    int32_t int_value;
};

struct QolCommands {
    QolResult (*post)(QolCommandTicket* out, const QolCommand* cmd); // 主线程执行
    int32_t (*state)(QolCommandTicket ticket);                       // pending/done/failed/cancelled
    QolResult (*cancel)(QolCommandTicket ticket);
};

}  // namespace qol::contracts
