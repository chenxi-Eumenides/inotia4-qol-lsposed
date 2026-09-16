std::string data_op_jewel(int role, int bag, int slot, int equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (extension_bag_is_logical_bag(bag)) {
        return extension_bag_put_jewel(role, bag, slot, equip_slot);
    }
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_put_jewel == nullptr || fn_is_jewel == nullptr || fn_consume_item == nullptr)
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
    // 原版统一入口 UIEquip_ApplyStuff(0xb8df8) 在 PutJewel/EnchantItem 成功后统一走
    // INVEN_ConsumeItem 消耗材料（0xb8ee4），而非 INVEN_RemoveItem：ConsumeItem 对可堆叠
    // 物品只减 1 个，RemoveItem 会整组删除——改版可堆叠宝石下用 RemoveItem 会一次删光
    // 整组宝石（与原版行为不符）。此处对齐原版改用 ConsumeItem。
    // 不直接调 ApplyStuff 的原因：其成功路径含 SOUNDSYSTEM_Play（离线 API 线程调用音频
    // 引擎句柄为空会崩溃）与失败弹窗（UIPopupMsg_CreateOK，API 场景会向游戏内注入弹窗），
    // 故拆用其内部分派的原版底层函数 PutJewel + ConsumeItem，效果与原版一致。
    fn_consume_item(jewel);
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
    // 原版链：UIEquip_ApplyStuff(0xb8df8) 判定 IsEnchantScroll 后调 ITEMSYSTEM_EnchantItem
    // （0xb8e38），成功(0)后写回 +0x1A 新附魔等级并统一 INVEN_ConsumeItem 消耗卷轴
    // （0xb8ee4）——本实现与该链一致（EnchantItem + ConsumeItem）。
    // 不直接调 ApplyStuff 的原因同 data_op_jewel：其成功路径的 SOUNDSYSTEM_Play 离线程
    // 会崩、失败分支弹游戏内弹窗（0xb8ef8 查表 UIPopupMsg_CreateOK），API 拆用底层函数
    // 并把失败码映射为 API 错误文案。
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
    // 口径说明（backlog 跟踪项）：仅接受原版 5 袋（bag 0..4）；扩展背包逻辑袋 6..10 的
    // 物品不在此路由到原版装备入口——原版 CHAR_EquipItemFromInvenToSlot 只认 g_inven
    // 物理槽，扩展物品无原生槽位编码。与 UI 详情装备路径（经扩展投影适配，可穿扩展物品）
    // 口径不一致为已知待裁定项，不做静默兼容。
    if (bag < 0 || bag >= 5 || slot < 0 || slot >= 16) return op_err("bad slot");
    uint8_t* bag_slots = reinterpret_cast<uint8_t*>(g_inven) + bag * 0x80;
    void* item = *reinterpret_cast<void**>(bag_slots + slot * 8);
    if (item == nullptr) return op_err("slot empty");
    if (!fn_can_equip(ch, item)) return op_err("cannot equip");
    // 原版交换入口 CHAR_EquipItemFromInvenToSlot：目标槽被占用时自动把旧装备
    // 放回背包（交换语义），失败返回 0。不做手动查槽+卸下的原版模拟。
    // 该入口已被 Stage4 Hook，扩展投影物品会透明路由到扩展装备适配，
    // 无需上层识别。
    if (fn_find_equip_slot == nullptr || fn_equip_item_from_inven_to_slot == nullptr)
        return op_err("symbol not resolved");
    const int target = fn_find_equip_slot(ch, item);
    if (target < 0 || target >= C_EQUIP_SLOTS) return op_err("cannot equip");
    const int r = fn_equip_item_from_inven_to_slot(ch, bag, slot, target);
    if (r) extension_bag_sync_projected_bag();
    return r ? op_ok() : op_err("equip failed");
}
std::string data_op_unequip(int role, int32_t equip_slot) {
    if (!game_in_world()) return op_err("not in game");
    void* ch = member_or_null(role);
    if (ch == nullptr) return op_err("role not found");
    if (fn_unequip == nullptr) return op_err("symbol not resolved");
    if (equip_slot < 0 || equip_slot >= C_EQUIP_SLOTS) return op_err("bad slot");
    void* item = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ch) + C_EQUIP + equip_slot * 8);
    if (item == nullptr) return op_err("equip slot empty");
    // 原版判定前置（写操作规范第 1 条：调执行函数先查判定函数）：原版脱装备按钮链经
    // CHAR_CanUnequipItem(ch, item)（0xe4e2c：ITEMDATABASE[cat].+7 bit4 不可脱标记 +
    // CHAR_CanChangeEquip 特殊 NPC/锁定角色闸门）与 CHAR_CanEquipItem 同源校验后才执行；
    // API 无 UI 按钮链，必须显式补同一判定，避免对原版 UI 永远不会脱下的装备直调执行函数。
    if (fn_can_unequip != nullptr && fn_can_unequip(ch, item) == 0) return op_err("cannot unequip");
    int r = fn_unequip(ch, equip_slot);
    if (r) extension_bag_sync_projected_bag();
    return r ? op_ok() : op_err("unequip failed");
}
