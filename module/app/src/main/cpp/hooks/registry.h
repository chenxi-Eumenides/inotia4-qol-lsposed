#pragma once

// 资源登记表（ARCHITECTURE.md §4.1 一资源一安装者、§4.2 占用状态、§4.3 登记表）。
// 插件在装配期只声明资源；登记表是代码事实，由 check_resources.py 汇总比对。
//
// 容量固定 64 条，静态数组实现；键为资源 id 字符串（strcmp 比较）。
// 线程纪律：登记与状态迁移只发生在装配期/主线程，本模块不引入锁。

#include <cstdint>

#include "contracts/plugin.h"

namespace qol::hooks {

// 登记声明。同 id 已有不同 owner → false（一资源一安装者）；
// 同 owner 重复登记 → true（幂等，保留首次登记，不覆盖）。
// id/symbol/owner 任一为空 → false。
bool register_claim(const contracts::ResourceClaim& claim);

// 未登记返回 nullptr。
const contracts::ResourceClaim* find_claim(const char* resource_id);

// 未登记返回 kFree。
contracts::ResourceState state_of(const char* resource_id);

// 未登记返回 false。
bool set_state(const char* resource_id, contracts::ResourceState state);

uint32_t claim_count();

// index 越界返回 nullptr。
const contracts::ResourceClaim* claim_at(uint32_t index);

uint32_t count_by_state(contracts::ResourceState state);

// 测试用：清空登记表。
void registry_reset_for_test();

}  // namespace qol::hooks
