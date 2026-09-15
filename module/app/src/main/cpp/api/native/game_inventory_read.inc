void append_item_attrs(std::string& s, void* item) {
    uint8_t* it = reinterpret_cast<uint8_t*>(item);
    if (fn_get_damage != nullptr) {
        s += ",\"damage\":" + std::to_string(fn_get_damage(item));
    }
    if (fn_get_defense != nullptr) {
        s += ",\"defense\":" + std::to_string(fn_get_defense(item));
    }
    s += ",\"magic_rate\":" + std::to_string(it[I_MAGIC_RATE]);
    if (fn_item_get_ability_level != nullptr) {
        s += ",\"need_level\":" + std::to_string(fn_item_get_ability_level(item));
    }
    // v0.4.64 位域拆解（docs/systems/inventory.md §2.4 反汇编确认）
    uint8_t socket = it[I_SOCKET];
    s += ",\"socket\":" + std::to_string(socket);
    s += ",\"socket_filled\":" + std::to_string((socket >> 0) & 0x0F);
    s += ",\"socket_total\":" + std::to_string((socket >> 4) & 0x0F);
    uint16_t enchant = *reinterpret_cast<uint16_t*>(it + I_ENCHANT);
    s += ",\"enchant\":" + std::to_string(enchant);
    s += ",\"chaos\":" + std::string((enchant & 1) ? "true" : "false");
    s += ",\"enchant_id\":" + std::to_string((enchant >> 11) & 0x1F);
    s += ",\"enchant_level\":" + std::to_string((enchant >> 6) & 0x1F);
    uint32_t cnt = *reinterpret_cast<uint32_t*>(it + I_COUNT);
    s += ",\"chaos_level\":" + std::to_string((cnt >> 0) & 0xFF);
    s += ",\"chaos_rate\":" + std::to_string((cnt >> 8) & 0xFF);
    // options = 词缀值数组（兼容旧字段）；optionIds = 词缀索引数组（节点 +0x00 低 7 位，与 options 对齐）
    uint8_t* opt = *reinterpret_cast<uint8_t**>(it + I_OPTION_LIST);
    bool ofirst = true;
    int ocount = 0;
    s += ",\"options\":[";
    while (opt != nullptr && ocount < 32) {
        if (!ofirst) s += ",";
        s += std::to_string(*reinterpret_cast<int16_t*>(opt + O_VALUE));
        ofirst = false;
        opt = *reinterpret_cast<uint8_t**>(opt + O_NEXT);
        ++ocount;
    }
    s += "]";
    uint8_t* opt2 = *reinterpret_cast<uint8_t**>(it + I_OPTION_LIST);
    ofirst = true;
    ocount = 0;
    s += ",\"option_ids\":[";
    while (opt2 != nullptr && ocount < 32) {
        if (!ofirst) s += ",";
        s += std::to_string(*reinterpret_cast<uint16_t*>(opt2 + O_INDEX) & 0x7F);
        ofirst = false;
        opt2 = *reinterpret_cast<uint8_t**>(opt2 + O_NEXT);
        ++ocount;
    }
    s += "]";
    // option_types：词缀节点 type（O_INDEX bit13-15：0=词缀 1=宝石），与 option_ids 对齐，Kotlin 据此拆分 bonus/gem
    uint8_t* opt3 = *reinterpret_cast<uint8_t**>(it + I_OPTION_LIST);
    ofirst = true;
    ocount = 0;
    s += ",\"option_types\":[";
    while (opt3 != nullptr && ocount < 32) {
        if (!ofirst) s += ",";
        s += std::to_string((*reinterpret_cast<uint16_t*>(opt3 + O_INDEX) >> 13) & 0x7);
        ofirst = false;
        opt3 = *reinterpret_cast<uint8_t**>(opt3 + O_NEXT);
        ++ocount;
    }
    s += "]";
}

// item_count_encoding / item_is_equip 已下沉至 data/native/item_class.{h,cpp}
// （声明经 game_inventory.h 引用）。

int canonical_item_count(void* item) {
    if (item == nullptr) return 0;
    // 可堆叠类别：绕过 H-17 getter 直接按 S2 解码——getter 在堆叠上限关闭态按
    // R-47 决策 b 只返回 b 视图，而物化/持久化需要完整 canonical 128a+b。
    if (item_count_encoding(item) == stack_codec::CountEncoding::kEncoded) {
        return static_cast<int>(stack_codec::s2_read_count(
            *reinterpret_cast<const uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT)));
    }
    return fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 0;
}

std::string build_inventory_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    // INVEN_pItem(G_INVEN_VMA)：背包槽数组，6 袋 × 0x80 步长，每槽 8B 物品指针。
    // 每袋 16 槽（6×16=96，与真机实测 slotCount 总和一致）。
    // 空槽=0 指针；物品 +0x08 类型位域(u16)、+0x10 数量位域(u32)。
    constexpr int BAG_COUNT = 6;
    constexpr size_t BAG_STRIDE = 0x80;
    constexpr int SLOTS_PER_BAG = 16;
    std::string s = "{\"bags\":[";
    for (int b = 0; b < BAG_COUNT; ++b) {
        if (b > 0) s += ",";
        s += "{\"bag\":" + std::to_string(b) + ",\"items\":[";
        int filled = 0;
        if (g_inven != nullptr) {
            uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + b * BAG_STRIDE;
            for (int j = 0; j < SLOTS_PER_BAG; ++j) {
                void* item = *reinterpret_cast<void**>(bag_slots + j * 8);
                if (item == nullptr) continue;
                uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
                if (filled > 0) s += ",";
                s += "{\"slot\":" + std::to_string(j);
                s += ",\"type_flags\":" + std::to_string(flags);
                s += ",\"raw_rarity\":" + std::to_string((flags >> 2) & 0x0F);
                if (fn_get_bit != nullptr) {
                    s += ",\"category\":" + std::to_string(fn_get_bit(flags, 15, 6));
                }
                if (fn_get_cumulate_count != nullptr) {
                    // count 语义：ITEM_GetCumulateCount 按类型与模式区分——可堆叠
                    // 类返回模式视图数量（启用态 S2 全量 count=128a+b、关闭态低 7
                    // 位 b，getter hook 统一解码，见规则册 R-49/R-47），不可堆叠
                    // 类（装备）返回 1（旧实现裸读位域导致装备 count=100）
                    s += ",\"count\":" + std::to_string(fn_get_cumulate_count(item));
                }
                s += std::string(",\"equip\":") + (item_is_equip(item) ? "true" : "false");
                if (fn_get_rarity != nullptr) {
                    s += ",\"rarity\":" + std::to_string(fn_get_rarity(item));
                }
                append_item_attrs(s, item);
                s += "}";
                ++filled;
            }
        }
        s += "],\"capacity\":16,\"slot_count\":" + std::to_string(filled) + "}";
    }
    // 扩展逻辑袋 6..10（extensionBagEnabled 且 world 时并入；schema 与原版袋一致）
    s += extension_bag_inventory_bags_json();
    s += "]}";
    return s;
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

std::string data_debug_item_raw_json(int32_t bag, int32_t slot) {
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
    s += ",\"kind\":" + std::string(ref.kind == InventoryItemKind::kExtension ? "\"extension\"" : "\"original\"");
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
