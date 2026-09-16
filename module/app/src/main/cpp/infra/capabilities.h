#pragma once

// 能力注册表（ARCHITECTURE.md §6.4）：接口指针注册与按版本查询。
// 接口只读查询；写操作走资源所有者提供的命令端口（§3.4.3）。
//
// 生命周期：接口对象由提供方保证在撤销事务完成前有效；消费者不得缓存裸指针越过
// 撤销点（消费方须处理 require 返回 nullptr 的降级路径）。

#include <cstdint>

#include "contracts/version.h"

namespace qol::infra {

using CapabilityId = const char*;  // 编译期字符串常量

struct CapabilityProvision {
    CapabilityId id = nullptr;
    QolVersion version{0, 0};
    const void* iface = nullptr;
};

// 重复注册同一 id（未撤销时）返回 false。
bool register_capability(const CapabilityProvision& provision);

bool unregister_capability(CapabilityId id);

// 已注册且版本满足 QolVersionRange 时返回接口指针，否则返回 nullptr。
const void* require_capability(CapabilityId id, QolVersionRange required);

uint32_t capability_count();

// 测试用。
void capabilities_reset_for_test();

}  // namespace qol::infra
