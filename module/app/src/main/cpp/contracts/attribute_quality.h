#pragma once

#include <cstddef>
#include <cstdint>

#include "result.h"
#include "item_handle.h"

// 属性质量只读能力；职责和依赖方向遵循 ARCHITECTURE.md。
// provider 返回属性事实与质量计算结果；呈现决策属于消费者。

namespace qol::contracts {

constexpr const char* kAttributeQualityCapabilityId = "attribute.quality";
constexpr uint16_t kAttributeQualityVersionMajor = 2;
constexpr uint16_t kAttributeQualityVersionMinor = 0;

constexpr int32_t kAttributeQualityHighestLevel = 5;
constexpr uint32_t kAttributeQualityItemCapacity = 32;

// 保留历史档位名称；v2 API 的质量结果使用 AttributeQuality::level。
enum class AttributeQualityTier : uint8_t {
    Grey = 0,
    White = 1,
    Green = 2,
    Blue = 3,
    Purple = 4,
    Gold = 5,
};

struct AttributeQuality {
    // 默认限定为 0..100；允许越界时仍须可表示为 int32_t。
    int32_t percentile;
    // 成功结果为连续等级 0..5，阈值为 30/60/75/90/100。
    int32_t level;
};

struct AttributeRange {
    int32_t min;
    int32_t max;
};

struct AttributeQualityOptions {
    // 0：clamp；1：允许越界百分位。nullptr options 等同全零默认值。
    uint8_t allow_outside_range;
    // 必须全零；未知选项值返回 InvalidArgument。
    uint8_t reserved[3];
};

enum class AttributeKind : uint32_t {
    Jewel = 0,
    StoredOption = 1,
    OptionContext = 2,
};

struct AttributeQualityItem {
    AttributeKind kind;
    // 单次存储快照的位置，不是持久身份或 UI 行号；context 为 UINT32_MAX。
    uint32_t ordinal;
    int32_t attribute_id;
    int32_t option_type;
    int32_t value;
    AttributeRange range;
    AttributeQuality quality;
    // kQolOk 或 kQolErr*；非成功项的 range/quality 不可消费。
    int32_t status;
};

struct AttributeQualityItemResult {
    uint32_t count;
    // 仅表示枚举完整，不表示所有项成功。容量不足时保留前 32 项及原序。
    uint8_t complete;
    uint8_t reserved[3];
    AttributeQualityItem attrs[kAttributeQualityItemCapacity];
};

// live 查询须在游戏主线程同步执行；错误线程、陈旧句柄及未就绪错误原样传播。
// 顶层失败清空 batch；取得列表后逐项错误写入 status，不跳项，顶层仍返回成功。
using AttributeQualityQueryItemFn = QolResult (*)(
    QolItemHandle item, const AttributeQualityOptions* options, AttributeQualityItemResult* out);
// 查询当前显示上下文，不按 option id 匹配存储列表；失败同时返回错误及 out.status。
using AttributeQualityQueryOptionContextFn = QolResult (*)(
    QolItemHandle item, int32_t option_id, int32_t display_value,
    const AttributeQualityOptions* options, AttributeQualityItem* out);
// 纯数值计算，不依赖游戏读面或启用状态。max<=min 返回 100/5。
// 非退化区间的整数百分位向零取整（负数 ceil，非负 floor）；越界结果溢出报错。
// 非空输出在执行前清零；失败后的质量数值不可消费。
using AttributeQualityEvaluateFn = QolResult (*)(
    int32_t value, AttributeRange range, const AttributeQualityOptions* options,
    AttributeQuality* out);

struct AttributeQualityApi {
    // 仅在有效能力调用生命周期内使用；不得跨撤销缓存，未承诺后台 acquire 安全。
    uint32_t struct_size;
    AttributeQualityQueryItemFn query_item;
    AttributeQualityQueryOptionContextFn query_option_context;
    AttributeQualityEvaluateFn evaluate;
};

inline bool attribute_quality_api_size_valid(const AttributeQualityApi* api) {
    constexpr uint32_t kRequiredSize = static_cast<uint32_t>(
        offsetof(AttributeQualityApi, evaluate) + sizeof(AttributeQualityEvaluateFn));
    return api != nullptr && api->struct_size >= kRequiredSize && api->query_item != nullptr &&
           api->query_option_context != nullptr && api->evaluate != nullptr;
}

}  // namespace qol::contracts
