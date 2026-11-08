#pragma once

#include <cstdint>
#include <string>

#include "../../contracts/result.h"

// 游戏世界调整的纯逻辑层（不触内存、不调游戏函数、无平台依赖）：
// 配置 JSON 编解码与校验、整数缩放。档位仅用于面板箭头预设，持久化值允许任意合法整数。
namespace qol::features::world_tuning {

// 面板行与配置字段顺序。新增倍率时同步追加在这里、kRateLadder 之外的中文元数据与面板行。
enum class RateId : uint8_t {
    kGold = 0,
    kExp,
    kEnemyHp,
    kDamageOut,
    kDamageIn,
    kDropQuantity,
    kDropReroll,
    kCount,
};

// 基础档位仅供 OP 关闭时面板使用；百分比配置可设 0 或超出档位范围，扩展档位才提供 0。
inline constexpr int32_t kRateLadder[] = {10, 30, 50, 80, 100, 120, 150, 200, 300, 500};
inline constexpr uint32_t kRateLadderSize = sizeof(kRateLadder) / sizeof(kRateLadder[0]);
inline constexpr int32_t kRateLadderExtended[] = {
    0, 10, 30, 50, 80, 100, 120, 150, 200, 300, 500, 1000, 10000, 100000, 1000000,
    10000000, 100000000};
inline constexpr uint32_t kRateLadderExtendedSize =
    sizeof(kRateLadderExtended) / sizeof(kRateLadderExtended[0]);
inline constexpr int32_t kRateDefault = 100;

// 基础掉落档位不含 0；配置仍可设为 0 表示不掉落。
inline constexpr int32_t kDropQuantityLadder[] = {1, 2, 3, 4, 5};
inline constexpr uint32_t kDropQuantityLadderSize =
    sizeof(kDropQuantityLadder) / sizeof(kDropQuantityLadder[0]);
inline constexpr int32_t kDropQuantityLadderExtended[] = {
    0, 1, 2, 3, 4, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
    130, 140, 150, 160, 170, 180, 190, 200};
inline constexpr uint32_t kDropQuantityLadderExtendedSize =
    sizeof(kDropQuantityLadderExtended) / sizeof(kDropQuantityLadderExtended[0]);
inline constexpr int32_t kDropQuantityDefault = 1;
// 基础重抽档位为 1..5；配置/扩展档位中的 0 表示不掉落。
inline constexpr int32_t kDropRerollLadder[] = {1, 2, 3, 4, 5};
inline constexpr uint32_t kDropRerollLadderSize =
    sizeof(kDropRerollLadder) / sizeof(kDropRerollLadder[0]);
inline constexpr int32_t kDropRerollLadderExtended[] = {
    0, 1, 2, 3, 4, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
    130, 140, 150, 160, 170, 180, 190, 200};
inline constexpr uint32_t kDropRerollLadderExtendedSize =
    sizeof(kDropRerollLadderExtended) / sizeof(kDropRerollLadderExtended[0]);
inline constexpr int32_t kDropRerollDefault = 1;

struct Rates {
    int32_t value[static_cast<uint32_t>(RateId::kCount)]{};
};

// 查询倍率是否已接入游戏行为。
bool rate_supported(RateId id);

const char* rate_key(RateId id);         // 配置 JSON 键名
const char* rate_name(RateId id);        // 面板行标题
const char* rate_description(RateId id); // 面板行副标题

// 按行面板预设表；未知 id 返回通用档位。
const int32_t* rate_ladder(RateId id, bool extended, uint32_t* out_size);
int32_t rate_default(RateId id);
bool rate_value_valid(RateId id, int32_t value);

// 档位查找与移动。非预设值的 index 为 -1；无可移动预设时保持原值。
int32_t ladder_index(RateId id, int32_t value, bool extended);
int32_t ladder_shift(RateId id, int32_t value, int32_t delta, bool extended);
bool ladder_can_shift(RateId id, int32_t value, int32_t delta, bool extended);

// 文本：通用倍率 → "×1.5"；掉落数量/重抽 → "×N"（整数）。
std::string rate_display(RateId id, int32_t value);

Rates default_rates();
// 把 rates 编码为单行 JSON 对象（字段顺序固定，整数）。
std::string encode_rates(const Rates& rates);
// 严格解析：只接受白名单字段与合法整数；缺字段用默认值；未知字段/重复字段/语法错误返回 false。
// 历史配置里的 "move_speed" 键（已移除的移速倍率）按合法字段解析后忽略，不使整份 rates 非法。
bool decode_rates(const char* json, uint32_t size, Rates* out);
// PluginDescriptor::validate_config 入口：只接受 key == "rates"，其余键拒绝。
QolResult validate_rates_config(const char* key, const char* json, uint32_t size);

// 百分比缩放：倍率 0 返回 0；正倍率结果钳到 [1, INT32_MAX]。
int32_t scale_by_percent(int32_t value, int32_t percent);
// 敌怪血量倍率 0 按 1e-6 缩放，正数结果下限为 1。
int32_t scale_attribute(int32_t raw, int32_t percent);
// 百分比缩放（金币，int64）：倍率 0 返回 0；int64 溢出时钳到 INT64_MAX。
int64_t scale_money(int64_t value, int32_t percent);

}  // namespace qol::features::world_tuning
