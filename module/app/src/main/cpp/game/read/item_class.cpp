// item_class.cpp —— 数据层物品类别原语（自 api/native/game_inventory_read.inc 下沉）。

#include "item_class.h"

#include "contracts/item_count.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/access/game_access.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"
#include "game_symbols.h"

#include <cstdint>
#include <string>

qol::contracts::QolItemCountEncoding item_count_encoding(void* item) {
    if (item == nullptr) return qol::contracts::QolItemCountEncoding::kUnknown;
    const qol::contracts::QolItemCountApi* api = qol::game::item_count_api_if_resident();
    if (api == nullptr || api->is_encoded == nullptr) {
        return qol::contracts::QolItemCountEncoding::kUnknown;
    }
    qol::contracts::QolItemCountEncoding encoding = qol::contracts::QolItemCountEncoding::kUnknown;
    const QolResult result = api->is_encoded(item, &encoding);
    return qol::contracts::succeeded(result) ? encoding : qol::contracts::QolItemCountEncoding::kUnknown;
}

bool item_is_equip(void* item) {
    return item_count_encoding(item) == qol::contracts::QolItemCountEncoding::kNotEncoded;
}

bool item_is_backpack(int category) {
    if (g_base == 0 || category < 0) return false;
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return item_is_backpack_from_class_data(category, class_data, stride);
}

bool category_is_no_equip(int category) {
    if (g_base == 0 || category < 0) return false;
    uint8_t* class_data =
        *reinterpret_cast<uint8_t**>(*reinterpret_cast<void**>(g_base + G_ITEMCLASS_DATA_GOT_VMA));
    uint8_t* size_ptr = *reinterpret_cast<uint8_t**>(g_base + G_ITEMCLASS_SIZE_GOT_VMA);
    const uint8_t stride = size_ptr != nullptr ? *size_ptr : 0;
    return category_is_no_equip_from_class_data(category, class_data, stride);
}

namespace {

// 调试定长 hex 输出（前导零补位），仅 /api/debug/item/raw 使用。
std::string dbg_hex(unsigned long value, int digits) {
    static const char kHexDigits[] = "0123456789abcdef";
    std::string s = "0x";
    for (int i = digits - 1; i >= 0; --i) {
        s += kHexDigits[(value >> (i * 4)) & 0xFu];
    }
    return s;
}

}  // namespace

std::string debug_item_raw_json(int32_t bag, int32_t slot) {
    // GET /api/debug/item/raw?bag=&slot=（仅测试/调试）：输出槽位物品原始字节 + 位分解，
    // 用于位级 A/B 实测（强化 / 镶宝石后哪一位段变化）。纯读取，不写任何游戏内存；
    // bag 0..10（6..10 扩展袋经 inventory_item_ref_at 稳定端口取物，取锁/放锁自管）。
    if (!game_in_world()) return op_err("not in world");
    InventoryItemRef ref{};
    if (!inventory_item_ref_at(bag, slot, &ref) || ref.native_item == nullptr) {
        return op_err("item not found");
    }
    uint8_t* it = reinterpret_cast<uint8_t*>(ref.native_item);
    const uint16_t type_raw = *reinterpret_cast<uint16_t*>(it + I_TYPE);
    const uint32_t count_raw = *reinterpret_cast<uint32_t*>(it + I_COUNT);
    const uint8_t socket_raw = it[I_SOCKET];
    const uint16_t enchant_raw = *reinterpret_cast<uint16_t*>(it + I_ENCHANT);
    std::string s = "{\"ok\":true";
    s += ",\"bag\":" + std::to_string(ref.bag);
    s += ",\"slot\":" + std::to_string(ref.slot);
    s += ",\"kind\":" + std::string(ref.kind == InventoryItemKind::kProvider ? "\"extension\"" : "\"original\"");
    s += ",\"category\":" + std::to_string(ref.category);
    s += ",\"count\":" + std::to_string(ref.count);
    // I_TYPE(0x08,u16)：bits2-5=raw grade（品质，档位映射待真机核实），bits6-15=category
    s += ",\"type_raw\":\"" + dbg_hex(type_raw, 4) + "\"";
    s += ",\"raw_grade\":" + std::to_string((type_raw >> 2) & 0x0F);
    // I_COUNT(0x10,u32)：可堆叠数量为 128a+b 编码原值，仅直出 raw
    s += ",\"count_raw\":\"" + dbg_hex(count_raw, 8) + "\"";
    // I_SOCKET(0x19,u8)：bits4-7=总孔数，bits0-3=已镶嵌数
    s += ",\"socket_raw\":\"" + dbg_hex(socket_raw, 2) + "\"";
    s += ",\"socket_total\":" + std::to_string((socket_raw >> 4) & 0x0F);
    s += ",\"socket_filled\":" + std::to_string(socket_raw & 0x0F);
    // I_ENCHANT(0x1A,u16)：bits6-10=已强化次数，bits2-5=未知（待实测，疑为剩余次数），
    // bits11-15=强化 ID，bit0=混沌
    s += ",\"enchant_raw\":\"" + dbg_hex(enchant_raw, 4) + "\"";
    s += ",\"enchant_count\":" + std::to_string((enchant_raw >> 6) & 0x1F);
    s += ",\"enhance_low4\":" + std::to_string((enchant_raw >> 2) & 0x0F);
    s += ",\"enchant_id\":" + std::to_string((enchant_raw >> 11) & 0x1F);
    s += ",\"chaos\":" + std::to_string(enchant_raw & 1u);
    s += ",\"note\":\"raw fields for bit-level verification\"}";
    return s;
}
