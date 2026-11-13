#pragma once

#include <cstdint>

// 装备字段的统一限制口径。这里区分两类：
// 1) 原生/表示限制：游戏字段位宽或静态表决定，写入越界会产生游戏无法解释的对象；
// 2) 系统安全限制：模块为可生成/可修改装备采用的保守上限，游戏原生并不一定拦截。
//
// 数据来源：I_TYPE/I_SOCKET/I_ENCHANT 位域、ITEMGRADEBASE 记录、Monster v29 真机测试。
namespace qol::contracts {

// ---- 原生/表示限制 ----
constexpr int32_t kItemPrefixRawBits = 4;
constexpr int32_t kItemPrefixRawMax = 14;  // ITEMGRADEBASE 只有 15 条；raw=15 越界
constexpr int32_t kItemSocketTotalBits = 4;
constexpr int32_t kItemSocketTotalMax = 15;
constexpr int32_t kItemSocketFilledBits = 4;
constexpr int32_t kItemSocketFilledMax = 15;
constexpr int32_t kItemEnchantRemainingBits = 4;
constexpr int32_t kItemEnchantRemainingMax = 15;
constexpr int32_t kItemEnchantLevelBits = 5;
constexpr int32_t kItemEnchantLevelMax = 31;
constexpr int32_t kItemEnchantIdBits = 5;
constexpr int32_t kItemEnchantIdMax = 31;
constexpr uint32_t kItemEquipmentMarker = 100;  // I_COUNT bits25-31；>99 = 已鉴定装备
constexpr uint32_t kItemEquipmentMarkerShift = 25;
constexpr uint32_t kItemEquipmentMarkerMask = 0x7Fu << kItemEquipmentMarkerShift;

// ITEMGRADEBASE 记录 +4：raw prefix -> GetRarity 返回的品质组 0..4。
// raw 0..3=白、4..6=绿、7..9=蓝、10=黄、11..14=紫。
constexpr int8_t kItemRawPrefixRarity[15] = {
    0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 4, 4, 4, 4};

constexpr int32_t item_rarity_from_raw_prefix(int32_t raw_prefix) {
    return raw_prefix >= 0 && raw_prefix <= kItemPrefixRawMax
               ? kItemRawPrefixRarity[raw_prefix]
               : -1;
}

// payload 总 option 节点上限：(255 - 19) / 4。
constexpr int32_t kItemPayloadMaxOptionNodes = 59;

// ---- 系统安全限制（模块生成/修改口径）----
// 原生词条数按品质的常规档位；Monster 允许白装 4 词条，但生成默认不采用该非法组合。
constexpr int32_t kItemSystemNativeOptionsByRarity[5] = {0, 1, 2, 3, 4};
// 常规装备最多 4 孔；字段可写到 15，但 15 属于原生可表示、系统不生成的范围。
constexpr int32_t kItemSystemMaxSockets = 4;
// 当前 Monster 强化等级上限 12；旧系统理论上限 15。字段可表示 31，但继续强化可能崩溃。
constexpr int32_t kItemSystemMaxEnchantLevel = 12;
constexpr int32_t kItemSystemMaxEnchantRemaining = 15;
// item.facts 每个分词列表容量 32；超过后枚举标记截断。
constexpr int32_t kItemSystemMaxParsedOptionNodes = 32;
// 真机已验证 4 个 type=2 附魔可显示；未发现原生单独上限，系统默认按 4 生成。
constexpr int32_t kItemSystemTestedEnchantNodes = 4;

static_assert(kItemPrefixRawMax < (1 << kItemPrefixRawBits), "prefix field width");
static_assert(kItemSocketTotalMax == (1 << kItemSocketTotalBits) - 1, "socket width");
static_assert(kItemEnchantLevelMax == (1 << kItemEnchantLevelBits) - 1, "enchant level width");
static_assert(kItemEnchantIdMax == (1 << kItemEnchantIdBits) - 1, "enchant id width");
static_assert(kItemPayloadMaxOptionNodes == (255 - 19) / 4, "payload option budget");

}  // namespace qol::contracts
