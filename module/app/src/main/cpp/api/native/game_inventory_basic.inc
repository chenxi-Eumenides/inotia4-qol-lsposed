// ---- 辅助定义（背包共用）----
std::string inventory_gained_json(void* const* before) {
    if (fn_get_bit == nullptr || fn_get_cumulate_count == nullptr) {
        QOL_LOG_WARN(QolDomain::kInventory, "inventory_gained_json: fn_get_bit/fn_get_cumulate_count not resolved");
        return std::string();
    }
    std::string s;
    int n = 0;
    struct Ctx { void* const* before; std::string* s; int* n; } ctx{before, &s, &n};
    for_each_bag_slot([](void* item, int b, int j, void* c) -> bool {
        Ctx* p = static_cast<Ctx*>(c);
        // ITEM_GetCumulateCount 自动适配 patch：可堆叠返回数量、装备/不可堆叠返回 1（已归一化）
        int count = fn_get_cumulate_count(item);
        void* old = p->before[b * 16 + j];
        if (old == item) {
            count -= fn_get_cumulate_count(old);
            if (count <= 0) return false;
        } else if (old != nullptr) {
            return false;  // 同槽不同指针：旧物品被消耗/替换，非新增
        }
        if (*p->n > 0) *p->s += ",";
        uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        *p->s += "{\"bag\":" + std::to_string(b);
        *p->s += ",\"slot\":" + std::to_string(j);
        *p->s += ",\"category\":" + std::to_string(fn_get_bit(flags, 15, 6));
        *p->s += ",\"count\":" + std::to_string(count) + "}";
        ++*p->n;
        return false;
    }, &ctx);
    return s;
}

std::string data_op_set_money(int64_t money) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_set_money == nullptr) return op_err("symbol not resolved");
    fn_set_money(money);
    return op_ok();
}

std::string data_op_add_money(int64_t delta) {
    if (!game_in_world()) return op_err("not in game");
    if (fn_add_money == nullptr) return op_err("symbol not resolved");
    int r = fn_add_money(delta);
    return r ? op_ok() : op_err("add money failed");
}

