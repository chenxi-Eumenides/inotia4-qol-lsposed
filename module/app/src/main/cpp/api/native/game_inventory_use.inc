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
