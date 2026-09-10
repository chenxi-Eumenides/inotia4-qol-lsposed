// 固定 canonical 布局 patch 点（反汇编逐字节确认）。
// 数量 patch 表准入规则：必须先证明目标位段是数量语义；装备标记、容量等非数量
// 位段不得纳入数量布局或上限 patch 表。
#define P(macro, off, o, r) { #macro, macro, off, o, r }
const PatchEntry g_stack_layout_patches[] = {
    // 位段扩展 mov w2,#0x19(bitStart=25) → mov w2,#0x16(bitStart=22)
    P(F_GET_CUMULATE_COUNT_VMA, 0x78, 0x52800322, 0x528002C2),
    P(F_INVEN_MOVE_ITEM_VMA, 0x178, 0x52800322, 0x528002C2),
    P(F_INVEN_MOVE_ITEM_VMA, 0x194, 0x52800322, 0x528002C2),
    P(F_INVEN_MOVE_ITEM_VMA, 0x224, 0x52800322, 0x528002C2),
    P(F_INVEN_MOVE_ITEM_VMA, 0x23c, 0x52800322, 0x528002C2),
    P(F_CONSUME_ITEM_VMA, 0x58, 0x52800322, 0x528002C2),
    P(F_CONSUME_ITEM_VMA, 0x80, 0x52800322, 0x528002C2),
    P(F_CONSUME_ITEM_VMA, 0x98, 0x52800322, 0x528002C2),
    P(F_CREATE_ITEM_VMA, 0x26c, 0x52800322, 0x528002C2),
    P(F_ITEMSYSTEM_DIVIDE_VMA, 0x68, 0x52800322, 0x528002C2),
    P(F_ITEMSYSTEM_DIVIDE_VMA, 0x90, 0x52800322, 0x528002C2),
    P(F_ITEMSYSTEM_DIVIDE_VMA, 0xa4, 0x52800322, 0x528002C2),
    P(F_ITEMSYSTEM_DIVIDE_VMA, 0xc0, 0x52800322, 0x528002C2),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xe8, 0x52800322, 0x528002C2),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0x110, 0x52800322, 0x528002C2),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0x168, 0x52800322, 0x528002C2),
    P(F_INVEN_SAVE_ITEM_DATA_VMA, 0x88, 0x52800322, 0x528002C2),
    P(F_INVEN_CHECK_SAVE_IN_NOT_EMPTY_SLOT_VMA, 0x14c, 0x52800322, 0x528002C2),
    P(F_INVEN_REMOVE_ITEM_DATA_VMA, 0x18c, 0x52800322, 0x528002C2),
    P(F_INVEN_GET_CUMULATE_SAVE_SLOT_EX_VMA, 0x15c, 0x52800322, 0x528002C2),
    P(F_INVEN_GET_CUMULATE_SAVE_SLOT_EX_VMA, 0x174, 0x52800322, 0x528002C2),
    P(F_UISTORE_BUTTON_SELL_EXE_VMA, 0xc0, 0x52800322, 0x528002C2),
    P(F_UISTORE_BUTTON_SELL_EXE_VMA, 0xd8, 0x52800322, 0x528002C2),
    P(F_UISTORE_SELL_ITEM_VMA, 0xd0, 0x52800322, 0x528002C2),
    // 商店复制购买路径写入数量起始位 bit25 → bit22，保持与 CreateItem/GetCumulateCount 同布局
    P(F_UISTORE_BUY_ITEM_VMA, 0x1a8, 0x52800322, 0x528002C2),
    P(F_GAME_START_NEW_GAME_VMA, 0xac, 0x52800322, 0x528002C2),
    P(F_MAKE_ITEM_VMA, 0x370, 0x52800322, 0x528002C2),
    P(F_ITEMSYSTEM_PROCESS_UNPACK_VMA, 0x354, 0x52800322, 0x528002C2),
    P(F_MAPITEMSYSTEM_CREATE_ITEM_VMA, 0xb4, 0x52800322, 0x528002C2),
    P(F_NETWORKSTORE_ADD_ITEM_VMA, 0x110, 0x52800322, 0x528002C2),
    P(F_SAVE_REVISE_CHARACTER_LOCATION_VMA, 0x248, 0x52800322, 0x528002C2),
    P(F_UIMIX_START_MIX_VMA, 0x180, 0x52800322, 0x528002C2),
    // 存档子物品检查位段缩窄 mov w1,#0x18(bitEnd=24) → mov w1,#0x15(bitEnd=21)
    P(F_SAVE_SAVE_INVENTORY_VMA, 0x78, 0x52800301, 0x528002A1),
    P(F_SAVE_LOAD_INVENTORY_VMA, 0xa8, 0x52800301, 0x528002A1),
};
const size_t g_stack_layout_patch_count = sizeof(g_stack_layout_patches) / sizeof(g_stack_layout_patches[0]);

