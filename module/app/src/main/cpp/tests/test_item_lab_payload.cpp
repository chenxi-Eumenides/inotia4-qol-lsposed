#include <cassert>
#include "../plugins/item_lab/spec.h"

using namespace qol::contracts;
using qol::features::item_lab::EquipmentSpec;

static void decode_and_check(const EquipmentSpec& spec, uint8_t raw, uint8_t native_count,
                             uint8_t gem_count, uint8_t enchant_count,
                             uint8_t socket_total, uint8_t socket_filled) {
    QolItemPayloadBlob blob{};
    assert(qol::features::item_lab::build_equipment_payload(spec, &blob));
    QolItemPayloadHeader header{};
    QolItemPayloadOption options[12]{};
    uint32_t count = 0;
    assert(item_payload_decode(blob.payload, blob.payload_size, &header, options, 12, &count));
    assert(count == native_count + gem_count + enchant_count);
    assert(((header.type_flags >> 6) & 0x3ffu) == static_cast<uint32_t>(spec.category));
    assert(((header.type_flags >> 2) & 0xfu) == raw);
    assert(header.count_field == (100u << 25));
    assert(header.magic_rate == 100);
    assert(header.socket == ((socket_total << 4) | socket_filled));
    assert(header.enchant == static_cast<uint16_t>(((spec.enchant_id & 0x1f) << 11) |
                                                   ((spec.enchant_level & 0x1f) << 6) |
                                                   ((spec.enchant_remaining & 0x0f) << 2)));
    uint32_t seen_native = 0, seen_gem = 0, seen_enchant = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t type = (options[i].code >> 13) & 0x7u;
        if (type == 0) ++seen_native;
        else if (type == 1) ++seen_gem;
        else if (type == 2) ++seen_enchant;
        assert((options[i].code & 0x1f80u) == 0);
    }
    assert(seen_native == native_count && seen_gem == gem_count &&
           seen_enchant == enchant_count);
    assert(item_payload_structurally_valid(blob.payload, blob.payload_size));
}

int main() {
    decode_and_check({462, 0, -1, 15, 0, 4, 0, 0, 31, 15, 1}, 0, 4, 0, 0, 15, 0);
    decode_and_check({462, 0, -1, 1, 4, 0, 4, 0, 0, 0, 0}, 0, 0, 4, 0, 1, 4);
    decode_and_check({462, 2, -1, 2, 2, 2, 2, 4, 0, 0, 0}, 7, 2, 2, 4, 2, 2);
    decode_and_check({462, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0}, 9, 0, 0, 0, 0, 0);
    decode_and_check({462, 4, 0, 0, 0, 4, 0, 0, 0, 0, 0}, 0, 4, 0, 0, 0, 0);

    // 4 附魔节点的固定顺序：id29=100、id0=500、id31=50、id30=10。
    QolItemPayloadBlob blob{};
    assert(qol::features::item_lab::build_equipment_payload(
        {462, 2, -1, 2, 2, 2, 2, 4, 0, 0, 0}, &blob));
    QolItemPayloadHeader header{};
    QolItemPayloadOption options[12]{};
    uint32_t count = 0;
    assert(item_payload_decode(blob.payload, blob.payload_size, &header, options, 12, &count));
    assert(count == 8);
    assert(((options[4].code >> 13) & 0x7u) == 2 && (options[4].code & 0x7fu) == 29 &&
           options[4].value == 100);
    assert(((options[5].code >> 13) & 0x7u) == 2 && (options[5].code & 0x7fu) == 0 &&
           options[5].value == 500);
}
