// ---- 辅助定义（背包共用）----
std::string inventory_gained_json(
    const qol::contracts::QolItemInventoryStateWriteResult& result) {
    std::string s;
    for (uint32_t i = 0; i < result.gained_count; ++i) {
        if (i != 0) s.push_back(',');
        const auto& item = result.gained[i];
        s += "{\"bag\":" + std::to_string(item.bag) +
             ",\"slot\":" + std::to_string(item.slot) +
             ",\"category\":" + std::to_string(item.category) +
             ",\"count\":" + std::to_string(item.count) + "}";
    }
    return s;
}

std::string data_op_add_money(int64_t delta) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_add_money == nullptr) return op_err("symbol not resolved");
    int r = fn_add_money(delta);
    return r ? op_ok() : op_err("add money failed");
}

std::string data_op_add_item(int32_t category, int32_t count, int32_t socket_total, int32_t enhance_remaining,
                             int32_t rarity_group) {
    qol::contracts::QolItemInventoryStateWriteResult operation_data{};
    const QolResult result = qol::game::ops::add_item(
        category, count, socket_total, enhance_remaining, rarity_group,
        qol::game::item_count_api_if_resident(), &operation_data);
    return qol::contracts::succeeded(result) ? "{\"ok\":true}" : op_err(result.message);
}

std::string data_op_minus_money(int64_t delta) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_minus_money == nullptr) return op_err("symbol not resolved");
    int r = fn_minus_money(delta);
    return r ? op_ok() : op_err("insufficient money");
}

std::string data_op_remove_item(int32_t category) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_remove_item == nullptr || fn_get_bit == nullptr) return op_err("symbol not resolved");
    InventoryItemRef ref;
    if (!find_inventory_item_ref(category, &ref)) return op_err("item not found");
    if (ref.native_item == nullptr) return op_err("extension item unavailable");
    if (ref.kind == InventoryItemKind::kExtension) {
        // D2b：扩展物品删除经能力接口现取；能力缺失 = EB disabled，删除失败降级
        //（与 port 在 disabled 下返回 false 的行为一致）。
        const qol::contracts::ExtensionBagApi* eb = extension_bag_capability();
        if (eb == nullptr || eb->remove_native_item == nullptr ||
            !eb->remove_native_item(ref.native_item)) {
            return op_err("extension item removal failed");
        }
        // 投影同步：port 在 disabled 下为 no-op（无模块视图），能力缺失时跳过等价。
        if (eb->sync_projected_slot != nullptr) eb->sync_projected_slot(ref.bag, ref.slot);
        return op_ok();
    }
    if (ref.bag == 5) return op_err("task bag excluded");
    const int result = fn_remove_item(ref.native_item);
    if (result) {
        const qol::contracts::ExtensionBagApi* eb = extension_bag_capability();
        if (eb != nullptr && eb->sync_projected_slot != nullptr) {
            eb->sync_projected_slot(ref.bag, ref.slot);
        }
    }
    return result ? op_ok() : op_err("item not found");
}
