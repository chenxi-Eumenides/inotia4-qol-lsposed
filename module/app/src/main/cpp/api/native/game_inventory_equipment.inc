std::string data_op_jewel(int role, int bag, int slot, int equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (extension_bag_is_logical_bag(bag)) {
        return extension_bag_put_jewel(role, bag, slot, equip_slot);
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_put_jewel == nullptr || fn_is_jewel == nullptr || fn_remove_item_direct == nullptr)
        return op_err("symbol not resolved");
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) return op_err("bad slot");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad equip slot");
    void* jewel = inventory_item_at(bag, slot);
    if (jewel == nullptr) return op_err("jewel not found");
    int extension_bag = -1;
    int extension_slot = -1;
    if (extension_bag_identify_native_item(jewel, &extension_bag, &extension_slot)) {
        return extension_bag_put_jewel(role, extension_bag + 6, extension_slot, equip_slot);
    }
    void* equip = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) + C_EQUIP + equip_slot * 8);
    if (equip == nullptr) return op_err("equip slot empty");
    int r = fn_put_jewel(equip, jewel);
    if (r != 0) return r == 2 ? op_err("no socket") : op_err("not jewel");
    // PutJewel 不消耗宝石物品本身，镶嵌成功后手动删除背包中的宝石（防刷宝石）
    fn_remove_item_direct(bag, slot);
    extension_bag_sync_projected_slot(bag, slot);
    return op_ok();
}
std::string data_op_enchant(int role, int bag, int slot, int equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_enchant_item == nullptr || fn_is_enchant_scroll == nullptr || fn_consume_item == nullptr || fn_get_bit == nullptr)
        return op_err("symbol not resolved");
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) return op_err("bad slot");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad equip slot");
    void* equip = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) + C_EQUIP + equip_slot * 8);
    if (equip == nullptr) return op_err("equip slot empty");
    void* scroll = inventory_item_at(bag, slot);
    if (scroll == nullptr) return op_err("scroll not found");
    uint16_t sflags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(scroll) + I_TYPE);
    int scroll_cat = fn_get_bit(sflags, 15, 6);
    if (!fn_is_enchant_scroll(scroll_cat)) return op_err("not enchant scroll");
    // ITEMSYSTEM_EnchantItem 成功(0)后写回 +0x1A 新附魔等级；卷轴消耗由调用方负责（UIEquip_ApplyStuff 同款）
    int r = fn_enchant_item(equip, scroll_cat);
    if (r != 0) return r == 7 ? op_err("not enchantable") : op_err("enchant failed");
    fn_consume_item(scroll);
    extension_bag_sync_projected_slot(bag, slot);
    return op_ok();
}
std::string data_op_equip(int role, int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (g_inven == nullptr || fn_equip_item == nullptr || fn_can_equip == nullptr)
        return op_err("symbol not resolved");
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) return op_err("bad slot");
    uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + bag * 0x80;
    void* item = *reinterpret_cast<void**>(bag_slots + slot * 8);
    if (item == nullptr) return op_err("slot empty");
    int extension_bag = -1;
    int extension_slot = -1;
    if (extension_bag_identify_native_item(item, &extension_bag, &extension_slot)) {
        if (fn_find_equip_slot == nullptr) return op_err("symbol not resolved");
        const int target = fn_find_equip_slot(ch, item);
        if (target < 0 || target >= C_EQUIP_SLOTS) return op_err("cannot equip");
        return extension_bag_equip_projected_item(item, extension_bag, extension_slot, target)
            ? op_ok() : op_err("extension equip failed");
    }
    if (!fn_can_equip(ch, item)) return op_err("cannot equip");
    // CHAR_EquipItem 在目标槽已被占用时返回 0；先查槽位，占用则自动脱下旧装备再穿。
    if (fn_find_equip_slot != nullptr && fn_get_equip_item != nullptr && fn_unequip != nullptr) {
        int target = fn_find_equip_slot(ch, item);
        if (target >= 0 && target < C_EQUIP_SLOTS) {
            void* occupied = fn_get_equip_item(ch, target);
            if (occupied != nullptr) {
                fn_unequip(ch, target);
            }
        }
    }
    int r = fn_equip_item(ch, item);
    if (r) extension_bag_sync_projected_bag();
    return r ? op_ok() : op_err("equip failed");
}
std::string data_op_unequip(int role, int32_t equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_unequip == nullptr) return op_err("symbol not resolved");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad slot");
    int r = fn_unequip(ch, equip_slot);
    if (r) extension_bag_sync_projected_bag();
    return r ? op_ok() : op_err("unequip failed");
}
