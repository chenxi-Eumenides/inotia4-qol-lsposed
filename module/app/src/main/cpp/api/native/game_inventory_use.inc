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
