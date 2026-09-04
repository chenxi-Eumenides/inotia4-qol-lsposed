// 懒注入线程：合成器界面（UIMix）只在玩家打开合成器时才创建（UIMix_CreateMainControl），
// 启动时（main_menu）宝石按钮槽 [0x305550+0xa0] 为空，立即注入会失败。这里后台轮询，
// 槽非空（界面已打开）时注入；关闭配置时由 data_craft_btn_set_enabled(false) 还原。
void ensure_craft_inject_thread() {
    if (g_craft_thread_started.exchange(true)) return;
    std::thread([]() {
        for (;;) {
            if (g_craft_want.load() && !g_craft_injected && g_base != 0 && g_uimix != nullptr) {
                void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_GEM_BTN);
                if (*slot != nullptr) {
                    data_craft_btn_inject();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

void data_craft_btn_set_enabled(bool enabled) {
    g_craft_want.store(enabled);
    if (enabled) {
        ensure_craft_inject_thread();
    } else {
        data_craft_btn_remove();
    }
}

// 批量宝石合成（按钮 ExecuteProc 回调，x0=控件对象）。
// 遍历第一袋 16 槽，按宝石类别 cat28-31（排除混沌 32）分组，每组 3 个一批走
// MIXSYSTEM_MakeItem（词条定向继承由游戏自动处理，无需复刻）→ 产物入库 → 消耗 3 材料。
// 失败（配方/背包满/金币不足）安全停止；完成后一次性扣费并只更新游戏内存。
void data_op_mix_gem_batch(void* ctrl) {
    (void)ctrl;
    if (g_base == 0 || g_inven == nullptr) {
        MOVE_LOG("mix_gem_batch: libgame not ready");
        return;
    }
    if (!game_in_world()) {
        MOVE_LOG("mix_gem_batch: not in game");
        return;
    }
    if (fn_make_mix == nullptr || fn_get_cost == nullptr || fn_is_jewel == nullptr ||
        fn_remove_item_direct == nullptr || fn_inven_save_item == nullptr ||
        fn_minus_money == nullptr || fn_get_bit == nullptr ||
        fn_get_money == nullptr) {
        MOVE_LOG("mix_gem_batch: symbol not resolved");
        return;
    }
    // 扫描第一袋，按类别分组记录槽位（cat28-31，排除混沌 32 及异常类别）
    int slot_by_cat[4][16];
    int cnt[4] = {0, 0, 0, 0};
    for (int slot = 0; slot < 16; ++slot) {
        void* item = inventory_item_at(0, slot);
        if (item == nullptr) continue;
        uint16_t flags = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        int cat = fn_get_bit(flags, 15, 6);
        if (!fn_is_jewel(cat)) continue;
        if (cat < 28 || cat > 31) continue;
        int idx = cat - 28;
        if (cnt[idx] < 16) slot_by_cat[idx][cnt[idx]++] = slot;
    }
    int64_t total_cost = 0;
    int made = 0;
    const char* stop_reason = nullptr;
    for (int idx = 0; idx < 4 && stop_reason == nullptr; ++idx) {
        int mix_type = 12 + idx;  // 12/13/14/15 = 低级/中级/高级/顶级 → 上一级
        int used = 0;
        while (cnt[idx] - used >= 3) {
            int64_t cost = fn_get_cost(mix_type, nullptr);
            if (cost < 0) { stop_reason = "cost failed"; break; }
            if (fn_get_money() < total_cost + cost) { stop_reason = "not enough money"; break; }
            void* out = nullptr;
            if (fn_make_mix(mix_type, &out) != 0 || out == nullptr) {
                stop_reason = "make item failed";
                break;
            }
            if (!fn_inven_save_item(out, nullptr)) {
                // 产物入库失败（背包满）：out 未入库，仅单个对象泄漏（每次点击至多 1 个），停止
                stop_reason = "inventory full";
                break;
            }
            // 成功：消耗 3 材料（RemoveItemDirect 仅清空该槽，不移动其它槽，槽位索引保持有效）
            for (int k = 0; k < 3; ++k) fn_remove_item_direct(0, slot_by_cat[idx][used + k]);
            used += 3;
            total_cost += cost;
            ++made;
        }
    }
    if (total_cost > 0) fn_minus_money(total_cost);
    MOVE_LOG("mix_gem_batch: made=%d cost=%lld%s%s", made, static_cast<long long>(total_cost),
             stop_reason != nullptr ? " stop=" : "", stop_reason != nullptr ? stop_reason : "");
}

// 注入批量合成按钮（方案 2：mmap 新建 ControlObject + 按钮数据，复用 [0x3055f0] 宝石按钮槽）。
// 绘制（UIMix_Draw）硬编码枚举固定槽 → 写新指针后即显示新按钮；
// 点击（ControlObject_EventProc）递归控件树遍历，原宝石按钮仍在树中 → 同时覆盖原按钮
// ExecuteProc 为批量合成，保证点击触发本函数。
bool data_craft_btn_inject() {
    std::lock_guard<std::mutex> lock(g_craft_mtx);
    if (g_craft_injected) return true;
    if (g_base == 0 || g_uimix == nullptr) {
        MOVE_LOG("craft_btn_inject: libgame not ready");
        return false;
    }
    void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_GEM_BTN);
    uint8_t* orig = reinterpret_cast<uint8_t*>(*slot);
    if (orig == nullptr) {
        MOVE_LOG("craft_btn_inject: gem button slot empty");
        return false;
    }
    uint8_t* orig_data = *reinterpret_cast<uint8_t**>(orig + CO_DATA);
    if (orig_data == nullptr) {
        MOVE_LOG("craft_btn_inject: gem button data empty");
        return false;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    size_t total = CO_SIZE + CB_SIZE;
    size_t len = (total + static_cast<size_t>(page) - 1) / static_cast<size_t>(page) * static_cast<size_t>(page);
    void* region = mmap(nullptr, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        MOVE_LOG("craft_btn_inject: mmap failed");
        return false;
    }
    uint8_t* co = reinterpret_cast<uint8_t*>(region);   // 新 ControlObject
    uint8_t* cb = co + CO_SIZE;                          // 新按钮数据
    memset(co, 0, CO_SIZE);
    memset(cb, 0, CB_SIZE);

    // 复刻原宝石按钮 rect/贴图，替换点击回调为批量合成
    *reinterpret_cast<uint32_t*>(co + CO_TYPE) = 3;
    *reinterpret_cast<uint32_t*>(co + CO_ACTIVE) = 0x20;
    *reinterpret_cast<int64_t*>(co + CO_RECT_X) = *reinterpret_cast<int64_t*>(orig + CO_RECT_X);
    *reinterpret_cast<int64_t*>(co + CO_RECT_Y) = *reinterpret_cast<int64_t*>(orig + CO_RECT_Y);
    *reinterpret_cast<int64_t*>(co + CO_RECT_W) = *reinterpret_cast<int64_t*>(orig + CO_RECT_W);
    *reinterpret_cast<int64_t*>(co + CO_RECT_H) = *reinterpret_cast<int64_t*>(orig + CO_RECT_H);
    *reinterpret_cast<uint64_t*>(co + CO_USERTYPE) = 1;
    *reinterpret_cast<void**>(co + CO_DATA) = cb;
    *reinterpret_cast<uint32_t*>(co + CO_COUNT) = 0;
    *reinterpret_cast<uint32_t*>(co + CO_EVENT_CALL_TYPE) = 0x200;
    *reinterpret_cast<uintptr_t*>(co + CO_PROC) = g_base + F_TOUCH_HANDLE_CONTROL_EVENT_PROC_VMA;
    *reinterpret_cast<uintptr_t*>(co + CO_CONTROL_PROC) = g_base + F_CONTROL_BUTTON_CONTROL_EVENT_PROC_VMA;
    *reinterpret_cast<void**>(co + CO_PARENT) = nullptr;

    *reinterpret_cast<uintptr_t*>(cb + CB_EXECUTE_PROC) = reinterpret_cast<uintptr_t>(&data_op_mix_gem_batch);
    *reinterpret_cast<uint32_t*>(cb + CB_DRAW_TYPE) = *reinterpret_cast<uint32_t*>(orig_data + CB_DRAW_TYPE);
    *reinterpret_cast<int64_t*>(cb + CB_DRAW_ID) = *reinterpret_cast<int64_t*>(orig_data + CB_DRAW_ID);
    *reinterpret_cast<int64_t*>(cb + CB_DRAW_SUB_ID) = *reinterpret_cast<int64_t*>(orig_data + CB_DRAW_SUB_ID);
    *reinterpret_cast<uintptr_t*>(cb + CB_DRAW_PROC) = g_base + F_UIMIX_BUTTON_DRAW_MIXING_GEM_VMA;
    *reinterpret_cast<uint8_t*>(cb + CB_STATE) = 0;
    *reinterpret_cast<uint8_t*>(cb + CB_ENABLED) = 1;

    // 槽指针写新按钮（绘制走固定槽）；覆盖原按钮 ExecuteProc（点击走控件树）
    g_craft_exec_hook.install_typed(orig_data + CB_EXECUTE_PROC, &data_op_mix_gem_batch);
    *slot = co;

    g_craft_orig = orig;
    g_craft_mmap = region;
    g_craft_mmap_len = len;
    g_craft_injected = true;
    MOVE_LOG("craft_btn_inject: ok, orig=%p new=%p", reinterpret_cast<void*>(orig),
             reinterpret_cast<void*>(co));
    return true;
}

// 还原宝石按钮槽指针 + 原 ExecuteProc，并释放 mmap。
void data_craft_btn_remove() {
    std::lock_guard<std::mutex> lock(g_craft_mtx);
    if (!g_craft_injected) return;
    if (g_uimix != nullptr && g_craft_orig != nullptr) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_GEM_BTN);
        g_craft_exec_hook.uninstall();
        *slot = g_craft_orig;
    }
    if (g_craft_mmap != nullptr) munmap(g_craft_mmap, g_craft_mmap_len);
    g_craft_mmap = nullptr;
    g_craft_mmap_len = 0;
    g_craft_orig = nullptr;
    g_craft_injected = false;
    MOVE_LOG("craft_btn_remove: restored");
}
