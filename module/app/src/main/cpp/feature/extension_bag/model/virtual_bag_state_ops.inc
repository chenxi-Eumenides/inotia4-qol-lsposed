inline void normalize(State* state) {
    if (state == nullptr) return;
    for (int index = 0; index < kBagCount; ++index) {
        if (!valid_type(state->types[index])) state->types[index] = 0;
        state->capacities[index] = derive_capacity(state->types[index]);
    }
    for (int bag = 0; bag < kBagCount; ++bag) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
            Item& item = state->items[bag][slot];
            if (item.category < 0 || item.count < 0) {
                item = {};
            }
            if (item.payload_size > 0 && (!valid_payload(item) || item.category <= 0)) {
                // 损坏载荷不能降级为 category/count 素体（随机类别会被渲染成完全
                // 不同的物品），也不得默默转空槽：先保留原始字节为只读隔离记录
                // （P4.5），再清槽。
                IsolationRecord record{};
                record.valid = true;
                record.observed_at_ms = state->isolation_now_ms;
                record.reason = isolation_reason::kPayloadInvalid;
                record.src_bag = bag;
                record.src_slot = slot;
                record.payload_size = static_cast<uint16_t>(
                    std::min<size_t>(item.payload_size, item.payload.size()));
                record.payload = item.payload;
                record.detail = item.category <= 0
                                    ? "category_invalid"
                                    : payload_validation_reason(validate_serialized_payload_buffer(
                                          item.payload.data(), item.payload.size(),
                                          item.payload_size));
                push_isolation(state, record);
                item = {};
            }
        }
    }
    if (state->original_selected < 0 || state->original_selected >= 6) {
        state->original_selected = 0;
    }
    if (!valid_index(state->selected)) {
        state->selected = -1;
    }
    if (state->mode == Mode::kModule &&
        (!valid_index(state->selected) || state->capacities[state->selected] == 0)) {
        state->mode = Mode::kOriginal;
        state->selected = -1;
    }
    if (state->info_bag != -1 &&
        (!valid_index(state->info_bag) || state->capacities[state->info_bag] == 0 ||
         state->mode != Mode::kModule)) {
        state->info_bag = -1;
    }
    if (state->inspected != state->selected) state->inspected = -1;
}

inline void enter_original(State* state, int bag) {
    if (state == nullptr) return;
    state->mode = Mode::kOriginal;
    state->original_selected = bag >= 0 && bag < 6 ? bag : 0;
    state->selected = -1;
    state->inspected = -1;
}

inline void begin_exit_module(State* state) {
    if (state == nullptr) return;
    state->mode = Mode::kExitingModule;
    state->inspected = -1;
}

inline bool set_test_equipped(State* state, int index, int bag_type) {
    if (state == nullptr || !valid_index(index) || !valid_type(bag_type) || bag_type == 0) {
        return false;
    }
    state->types[index] = static_cast<uint8_t>(bag_type);
    normalize(state);
    return true;
}

inline bool unequip_bag(State* state, int index) {
    if (state == nullptr || !valid_index(index)) return false;
    for (const Item& item : state->items[index]) {
        if (item.category > 0 || item.count > 0) return false;
    }
    state->types[index] = 0;
    if (state->info_bag == index) state->info_bag = -1;
    if (state->selected == index) {
        state->selected = -1;
        state->mode = Mode::kOriginal;
    }
    normalize(state);
    return true;
}

// 真实装备（P3 装备路径）：占用空闲扩展位；容量经 normalize 由 bag_type
// 派生（与原版 ITEMSTATICBASE 1→4/2→8/3→12/4→16 同源）。已占用位拒绝。
inline bool equip_bag(State* state, int index, int bag_type) {
    if (state == nullptr || !valid_index(index) || !valid_type(bag_type) || bag_type == 0) {
        return false;
    }
    if (state->types[index] != 0) return false;
    state->types[index] = static_cast<uint8_t>(bag_type);
    normalize(state);
    return true;
}

inline ClickResult click(State* state, int index) {
    if (state == nullptr || !valid_index(index)) {
        return ClickResult::kIgnored;
    }
    if (state->capacities[index] == 0) {
        return ClickResult::kIgnored;
    }
    if (state->selected == index) {
        state->mode = Mode::kModule;
        state->info_bag = index;  // 二次点击 = 袋信息态（原版语义：desc_type=1 + MakeDesc）
        state->inspected = -1;
        return ClickResult::kInspected;
    }
    state->mode = Mode::kModule;
    state->selected = index;
    state->inspected = -1;
    state->info_bag = -1;
    return ClickResult::kSelected;
}

inline bool set_item(State* state, int bag, int slot, int category, int count) {
    if (state == nullptr || !valid_index(bag) || slot < 0 || slot >= kSlotCount ||
        category < 0 || count < 0) {
        return false;
    }
    state->items[bag][slot] = Item{category, count};
    return true;
}

// ---- JSON 桥接（Kotlin ExtensionBagUiBridge 往返，payload 以 base64 安全编码）----