std::string data_op_add_item(int32_t category, int32_t count, int32_t socket_total, int32_t enhance_remaining,
                             int32_t rarity_group) {
    if (!game_in_world()) return op_err("not in game");
    if (count <= 0) return op_err("bad count");
    if (fn_create_item == nullptr || fn_inven_save_item == nullptr || fn_inven_find_save_slot == nullptr ||
        fn_itempool_free == nullptr)
        return op_err("symbol not resolved");
    // ITEMSYSTEM_CreateItem 创建物品对象（MakeItem 带 search_tbl 校验会失败，CreateItem 无此限制）
    void* item = fn_create_item(category, 0, 0, 0);
    if (item == nullptr) return op_err("create item failed");
    // 孔位/强化构造（仅测试用，验证自动出售筛选 + 位级 A/B 实测；用户裁决 2026-09-15：
    // 只设「总孔位」与「剩余强化次数」两段）。CreateItem 产物 I_SOCKET/I_ENCHANT 恒为 0，
    // 缺省 0 时写入等价于不写。读改写、保留其余位：
    // I_SOCKET bits4-7=总孔数 socket_total（钳 0..15），bits0-3=已镶嵌数保持 0 不写；
    // I_ENCHANT bits2-5=剩余强化次数 enhance_remaining（钳 0..15，写 0 → 游戏内强化时
    // 提示耐久不足），bits6-10=已强化次数与 bits11-15=强化 ID 保持 0 不写，bit0/bit1 不动。
    // 已镶嵌数/已强化次数/强化 ID 由游戏自行维护（首次强化时游戏写强化 ID、镶嵌时游戏写
    // 已镶数），人工写入会造成游戏内不可能状态——实测：已强化=2 且强化 ID=0 时
    // ITEMSYSTEM_EnchantItem 返回 2、弹「与一般的强化卷轴不同」。
    // 消费方（自动出售筛选、图标、战斗）均为即时位域直读，无派生缓存需同步。
    // 负值按 0，超上限钳到上限（均 15）。
    if (socket_total < 0) socket_total = 0;
    if (socket_total > 15) socket_total = 15;
    if (enhance_remaining < 0) enhance_remaining = 0;
    if (enhance_remaining > 15) enhance_remaining = 15;
    uint8_t* p_socket = reinterpret_cast<uint8_t*>(item) + I_SOCKET;
    *p_socket = static_cast<uint8_t>((*p_socket & 0x0F) | ((socket_total & 0x0F) << 4));
    uint16_t* p_enchant = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_ENCHANT);
    *p_enchant = static_cast<uint16_t>((*p_enchant & ~(0x0F << 2)) | ((enhance_remaining & 0x0F) << 2));
    // 品质构造（仅测试用，验证自动出售品质筛选）：CreateItem 的 ApplyGrade 会随机掷
    // I_TYPE bits2-5 raw grade；rarity_group=-1（缺省）保留随机，0..4 指定档位
    // （白/绿/蓝/黄/紫）。其余值钳到 [-1,4]：所有负值视同 -1，>4 视同 4。
    // 读改写只动 bits2-5，保留 bits0-1 与 bits6-15（后者是 category，写坏会变成别的物品）。
    static constexpr uint8_t kRarityRawForGroup[5] = {0, 4, 7, 10, 11};  // 档位→raw_grade，映射待真机核实（ITEMGRADEBASE[raw].+4 rarity_group）
    if (rarity_group < -1) rarity_group = -1;
    if (rarity_group > 4) rarity_group = 4;
    if (rarity_group >= 0) {
        const uint8_t raw = kRarityRawForGroup[rarity_group];
        uint16_t* p_type = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        *p_type = static_cast<uint16_t>((*p_type & ~(0x0F << 2)) | ((raw & 0x0F) << 2));
    }
    // 可堆叠判定：ITEMCLASSBASE 记录 +6 bit0（与 ITEM_GetCumulateCount 同源）。
    // 类别表不可用时拒绝多数量写入，不能把未知类别当作可堆叠物品。
    const stack_codec::CountEncoding count_encoding = item_count_encoding(item);
    if (count > 1 && count_encoding != stack_codec::CountEncoding::kEncoded) {
        fn_itempool_free(item);
        return op_err(count_encoding == stack_codec::CountEncoding::kUnknown
                          ? "item category unavailable"
                          : "item not stackable");
    }
    if (count > 1) {
        // S2 创建写点：类别门控已在上方 fail-closed（非可堆叠拒绝多数量写入）；
        // 总量按模式 clamp（启用 999 / 关闭 99）后经 effective_write_count 写入：
        // 启用态全量拆段（b 溢出 127 的进位 →a+1、b 归零由编码内建），关闭态只写
        // b、保留 a（R-47 决策 b），不得改用裸位段直写。
        count = static_cast<int>(stack_codec::effective_clamp(static_cast<uint32_t>(count),
                                                              stack_limit_enabled()));
        uint32_t cf = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT);
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(item) + I_COUNT) =
            stack_codec::effective_write_count(cf, static_cast<uint32_t>(count),
                                               stack_limit_enabled());
    }
    int8_t save_slot = -1;
    if (!fn_inven_find_save_slot(item, &save_slot)) {
        fn_itempool_free(item);
        return op_err("inventory full");
    }
    if (!fn_inven_save_item(item, nullptr)) {
        fn_itempool_free(item);
        return op_err("save item failed");
    }
    return op_ok();
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
        if (!extension_bag_remove_native_item(ref.native_item)) {
            return op_err("extension item removal failed");
        }
        extension_bag_sync_projected_slot(ref.bag, ref.slot);
        return op_ok();
    }
    if (ref.bag == 5) return op_err("task bag excluded");
    const int result = fn_remove_item(ref.native_item);
    if (result) extension_bag_sync_projected_slot(ref.bag, ref.slot);
    return result ? op_ok() : op_err("item not found");
}
