#pragma once

// inventory_hooks 插件内部入口（D2c-2a）：声明式资源声明表与入口 hook backup 交还。
//
// 定义在 plugins/inventory_hooks/native_inventory_hook.cpp（handler 取址只能在该 TU）。
// 迁入本插件目录后，plugin.cpp 与 native_inventory_hook.cpp 同层，直接 include 本头即可，
// 不再需要为规避跨层依赖而设的全局桥（原 inventory_hooks_claims /
// inventory_hooks_bind_backup 前置声明）。

#include <cstdint>

#include "../../contracts/plugin.h"

// 24 条入口 hook 的声明表（静态存储，生命周期 ≥ 进程）；out_count 可空。
const qol::contracts::ResourceClaim* inventory_hooks_claims(uint32_t* out_count);

// 入口 hook backup 交还（宿主 on_resource_ready 调用）：按资源 id 写入对应 wrapper 的
// 原函数槽；未知 id / 空 backup 返回非 0，宿主按 fail-closed 回滚。
int32_t inventory_hooks_bind_backup(const char* resource_id, void* backup);
