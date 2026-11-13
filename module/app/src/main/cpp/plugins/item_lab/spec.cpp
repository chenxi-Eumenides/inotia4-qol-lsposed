#include "spec.h"
namespace qol::features::item_lab {
namespace {
constexpr uint8_t kRawRarity[5] = {0, 4, 7, 10, 11};
constexpr contracts::QolItemPayloadOption kNativeOptions[] = {
    {0, 5}, {1, 5}, {2, 5}, {3, 5}};
// type=1：已镶嵌宝石节点；id 使用 ITEMOPTINFOBASE 属性索引，值固定。
constexpr contracts::QolItemPayloadOption kGemOptions[] = {
    {static_cast<uint16_t>(1u << 13), 10},
    {static_cast<uint16_t>((1u << 13) | 1u), 8},
    {static_cast<uint16_t>((1u << 13) | 2u), 6},
    {static_cast<uint16_t>((1u << 13) | 3u), 4}};
// type=2：Monster 可见附魔节点。29 为百分比类型，0 为整数类型，用于交叉错误测试。
constexpr contracts::QolItemPayloadOption kEnchantOptions[] = {
    {static_cast<uint16_t>((2u << 13) | 29u), 100},
    {static_cast<uint16_t>((2u << 13) | 0u), 500},
    {static_cast<uint16_t>((2u << 13) | 31u), 50},
    {static_cast<uint16_t>((2u << 13) | 30u), 10}};
constexpr uint32_t kEquipmentMarker = 100u << 25;
}
bool build_equipment_payload(const EquipmentSpec& spec, contracts::QolItemPayloadBlob* out) {
    if (out == nullptr || spec.rarity > 4 ||
        spec.native_option_count > 4 || spec.gem_option_count > 4 ||
        spec.enchant_option_count > 4) {
        return false;
    }
    contracts::QolItemPayloadOption options[12]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < spec.native_option_count; ++i) options[count++] = kNativeOptions[i];
    for (uint32_t i = 0; i < spec.gem_option_count; ++i) options[count++] = kGemOptions[i];
    for (uint32_t i = 0; i < spec.enchant_option_count; ++i) options[count++] = kEnchantOptions[i];
    const uint8_t raw = spec.raw_prefix < 0 ? kRawRarity[spec.rarity]
                                            : static_cast<uint8_t>(spec.raw_prefix & 0x0F);
    const uint16_t enchant_word = static_cast<uint16_t>(
        ((spec.enchant_id & 0x1F) << 11) | ((spec.enchant_level & 0x1F) << 6) |
        ((spec.enchant_remaining & 0x0F) << 2));
    const contracts::QolItemPayloadHeader header{
        0,
        static_cast<uint16_t>((static_cast<uint32_t>(spec.category) << 6) |
                              (static_cast<uint32_t>(raw) << 2)),
        kEquipmentMarker,
        100,
        static_cast<uint8_t>((spec.socket_total << 4) | (spec.socket_filled & 0x0F)),
        enchant_word};
    if (!contracts::item_payload_build(header, options, count, out->payload,
                                       sizeof(out->payload), &out->payload_size)) {
        return false;
    }
    out->struct_size = sizeof(*out);
    return true;
}
}
