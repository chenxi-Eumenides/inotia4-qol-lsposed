// S2 数量布局（a=bits22-24、b=bits25-31、count=128a+b，见 core/native/stack_codec.h）下的
// 常驻 patch 表（反汇编逐字节确认）。
// S2 布局中 bits25-31 就是原版 7 位字段 b：原版 UTIL_Get/SetBitValue(start=25,end=31)
// 的读写天然作用于 b 且不破坏 a（bits22-24），S1 时代「把 bitStart 25 改 22 的单一
// 10 位段布局 patch」与 S2 编码冲突（count<<22 的错位模式会写坏 a/b 段），已全部移除。
// 数量 patch 表准入规则不变：必须先证明目标位段是数量语义；装备 marker（bits25-31）、
// 袋容量（bits0-24）、宝石选项（bits18-23）等非数量位段不得纳入本表。
#define P(macro, off, o, r) { #macro, macro, off, o, r }
const PatchEntry g_stack_layout_patches[] = {
    // 存档子物品检查位段缩窄 mov w1,#0x18(bitEnd=24) → mov w1,#0x15(bitEnd=21)。
    // SAVE_SaveInventory/SAVE_LoadInventory 以 GetBitValue(+0x10, end=24, start=0)
    // 判断袋对象是否有子物品（容量位非零）。S2 下 bits22-24 装可堆叠数量的高位段 a，
    // count≥128 的普通物品会让该检查误判出「子物品」，因此检查位段必须继续避开
    // bits22-24——这条缩窄在 S2 下仍然必要且语义不变（bits0-21 为容量/非数量位）。
    P(F_SAVE_SAVE_INVENTORY_VMA, 0x78, 0x52800301, 0x528002A1),
    P(F_SAVE_LOAD_INVENTORY_VMA, 0xa8, 0x52800301, 0x528002A1),
};
const size_t g_stack_layout_patch_count = sizeof(g_stack_layout_patches) / sizeof(g_stack_layout_patches[0]);

// 可逆上限 clamp patch（启用态 apply / 关闭态 revert）。
// S2 语义口径：clamp 值 999 只允许作用于「数量完整值」判定点——即比较数来自
// ITEM_GetCumulateCount（S2 getter hook 后返回 128a+b 总量）或函数入参总量的指令；
// 直接读取 b 段（mod 128 残量）的判定点不进本表：999 对 b 值无意义且会高估剩余
// 空间（不 fail-closed），保持原版 99 的 b 满判定即可安全拒绝溢出合并。
// 已知边界（S2-P4 前的开放点）：启用态合并/创建总量 >127 时，原版写点
// SetBitValue(start=25,end=31) 只能写 b、无法对 a 进位；≤127 全部正确，>127 的
// 进位由 S2-P4 原版袋 +0x10 迁移扫描或后续调用方级方案闭合（禁止 hook UTIL_SetBitValue，
// 见规则册 R-46/R-49）。
//
// 出售/拆堆直接位读的重定向（VM-37，见 g_stack_getter_redirect_patches）：
// 这些读点原为 `ldr w0,[xN,#0x10]; bl UTIL_GetBitValue(_,31,25)` 只见 b 段，
// 改为 `mov x0,xN; bl ITEM_GetCumulateCount(item)`——getter 已按类别门控 + 模式
// 感知解码（启用态 128a+b、关闭态 b），故重定向常驻（不随开关 revert）。
// 注意：不能把 GetBitValue 的 start 常量 25→22 当作全量读——窗口 (v>>22)&0x3FF
// 在数值上是 `a + 8b`（a 在窗口低位、b 在高位），与 `a*128+b` 不等价。
const PatchEntry g_stack_limit_clamp_patches[] = {
    // 总量 999 clamp（cmp/mov #0x63→#0x3E7，#0x62→#0x3E6）；判定数均为完整数量值。
    P(F_INVEN_MOVE_ITEM_VMA, 0x150, 0x71018c1f, 0x710f9c1f),
    P(F_INVEN_MOVE_ITEM_VMA, 0x158, 0x52800c77, 0x52807cf7),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xdc, 0x71018c7f, 0x710f9c7f),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xf0, 0x71018e9f, 0x710f9e9f),
    P(F_INVEN_SAVE_ITEM_DATA_VMA, 0x7c, 0x71018a9f, 0x710f9a9f),
    P(F_INVEN_FIND_SAVE_SLOT_VMA, 0x238, 0x71018c1f, 0x710f9c1f),
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
            QOL_LOG_ERROR(QolDomain::kPlatform, "patch_apply mismatch %s+0x%x got 0x%08x want 0x%08x",
                                e.func_macro, e.func_offset, cur, e.orig);
            write_back(entries, done);
            return false;
        }
        if (!write_insn(addr, e.replacement)) { write_back(entries, done); return false; }
        QOL_LOG_INFO(QolDomain::kPlatform, "patch_apply %s+0x%x 0x%08x -> 0x%08x",
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
            QOL_LOG_ERROR(QolDomain::kPlatform, "patch_revert mismatch %s+0x%x got 0x%08x want 0x%08x",
                                e.func_macro, e.func_offset, cur, e.replacement);
            return false;
        }
        if (!write_insn(addr, e.orig)) return false;
        QOL_LOG_INFO(QolDomain::kPlatform, "patch_revert %s+0x%x 0x%08x -> 0x%08x",
                            e.func_macro, e.func_offset, e.replacement, e.orig);
        ++done;
    }
    return true;
}

