std::string data_op_use_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return op_err(err.c_str());
    }
    if (fn_get_bit == nullptr) return op_err("symbol not resolved");
    void* item = inventory_item_at(bag, slot);
    if (item == nullptr) return op_err("slot empty");
    uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
    int category = fn_get_bit(flags, 15, 6);
    // 解封/开箱/骰子类物品 fn_is_use 返回 0（IsUseAfterConfirm 判定集不含这些类别），
    // 但它们有自己的独立使用路径，不受 fn_is_use 限制
    bool sealed_or_box = (fn_is_sealed != nullptr && fn_is_sealed(category)) ||
                         (fn_is_item_box != nullptr && fn_is_item_box(category)) ||
                         (fn_is_dice != nullptr && fn_is_dice(category));
    if (!sealed_or_box && fn_is_use != nullptr && !fn_is_use(category))
        return op_err("item not usable");

    void* leader = member_or_null(0);

    // 按 UIEquip_SetDescMenu 按钮判定链分派（权威：反汇编 UI 按钮显隐逻辑）
    // 骰子（0x34-0x38）— 两段式：掷骰只生成 pending 返回变化量（不应用），接受/拒绝由 dice-accept/dice-reject 端点处理
    if (fn_is_dice != nullptr && fn_is_dice(category)) {
        if (fn_status_dice_roll == nullptr || fn_get_stat_base == nullptr)
            return op_err("symbol not resolved");
        if (leader == nullptr) return op_err("no leader");
        // 有未确认结果时拒绝再掷（flag bit0，ButtonRollExe 置位/Create+Apply 复位）
        uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
        if (flag != nullptr && (*flag & 1u)) return op_err("dice result pending, accept or reject first");
        int8_t char_idx = *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(leader) + 0xd);
        int type = category - 0x34;
        if (char_idx < 0 || char_idx > 5) return op_err("bad char");
        if (type < 0 || type > 4) return op_err("bad dice type");
        int base[5];
        for (int i = 0; i < 5; ++i) base[i] = fn_get_stat_base(leader, i);
        if (!fn_status_dice_roll(char_idx, type)) return op_err("dice roll failed");
        int8_t* pending = *reinterpret_cast<int8_t**>(g_base + G_STATUSDICE_PENDING_GOT_VMA);
        if (pending == nullptr) return op_err("dice result missing");
        // 无 pending 时掷骰即消耗（原版 ButtonRollExe 语义），置 flag 待确认
        if (fn_consume_item != nullptr) fn_consume_item(item);
        if (flag != nullptr) *flag |= 1u;
        extension_bag_sync_projected_slot(bag, slot);
        std::string s = "{\"ok\":true,\"base\":[";
        for (int i = 0; i < 5; ++i) {
            if (i > 0) s += ",";
            s += std::to_string(base[i]);
        }
        s += "],\"pending\":[";
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
    // 解封（0x3a6-0x3ab）— ITEMSYSTEM_ReleaseSealed 独立路径，成功后手动消耗
    if (fn_is_sealed != nullptr && fn_is_sealed(category) && fn_release_sealed != nullptr) {
        void* before[96] = {nullptr};
        for_each_bag_slot([](void* item, int b, int j, void* c) -> bool {
            static_cast<void**>(c)[b * 16 + j] = item;
            return false;
        }, before);
        int ok = fn_release_sealed(category);
        if (ok) {
            if (fn_consume_item != nullptr) fn_consume_item(item);
            extension_bag_sync_projected_slot(bag, slot);
            return "{\"ok\":true,\"gained\":[" + inventory_gained_json(before) + "]}";
        }
        return op_err("release sealed failed");
    }
    // 开箱（0x3ef-0x3f1）— ITEMSYSTEM_OpenItemBox 独立路径，成功后手动消耗
    if (fn_is_item_box != nullptr && fn_is_item_box(category) && fn_open_item_box != nullptr) {
        void* before[96] = {nullptr};
        for_each_bag_slot([](void* item, int b, int j, void* c) -> bool {
            static_cast<void**>(c)[b * 16 + j] = item;
            return false;
        }, before);
        int ok = fn_open_item_box(category);
        if (ok) {
            if (fn_consume_item != nullptr) fn_consume_item(item);
            extension_bag_sync_projected_slot(bag, slot);
            return "{\"ok\":true,\"gained\":[" + inventory_gained_json(before) + "]}";
        }
        return op_err("open box failed");
    }

    // 其余类型：CHAR_UseItemEx — 药水/卷轴/技能书/配方书/佣兵卡/增益/超药水/打包物
    //   内部成功时已调 INVEN_ConsumeItem；失败（CD/状态不符）不消耗
    if (leader == nullptr) return op_err("no leader");
    if (fn_char_use_item_ex == nullptr) return op_err("symbol not resolved");
    int ok = fn_char_use_item_ex(leader, item, 0);
    // 用药成功且药水教学激活（obj170==6）→ 复现官方 0xec340 教学完成链（CHAR_ProcessShortcut 用药后检查）
    if (ok && tutorial_state() == 6) tutorial_cancel();
    if (ok) extension_bag_sync_projected_slot(bag, slot);
    return ok ? op_ok() : op_err("on cooldown");
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
    if (!game_in_world()) return op_err("not in game");
    uint8_t* flag = *reinterpret_cast<uint8_t**>(g_base + G_STATUSDICE_FLAG_GOT_VMA);
    if (flag == nullptr || !(*flag & 1u)) return op_err("no dice result pending");
    *flag &= ~1u;
    return op_ok();
}
std::string data_op_discard_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_remove_item_direct == nullptr) return op_err("symbol not resolved");
    if (inventory_item_at(bag, slot) == nullptr) return op_err("slot empty");
    fn_remove_item_direct(bag, slot);
    if (inventory_item_at(bag, slot) != nullptr) return op_err("discard failed");
    extension_bag_sync_projected_slot(bag, slot);
    return op_ok();
}
std::string data_op_sell_item(int bag, int slot) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_remove_item_direct == nullptr || fn_add_money == nullptr || fn_item_get_price == nullptr)
        return op_err("symbol not resolved");
    void* item = inventory_item_at(bag, slot);
    if (item == nullptr) return op_err("slot empty");
    // 合法出售（v0.4.20）：ITEM_GetPrice 返回原始价格，出售价 = 原价 / 5
    // （改版币制：5 铜 = 1 银，出售价 = 真实价格 ÷ 5）
    int64_t base_price = fn_item_get_price(item);
    int64_t price = base_price / 5;
    fn_remove_item_direct(bag, slot);
    if (inventory_item_at(bag, slot) != nullptr) return op_err("sell failed");
    extension_bag_sync_projected_slot(bag, slot);
    fn_add_money(price);
    return "{\"ok\":true,\"price\":" + std::to_string(price) + "}";
}
std::string data_op_move_item(int bag, int slot, int count, int to_bag, int to_slot) {
    if (!game_in_world()) return op_err("not in game");
    if (extension_bag_module_view_installed()) {
        return op_err("extension view open; movement disabled (P2)");
    }
    if (fn_inven_move_item == nullptr) return op_err("symbol not resolved");
    void* item = inventory_item_at(bag, slot);
    if (item == nullptr) return op_err("slot empty");
    if (count <= 0) return op_err("bad count");
    if (to_bag < 0 || to_bag >= 6 || to_slot < 0 || to_slot >= 16) return op_err("bad target");
    if (bag == to_bag && slot == to_slot) return op_err("same slot");
    int r = fn_inven_move_item(item, count, to_bag, to_slot);
    // 返回 1=成功（mov w1,#0x1），0/失败返回空——按目标槽是否有物品判定
    if (r) {
        extension_bag_sync_projected_slot(bag, slot);
        extension_bag_sync_projected_slot(to_bag, to_slot);
    }
    return r ? op_ok() : op_err("move failed");
}

