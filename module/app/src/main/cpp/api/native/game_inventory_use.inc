std::string data_op_use_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (bag == 5) return op_err("task bag excluded");
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) return op_err("slot empty");
    if (item_ref.kind == InventoryItemKind::kExtension) return extension_bag_use_item(bag, slot);
    qol::contracts::QolItemInventoryStateWriteResult result{};
    const QolResult operation = qol::game::ops::use_item(bag, slot, &result);
    if (!qol::contracts::succeeded(operation)) {
        return op_err(operation.message != nullptr ? operation.message : "item use failed");
    }
    if ((result.flags & qol::contracts::kQolItemInventoryStateHasGained) == 0) {
        return "{\"ok\":true}";
    }
    return "{\"ok\":true,\"gained\":[" + inventory_gained_json(result) + "]}";
}
std::string data_op_dice_accept() {
    if (!game_in_world()) return op_err("not in game");
    if (fn_set_stat_base == nullptr || fn_get_stat_base == nullptr)
        return op_err("symbol not resolved");
    uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
    if (flag == nullptr || !(*flag & 1u)) return op_err("no dice result pending");
    void* leader = member_or_null(0);
    if (leader == nullptr) return op_err("no leader");
    int8_t* pending = *reinterpret_cast<int8_t**>(g_base + G_STATUSDICE_PENDING_GOT_VMA);
    if (pending == nullptr) return op_err("dice result missing");
    // 骰子已在掷骰时消耗，accept 只应用结果不重复消耗
    int base[5];
    for (int i = 0; i < 5; ++i) base[i] = fn_get_stat_base(leader, i);
    for (int i = 0; i < 5; ++i) fn_set_stat_base(leader, i, pending[i]);
    if (flag != nullptr) *flag &= ~1u;
    std::string s = "{\"ok\":true,\"base\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(base[i]);
    }
    s += "],\"applied\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(pending[i]);
    }
    s += "],\"delta\":[";
    for (int i = 0; i < 5; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(pending[i] - base[i]);
    }
    s += "]}";
    return s;
}
std::string data_op_dice_reject() {
    const QolResult result = qol::game::ops::reject_dice();
    return qol::contracts::succeeded(result) ? "{\"ok\":true}" : op_err(result.message);
}
// 处置原语已下沉至 core/native/inventory_trade.*（阶段 A）；此处仅做状态→JSON 映射，
// 错误文案/顺序与下沉前一致，source 固定 "api"。
std::string data_op_discard_item(int bag, int slot) {
    // I-1：先解析期望物品，传入处置原语做身份复核（并发下槽位被改动则不结算）。
    InventoryItemRef expected_ref;
    const bool found = inventory_item_ref_at(bag, slot, &expected_ref);
    switch (inventory_trade::discard(bag, slot, found ? expected_ref.native_item : nullptr)) {
        case inventory_trade::Status::kOk:
            return op_ok();
        case inventory_trade::Status::kNotInWorld:
            return op_err("not in game");
        case inventory_trade::Status::kTaskBagExcluded:
            return op_err("task bag excluded");
        case inventory_trade::Status::kEmptySlot:
            return op_err("slot empty");
        case inventory_trade::Status::kNoNativeItem:
            return op_err("extension item unavailable");
        case inventory_trade::Status::kStale:
            // 并发下槽位物品已变：对外沿用既有 not found 语义（不新增错误文案）。
            return op_err("slot empty");
        case inventory_trade::Status::kSymbolsUnresolved:
            return op_err("symbol not resolved");
        case inventory_trade::Status::kRemoveFailed:
            return op_err("discard failed");
        default:
            return op_err("discard failed");
    }
}
std::string data_op_sell_item(int bag, int slot) {
    // I-1：先解析期望物品，传入处置原语做身份复核（并发下槽位被改动则不结算）。
    InventoryItemRef expected_ref;
    const bool found = inventory_item_ref_at(bag, slot, &expected_ref);
    const inventory_trade::Result result =
        inventory_trade::sell(bag, slot, found ? expected_ref.native_item : nullptr, "api");
    switch (result.status) {
        case inventory_trade::Status::kOk:
            return "{\"ok\":true,\"price\":" + std::to_string(result.price) + "}";
        case inventory_trade::Status::kNotInWorld:
            return op_err("not in game");
        case inventory_trade::Status::kTaskBagExcluded:
            return op_err("task bag excluded");
        case inventory_trade::Status::kEmptySlot:
            return op_err("slot empty");
        case inventory_trade::Status::kNoNativeItem:
            return op_err("extension item unavailable");
        case inventory_trade::Status::kStale:
            // 并发下槽位物品已变：对外沿用既有 not found 语义（不新增错误文案）。
            return op_err("slot empty");
        case inventory_trade::Status::kSymbolsUnresolved:
            return op_err("symbol not resolved");
        case inventory_trade::Status::kNoSell:
            return op_err("item cannot be sold");
        case inventory_trade::Status::kPriceInvalid:
            return op_err("invalid sell price");
        case inventory_trade::Status::kCreditFailed:
            return op_err("sell money update failed");
        case inventory_trade::Status::kRemoveFailed:
            return op_err("sell failed");
        default:
            return op_err("sell failed");
    }
}
std::string data_op_move_item(int bag, int slot, int count, int to_bag, int to_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (bag == 5 || to_bag == 5) return op_err("task bag excluded");
    // D2b：逻辑袋判定 / 模块视图门禁经能力接口现取；能力缺失 = EB disabled，
    // 逻辑袋视为不存在、视图视为未安装（与 D2b-1 的降级口径一致）。
    // 扩展→扩展移动本身走 port（move_item 为 std::string 面，契约 v1.0 不含，D2d 收口）。
    const qol::contracts::ExtensionBagApi* eb = extension_bag_capability();
    if (eb != nullptr && eb->is_logical_bag != nullptr &&
        (eb->is_logical_bag(bag) || eb->is_logical_bag(to_bag))) {
        if (count <= 0) return op_err("bad count");
        if (to_slot < 0 || to_slot >= 16) return op_err("bad target");
        return extension_bag_move_item(bag, slot, to_bag, to_slot);
    }
    if (eb != nullptr && eb->module_view_installed != nullptr && eb->module_view_installed()) {
        return op_err("extension view open; movement disabled (P2)");
    }
    if (fn_inven_move_item == nullptr) return op_err("symbol not resolved");
    InventoryItemRef item_ref;
    if (!inventory_item_ref_at(bag, slot, &item_ref)) return op_err("slot empty");
    if (item_ref.kind == InventoryItemKind::kExtension) {
        return op_err("extension item move not supported by original API");
    }
    void* item = item_ref.native_item;
    if (count <= 0) return op_err("bad count");
    if (to_bag < 0 || to_bag >= 5 || to_slot < 0 || to_slot >= 16) return op_err("bad target");
    if (bag == to_bag && slot == to_slot) return op_err("same slot");
    int r = fn_inven_move_item(item, count, to_bag, to_slot);
    // 返回 1=成功（mov w1,#0x1），0/失败返回空——按目标槽是否有物品判定
    if (r) {
        if (eb != nullptr && eb->sync_projected_slot != nullptr) {
            eb->sync_projected_slot(bag, slot);
            eb->sync_projected_slot(to_bag, to_slot);
        }
    }
    return r ? op_ok() : op_err("move failed");
}
