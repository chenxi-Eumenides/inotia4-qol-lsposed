#pragma once
#include "../../contracts/item_payload.h"
namespace qol::features::item_lab {
// 固定测试装备描述。rarity 0..4 = 白/绿/蓝/黄/紫；原生词条数必须等于 rarity 对应档位
// （白 0/绿 1/蓝 2/黄 3/紫 4）。socket_filled 必须等于 type=1 宝石节点数且不超过孔数；
// 只有 socket_filled == socket_total 时才允许 1~2 个 type=2 附魔节点。
struct EquipmentSpec {
    int32_t category;
    uint8_t rarity;
    uint8_t socket_total;
    uint8_t socket_filled;
    uint8_t native_option_count;
    uint8_t gem_option_count;
    uint8_t enchant_option_count;
};
bool build_equipment_payload(const EquipmentSpec& spec, contracts::QolItemPayloadBlob* out);
}