// ---- S2 直接位读重定向（VM-37，常驻）----
// 调用点原序：`ldr w0,[xN,#0x10]`（或 `mov w0,wN`，N 持有物品指针）→
// `bl UTIL_GetBitValue(_,31,25)`。重定向后 x0=物品指针、bl 到
// ITEM_GetCumulateCount（已 hook：类别门控 + 模式感知 S2 解码）。
// bl 目标同属 libgame.so，imm26 按 g_base 运行时计算（无需 thunk）。
// 第 4 个参数 arg_repl 是 arg 点的替换指令：物品指针所在寄存器随调用点不同
// （x19 用 0xaa1303e0，x21 用 0xaa1503e0），禁止默认套用同一寄存器。
#define R(func, arg_off, arg_orig, arg_repl, call_off, call_orig, target) \
    { #func, func, arg_off, arg_orig, arg_repl, call_off, call_orig, #target, target }
const S2ReadRedirectEntry g_stack_getter_redirect_patches[] = {
    // UIStore_ButtonSellExe：0xd18d0（b≤1 整堆判定）、0xd18e8（输入框上限）。
    R(F_UISTORE_BUTTON_SELL_EXE_VMA, 0xb8, 0xb9401260, 0xaa1303e0, 0xc4, 0x9401bb13, F_GET_CUMULATE_COUNT_VMA),
    R(F_UISTORE_BUTTON_SELL_EXE_VMA, 0xd0, 0xb9401260, 0xaa1303e0, 0xdc, 0x9401bb0d, F_GET_CUMULATE_COUNT_VMA),
    // UIStore_SellItem：0xd26b8（b≤1 整堆判定）。
    R(F_UISTORE_SELL_ITEM_VMA, 0xc8, 0xb9401260, 0xaa1303e0, 0xd4, 0x9401b799, F_GET_CUMULATE_COUNT_VMA),
    // ITEMSYSTEM_Divide：0x108458（count≥满量守卫）、0x10848c/0x108490（remain 计算）。
    R(F_ITEMSYSTEM_DIVIDE_VMA, 0x60, 0xb9401260, 0xaa1303e0, 0x6c, 0x9400e031, F_GET_CUMULATE_COUNT_VMA),
    R(F_ITEMSYSTEM_DIVIDE_VMA, 0x94, 0x2a1603e0, 0xaa1303e0, 0x98, 0x9400e026, F_GET_CUMULATE_COUNT_VMA),
};
#undef R
const size_t g_stack_getter_redirect_patch_count =
    sizeof(g_stack_getter_redirect_patches) / sizeof(g_stack_getter_redirect_patches[0]);

uintptr_t redirect_func_addr(const S2ReadRedirectEntry& e) {
    return g_base == 0 ? 0 : g_base + fn_resolve(e.func_macro, e.func_vma);
}

uintptr_t redirect_target_addr(const S2ReadRedirectEntry& e) {
    return g_base == 0 ? 0 : g_base + fn_resolve(e.target_macro, e.target_vma);
}

// 同 .so 内 bl 编码：imm26 = (target - bl_addr) >> 2；超出 ±128MB 或非 4 字节对齐拒绝。
bool encode_bl(uintptr_t bl_addr, uintptr_t target, uint32_t* out) {
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(bl_addr);
    if ((delta & 0x3) != 0 || delta <= -0x08000000LL || delta >= 0x08000000LL) return false;
    *out = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03FFFFFFu);
    return true;
}

