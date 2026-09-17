#pragma once

#include <cstdint>

#include "contracts/attribute_range.h"
#include "contracts/plugin.h"

// 属性显示范围（attribute-range-display）业务层（ARCHITECTURE.md §3.3/§3.4）。
//
// 在宝石/装备详情中，按随机属性值在其可能范围内的百分位改写该行内联颜色码
// （金/紫/蓝/绿/白/灰）。三个入口 hook（MATH_GetRandom / UIDesc_AddOption /
// UIDesc_MakeItem）改为声明式资源，由宿主安装；原函数入口经 on_resource_ready 交还。
//
// 能力：提供 attr_range v1.0，消费者经 infra/capabilities require 取得只读查询。
// 撤销期（§3.4.4）active=false，probe 返回 false，消费者走降级路径。

namespace qol::features::attribute_range {

// 资源 id：plugin.cpp 的 ResourceClaim 与本层 on_resource_ready 分派共用。
inline constexpr const char* kResourceIdMathGetRandom = "hook:MATH_GetRandom";
inline constexpr const char* kResourceIdAddOption = "hook:UIDesc_AddOption";
inline constexpr const char* kResourceIdMakeItem = "hook:UIDesc_MakeItem";

// 业务门禁：true = 探测/改写生效；false = 撤销期或停用，能力查询拒绝。
void set_active(bool active);
bool active();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 id 忽略，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 能力接口实现：只读探测；撤销期 / hook 未就绪返回 false（消费者降级）。
bool api_probe_jewel_range(int type, void* item, int* out_min, int* out_max);

// 三个入口 wrapper：由 plugin_descriptor 的 ResourceClaim.handler 声明，宿主安装。
int math_get_random_wrapper(int min, int max);
void uid_desc_add_option_wrapper(void* builder, int type, int option_index, int value);
uintptr_t uid_desc_make_item_wrapper(void* item, void* character, void* arg2);

}  // namespace qol::features::attribute_range