// 可逆上限 patch：固定布局不变，只切换新操作的 99/999 上限。
const PatchEntry g_stack_limit_clamp_patches[] = {
    // 99→999 clamp（cmp/mov #0x63→#0x3E7，#0x62→#0x3E6）；仅限已证明的数量语义路径。
    P(F_INVEN_MOVE_ITEM_VMA, 0x150, 0x71018c1f, 0x710f9c1f),
    P(F_INVEN_MOVE_ITEM_VMA, 0x158, 0x52800c77, 0x52807cf7),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xdc, 0x71018c7f, 0x710f9c7f),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xf0, 0x71018e9f, 0x710f9e9f),
    P(F_INVEN_SAVE_ITEM_DATA_VMA, 0x7c, 0x71018a9f, 0x710f9a9f),
    P(F_INVEN_SAVE_ITEM_DATA_VMA, 0x8c, 0x52800c63, 0x52807ce3),
    P(F_INVEN_FIND_SAVE_SLOT_VMA, 0x238, 0x71018c1f, 0x710f9c1f),
    P(F_INVEN_CHECK_SAVE_IN_NOT_EMPTY_SLOT_VMA, 0xa4, 0x52800c7a, 0x52807cfa),
    P(F_INVEN_GET_CUMULATE_SAVE_SLOT_EX_VMA, 0x164, 0x7101881f, 0x710f981f),
    P(F_INVEN_GET_CUMULATE_SAVE_SLOT_EX_VMA, 0x17c, 0x52800c62, 0x52807ce2),
};
#undef P
const size_t g_stack_limit_clamp_patch_count = sizeof(g_stack_limit_clamp_patches) / sizeof(g_stack_limit_clamp_patches[0]);
PatchSet g_stack_layout_patch_set(g_stack_layout_patches, g_stack_layout_patch_count);
PatchSet g_stack_limit_clamp_patch_set(g_stack_limit_clamp_patches, g_stack_limit_clamp_patch_count);

bool patch_apply(const PatchEntry* entries, size_t n) {
    if (entries == nullptr) return false;
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    size_t done = 0;
    for (size_t i = 0; i < n; ++i) {
        const PatchEntry& e = entries[i];
        uintptr_t addr = patch_addr(e);
        if (addr == 0) { write_back(entries, done); return false; }
        uint32_t cur = *reinterpret_cast<uint32_t*>(addr);
        if (cur == e.replacement) { ++done; continue; }
        if (cur != e.orig) {
            __android_log_print(ANDROID_LOG_ERROR, PATCH_TAG, "patch_apply mismatch %s+0x%x got 0x%08x want 0x%08x",
                                e.func_macro, e.func_offset, cur, e.orig);
            write_back(entries, done);
            return false;
        }
        if (!write_insn(addr, e.replacement)) { write_back(entries, done); return false; }
        __android_log_print(ANDROID_LOG_INFO, PATCH_TAG, "patch_apply %s+0x%x 0x%08x -> 0x%08x",
                            e.func_macro, e.func_offset, e.orig, e.replacement);
        ++done;
    }
    return true;
}

bool patch_revert(const PatchEntry* entries, size_t n) {
    if (entries == nullptr) return false;
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    size_t done = 0;
    for (size_t i = 0; i < n; ++i) {
        const PatchEntry& e = entries[i];
        uintptr_t addr = patch_addr(e);
        if (addr == 0) return false;
        uint32_t cur = *reinterpret_cast<uint32_t*>(addr);
        if (cur == e.orig) { ++done; continue; }
        if (cur != e.replacement) {
            __android_log_print(ANDROID_LOG_ERROR, PATCH_TAG, "patch_revert mismatch %s+0x%x got 0x%08x want 0x%08x",
                                e.func_macro, e.func_offset, cur, e.replacement);
            return false;
        }
        if (!write_insn(addr, e.orig)) return false;
        __android_log_print(ANDROID_LOG_INFO, PATCH_TAG, "patch_revert %s+0x%x 0x%08x -> 0x%08x",
                            e.func_macro, e.func_offset, e.replacement, e.orig);
        ++done;
    }
    return true;
}

bool apply_fixed_stack_layout() {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    if (g_stack_layout_patch_set.applied()) return true;
    bool ok = g_stack_layout_patch_set.apply();
    __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, PATCH_TAG,
                        "stack canonical layout %s", ok ? "applied" : "failed");
    return ok;
}

bool set_stack_limit_enabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0 || !g_stack_layout_patch_set.applied()) return false;
    if (enabled == g_stack_enabled.load()) return true;
    if (enabled) {
        if (!g_stack_limit_clamp_patch_set.apply()) return false;
        g_stack_enabled.store(true);
        return true;
    }
    bool ok = g_stack_limit_clamp_patch_set.revert();
    g_stack_enabled.store(false);
    return ok;
}

bool stack_limit_enabled() {
    return g_stack_enabled.load();
}

std::string data_recover_after_hive_block() {
    if (g_base == 0) return op_err("base not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    if (fn_networkstore_set_state == nullptr) return op_err("symbol not resolved");
    uint32_t** daily_trigger = reinterpret_cast<uint32_t**>(g_base + G_DAILY_TRIGGER_GOT_VMA);
    if (*daily_trigger != nullptr) **daily_trigger = 1;
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (*hud_gate != nullptr) **hud_gate = 1;
    fn_networkstore_set_state(0);
    fn_ui_set_popup_process_info(4, 0);
    return op_ok();
}
