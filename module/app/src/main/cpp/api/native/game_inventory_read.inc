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
