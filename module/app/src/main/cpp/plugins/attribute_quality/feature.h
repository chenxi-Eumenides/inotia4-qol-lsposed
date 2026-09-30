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

// 宿主在资源安装成功后交还 backup（原函数入口）；未知 id 忽略，恒返回 0。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 能力接口实现：只读探测；撤销期 / hook 未就绪返回错误（消费者降级）。
QolResult api_probe_jewel(contracts::QolItemHandle item, int32_t attribute_type, int32_t value,
                          contracts::AttributeQualityResult* out);
QolResult api_probe_option(contracts::QolItemHandle item, int32_t option, int32_t level, int32_t flag,
                           int32_t value,
                           contracts::AttributeQualityResult* out);
QolResult api_classify(int32_t value, int32_t min, int32_t max,
                       contracts::AttributeQualityTier* out_tier);
QolResult api_percentile(int32_t value, int32_t min, int32_t max, int32_t* out_percentile);

// MATH_GetRandom wrapper：由 provider 的 ResourceClaim.handler 声明，宿主安装。
int math_get_random_wrapper(int min, int max);

}  // namespace qol::features::attribute_quality
