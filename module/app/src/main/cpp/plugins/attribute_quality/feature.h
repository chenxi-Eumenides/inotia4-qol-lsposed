#pragma once

#include <cstdint>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/plugin.h"
#include "../../contracts/item_facts.h"

// 属性质量 provider：范围探测、百分位和档位判定。颜色码与文本改写不属于本目录。

namespace qol::features::attribute_quality {

// 资源 id：plugin.cpp 的 ResourceClaim 与本层 on_resource_ready 分派共用。
inline constexpr const char* kResourceIdMathGetRandom = "hook:MATH_GetRandom";

// 安装期注入的 Game Host 表；provider 不直接 include 宿主 game/实现。
void set_context(const contracts::PluginContext& ctx);
void set_item_facts(const contracts::ItemFactsApi* facts);

// 业务门禁：true = 探测生效；false = 撤销期或停用，能力查询拒绝。
void set_active(bool active);
bool active();

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 id 忽略，空 backup 拒绝资源。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 能力接口实现：只读属性批量查询与纯质量计算。
QolResult api_query_item(contracts::QolItemHandle item,
                         const contracts::AttributeQualityOptions* options,
                         contracts::AttributeQualityItemResult* out);
QolResult api_query_option_context(contracts::QolItemHandle item, int32_t option_id,
                                   int32_t display_value,
                                   const contracts::AttributeQualityOptions* options,
                                   contracts::AttributeQualityItem* out);
QolResult api_evaluate(int32_t value, contracts::AttributeRange range,
                       const contracts::AttributeQualityOptions* options,
                       contracts::AttributeQuality* out);

// Lifecycle teardown clears all borrowed Host and capability pointers.
void clear_runtime();

// MATH_GetRandom wrapper：由 provider 的 ResourceClaim.handler 声明，宿主安装。
int math_get_random_wrapper(int min, int max);

}  // namespace qol::features::attribute_quality
