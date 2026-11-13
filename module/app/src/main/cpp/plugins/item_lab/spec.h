#pragma once
#include "../../contracts/item_payload.h"
namespace qol::features::item_lab {
// 固定测试装备描述。rarity 0..4 = 白/绿/蓝/黄/紫；raw_prefix < 0 时按 rarity 映射
// （白0/绿4/蓝7/黄10/紫11），>=0 时直接写入 I_TYPE bits2-5，用于构造前缀与品质矛盾。
// 词条/宝石/附魔节点数量上限均为 4；socket_total/socket_filled 为 4 位字段。
struct EquipmentSpec {
    int32_t category;
    uint8_t rarity;
    int8_t raw_prefix;
    uint8_t socket_total;
    uint8_t socket_filled;
    uint8_t native_option_count;
    uint8_t gem_option_count;
    uint8_t enchant_option_count;
    uint8_t enchant_level;
    uint8_t enchant_remaining;
    uint8_t enchant_id;
};
bool build_equipment_payload(const EquipmentSpec& spec, contracts::QolItemPayloadBlob* out);
}
