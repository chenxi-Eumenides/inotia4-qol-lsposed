#pragma once

#include <cstdint>
#include <string>

#include "../../contracts/result.h"

// 游戏世界调整的纯逻辑层（不触内存、不调游戏函数、无平台依赖）：
// 倍率档位、配置 JSON 编解码与校验、整数缩放。供 host test 穷举，不依赖 UI 或游戏线程。
namespace qol::features::world_tuning {

// 面板行与配置字段顺序。新增倍率时同步追加在这里、kRateLadder 之外的中文元数据与面板行。
enum class RateId : uint8_t {
    kGold = 0,
    kExp,
    kPlayerHp,
    kEnemyHp,
    kDamageOut,
    kDamageIn,
    kMoveSpeed,
    kCount,
};

// 统一档位（百分比，100 = 不修改）。面板左右箭头只在这组值之间移动。
inline constexpr int32_t kRateLadder[] = {10, 30, 50, 80, 100, 120, 150, 200, 300, 500};
inline constexpr uint32_t kRateLadderSize = sizeof(kRateLadder) / sizeof(kRateLadder[0]);
inline constexpr int32_t kRateDefault = 100;

struct Rates {
    int32_t value[static_cast<uint32_t>(RateId::kCount)]{};
};

// 当前实现真正接线的倍率。其余行在面板中可见但箭头禁用（界面先行、行为未接）。
bool rate_supported(RateId id);

const char* rate_key(RateId id);         // 配置 JSON 键名
const char* rate_name(RateId id);        // 面板行标题
const char* rate_description(RateId id); // 面板行副标题

// 档位查找与移动。非法值返回 -1 / 原值。
int32_t ladder_index(int32_t value);
int32_t ladder_shift(int32_t value, int32_t delta);

// 文本：百分比整数 → "×1.5"（一位小数，去掉多余 0）。
std::string rate_display(int32_t value);

Rates default_rates();
// 把 rates 编码为单行 JSON 对象（字段顺序固定，整数）。
std::string encode_rates(const Rates& rates);
// 严格解析：只接受白名单字段、整数值、档位内取值；缺字段用默认值；未知字段/重复字段/语法错误返回 false。
bool decode_rates(const char* json, uint32_t size, Rates* out);
// PluginDescriptor::validate_config 入口：只接受 key == "rates"，其余键拒绝。
QolResult validate_rates_config(const char* key, const char* json, uint32_t size);

// 百分比缩放（伤害）：value<=0 或 percent==100 原样返回；int64 计算后钳到 [1, INT32_MAX]。
int32_t scale_by_percent(int32_t value, int32_t percent);
// 百分比缩放（属性/生命）：raw<=0 原样返回；percent==100 原样返回；结果下限 1。
int32_t scale_attribute(int32_t raw, int32_t percent);

}  // namespace qol::features::world_tuning
