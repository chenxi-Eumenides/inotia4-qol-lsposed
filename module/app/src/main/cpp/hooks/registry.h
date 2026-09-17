#pragma once

// 资源登记表（ARCHITECTURE.md §4.1 一资源一安装者、§4.2 占用状态、§4.3 登记表）。
// 插件在装配期只声明资源；登记表是代码事实，由 check_resources.py 汇总比对。
//
// 容量固定 128 条，静态数组实现；键为资源 id 字符串（strcmp 比较）。
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

// 资源运行期记录（安装成功后登记；释放成功后清除）。
// 运行期字段与登记声明同表存储：事务引擎与宿主诊断均从此处读取。
struct ResourceRuntime {
    uintptr_t resolved_entry = 0;
    void* backup = nullptr;
    uint32_t in_flight = 0;   // 快照值；写路径见 in_flight_enter/leave
    bool isolated = false;
};

// 登记运行期解析结果：已登记 → 写入 entry/backup 并返回 true；未登记 → false。
// 只写 resolved_entry/backup，不改变 in_flight/isolated。
bool runtime_set(const char* resource_id, uintptr_t resolved_entry, void* backup);

// 清除运行期记录（物理释放成功后登记表回到 free）：已登记 → 清零 entry/backup/in_flight 并返回 true。
// isolated 是进程级隔离标记，不随运行期记录清除。
bool runtime_clear(const char* resource_id);

// 未登记或未写运行期（resolved_entry==0）→ false。
bool runtime_of(const char* resource_id, ResourceRuntime* out);

// 在途计数原子 +1；未登记 → false。
bool in_flight_enter(const char* resource_id);

// 在途计数原子 -1（下界 0）；未登记 → false。
bool in_flight_leave(const char* resource_id);

// 未登记 → 0。
uint32_t in_flight_count(const char* resource_id);

// 隔离标记写入/读取；未登记 → false。
bool isolated_set(const char* resource_id, bool isolated);
bool isolated_get(const char* resource_id);

// 测试用：清空登记表与全部运行期字段。
void registry_reset_for_test();

}  // namespace qol::hooks