bool apply_s2_getter_redirects() {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    for (size_t i = 0; i < g_stack_getter_redirect_patch_count; ++i) {
        const S2ReadRedirectEntry& e = g_stack_getter_redirect_patches[i];
        const uintptr_t func = redirect_func_addr(e);
        if (func == 0) return false;
        const uintptr_t arg_addr = func + e.arg_offset;
        const uintptr_t call_addr = func + e.call_offset;
        uint32_t replacement = 0;
        if (!encode_bl(call_addr, redirect_target_addr(e), &replacement)) {
            QOL_LOG_ERROR(QolDomain::kPlatform,
                                "redirect encode failed %s+0x%x -> %s", e.func_macro,
                                e.call_offset, e.target_macro);
            return false;
        }
        const uint32_t cur_arg = *reinterpret_cast<uint32_t*>(arg_addr);
        const uint32_t cur_call = *reinterpret_cast<uint32_t*>(call_addr);
        if (cur_arg == e.arg_replacement && cur_call == replacement) continue;  // 幂等
        if (cur_arg != e.arg_orig || cur_call != e.call_orig) {
            QOL_LOG_ERROR(QolDomain::kPlatform,
                                "redirect mismatch %s arg=0x%08x call=0x%08x", e.func_macro,
                                cur_arg, cur_call);
            return false;
        }
        if (!write_insn(arg_addr, e.arg_replacement) || !write_insn(call_addr, replacement)) {
            return false;
        }
        QOL_LOG_INFO(QolDomain::kPlatform,
                            "redirect %s+0x%x 0x%08x->0x%08x; +0x%x 0x%08x->0x%08x (%s)",
                            e.func_macro, e.arg_offset, e.arg_orig, e.arg_replacement,
                            e.call_offset, e.call_orig, replacement, e.target_macro);
    }
    return true;
}

bool revert_s2_getter_redirects() {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    for (size_t i = g_stack_getter_redirect_patch_count; i > 0; --i) {
        const S2ReadRedirectEntry& e = g_stack_getter_redirect_patches[i - 1];
        const uintptr_t func = redirect_func_addr(e);
        if (func == 0) return false;
        write_insn(func + e.arg_offset, e.arg_orig);
        write_insn(func + e.call_offset, e.call_orig);
    }
    return true;
}

bool apply_fixed_stack_layout() {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    if (g_stack_layout_patch_set.applied()) return true;
    bool ok = g_stack_layout_patch_set.apply();
    if (ok) ok = apply_s2_getter_redirects();
    if (ok) {
        QOL_LOG_INFO(QolDomain::kPlatform, "stack canonical layout %s", "applied");
    } else {
        QOL_LOG_ERROR(QolDomain::kPlatform, "stack canonical layout %s", "failed");
    }
    return ok;
}

// monster 版（大黄猫改版）物品数量上界兼容：
// monster mod 在 SAVE_LoadFile 内用自写包装器（注入段）逐件校验物品记录，
// 对 I_COUNT（item+0x10）最高字节要求 ((b-2)>>1) <= 0x62（原版 7 位编码 ≤99）。
// 模块 S2 扩展上限编码（a=bits22-24）会改变该最高字节，使扩展数量物品被拒，
// 进而 SAVE_LoadFile 返回 0 → SAVE_Load 提前返回 → 角色未加载 → EnterPlay 崩溃。
// 运行时按特征字节定位该上界判定并放宽：cmp w8,#0x62 (0x7101891F) → cmp w8,#0x7F (0x7101FD1F)。
// 仅在 libgame.so 可执行映射内匹配（数据区同字节不命中），非 monster 版无操作。
bool apply_monster_item_count_compat() {
    std::lock_guard<std::recursive_mutex> lock(g_patch_mtx);
    if (g_base == 0) return false;
    static const uint8_t kSig[8] = {0x1F, 0x89, 0x01, 0x71, 0x68, 0x00, 0x00, 0x54};
    constexpr uint32_t kOrig = 0x7101891Fu;  // cmp w8, #0x62
    constexpr uint32_t kRepl = 0x7101FD1Fu;  // cmp w8, #0x7F
    int hits = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (f == nullptr) return false;
    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        char path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perms, path) < 4) continue;
        if (strstr(path, "libgame.so") == nullptr || perms[2] != 'x') continue;
        for (uintptr_t p = lo; p + sizeof(kSig) <= hi; ++p) {
            if (memcmp(reinterpret_cast<const void*>(p), kSig, sizeof(kSig)) != 0) continue;
            uint32_t* ip = reinterpret_cast<uint32_t*>(p);
            if (*ip == kRepl) {
                ++hits;
                continue;
            }
            if (*ip != kOrig) continue;
            if (write_insn(p, kRepl)) {
                ++hits;
                QOL_LOG_INFO(QolDomain::kPlatform,
                                    "monster item-count compat: 0x%lx cmp w8,#0x62 -> #0x7F",
                                    static_cast<unsigned long>(p));
            }
        }
    }
    fclose(f);
    if (hits == 0) {
        QOL_LOG_INFO(QolDomain::kPlatform,
                            "monster item-count compat: no target (non-monster build)");
    }
    return true;
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
