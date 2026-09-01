#include "game_patch.h"

#include "game_access.h"
#include "game_inventory.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ops_common.h"
#include "game_ptr_hook.h"
#include "game_ui_virtbag.h"

#include <android/log.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#define PATCH_TAG "Inotia4Export"
#define MOVE_TAG "Inotia4Move"
#define MOVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, MOVE_TAG, __VA_ARGS__)

namespace {

std::recursive_mutex g_patch_mtx;
std::atomic<bool> g_stack_enabled{false};

// 合成器批量宝石合成 + 自定义 UI 按钮（v0.5.18）注入状态
std::mutex g_craft_mtx;            // 注入/还原互斥（串行化 enable/disable）
bool g_craft_injected = false;     // 是否已注入
void* g_craft_orig = nullptr;      // 原宝石按钮 ControlObject*（还原槽指针用）
PtrHook g_craft_exec_hook;         // 原宝石按钮 ExecuteProc 的函数指针 hook（覆盖为批量合成）
void* g_craft_mmap = nullptr;      // mmap 区域（ControlObject + 按钮数据）
size_t g_craft_mmap_len = 0;       // mmap 长度
std::atomic<bool> g_craft_want{false};           // 是否期望注入（配置开关）
std::atomic<bool> g_craft_thread_started{false};
PtrHook g_move_merge_hook;
std::mutex g_move_merge_mtx;

uintptr_t patch_addr(const PatchEntry& e) {
    if (g_base == 0) return 0;
    return g_base + fn_resolve(e.func_macro, e.func_vma) + e.func_offset;
}

bool write_insn(uintptr_t addr, uint32_t value) {
    const uintptr_t page = addr & ~uintptr_t{0xFFF};
    const size_t plen = (addr + sizeof(uint32_t) - page + 0xFFF) & ~size_t{0xFFF};
    if (mprotect(reinterpret_cast<void*>(page), plen, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, PATCH_TAG, "patch: mprotect(0x%lx) failed errno=%d", static_cast<unsigned long>(page), errno);
        return false;
    }
    *reinterpret_cast<uint32_t*>(addr) = value;
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr + sizeof(uint32_t)));
    return true;
}

void write_back(const PatchEntry* entries, size_t upto) {
    for (size_t i = 0; i < upto; ++i) {
        uintptr_t addr = patch_addr(entries[i]);
        if (addr == 0) continue;
        write_insn(addr, entries[i].orig);
    }
}

}  // namespace

// 固定 canonical 布局 patch 点（反汇编逐字节确认）。
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
    // 99→999 clamp（cmp/mov #0x63→#0x3E7，#0x62→#0x3E6）
    P(F_INVEN_MOVE_ITEM_VMA, 0x150, 0x71018c1f, 0x710f9c1f),
    P(F_INVEN_MOVE_ITEM_VMA, 0x158, 0x52800c77, 0x52807cf7),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xdc, 0x71018c7f, 0x710f9c7f),
    P(F_INVEN_SAVE_ITEM_DIRECT_VMA, 0xf0, 0x71018e9f, 0x710f9e9f),
    P(F_INVEN_SAVE_ITEM_DATA_VMA, 0x7c, 0x71018a9f, 0x710f9a9f),
    P(F_INVEN_SAVE_ITEM_DATA_VMA, 0x8c, 0x52800c63, 0x52807ce3),
    P(F_INVEN_FIND_SAVE_SLOT_VMA, 0x238, 0x71018c1f, 0x710f9c1f),
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

uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const uint64_t observation_token = virtual_bag_observe_item_proc_pre(control, event, x2, param);
    const auto finish = [observation_token, control, event, x2, param](uint64_t result) {
        virtual_bag_observe_item_proc_post(observation_token, control, event, x2, param, result);
        return result;
    };
    // G-8：投影拖动 drop 到扩展格（0x02）→ 路由 ext→ext 事务，抑制原版
    // INVEN_MoveItem（借出对象不在 INVEN，原版移动不可靠）。
    if (event == 0x2 && param != nullptr && param != nullptr) {
        void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param, ctrl_src);
        if (virtual_bag_projection_drop_to_slot(control, ctrl_src)) {
            return finish(1);
        }
    }
    // G-7：扩展视图内只拦截 drop（0x04，借出对象不得拖入原版袋——SaveItemOnEmpty
    // 门禁为第二层）；详情(0x80)/选中(0x01,0x02)/其余事件放行原版控件链。
    if (virtual_bag_original_item_input_blocked() && event == 0x04) {
        if (param != nullptr) {
            void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
            virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param,
                                                 ctrl_src);
        }
        // G-9 防护：扩展标签挂物品列表 root（索引 16+），GetItemSlotIndex 对其返回
        // 越界值 16——drop 落到标签控件时直接拒绝，防越界写。
        if (fn_ui_equip_get_item_slot_index != nullptr &&
            fn_ui_equip_get_item_slot_index(control) >= 16) {
            MOVE_LOG("drop suppressed: dst is extension tab control=%p", control);
            return finish(0);
        }
        MOVE_LOG("move_merge: drop suppressed in extension view control=%p",
                 control);
        return finish(0);
    }
    if (event == 4 && param != nullptr && fn_control_object_get_data != nullptr &&
        fn_ui_equip_is_apply_stuff != nullptr && fn_ui_equip_get_item_slot_index != nullptr &&
        fn_get_cumulate_count != nullptr && fn_get_bit != nullptr && fn_inven_move_item != nullptr) {
        void* ctrl_src = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
        virtual_bag_observe_item_proc_source(observation_token, control, event, x2, param, ctrl_src);
        void* src_data = ctrl_src != nullptr ? fn_control_object_get_data(ctrl_src) : nullptr;
        void* dst_data = fn_control_object_get_data(control);
        void* item_a = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
        void* item_b = dst_data != nullptr ? *reinterpret_cast<void**>(dst_data) : nullptr;
        if (item_a != nullptr && item_b != nullptr &&
            !fn_ui_equip_is_apply_stuff(item_b, item_a) && !item_is_equip(item_a)) {
            const uint16_t flags_a =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item_a) + I_TYPE);
            const uint16_t flags_b =
                *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item_b) + I_TYPE);
            if (fn_get_bit(flags_a, 15, 6) == fn_get_bit(flags_b, 15, 6)) {
                const int bag = *reinterpret_cast<uint8_t*>(g_base + G_UIEQUIP_CUR_BAG_VMA);
                const int dst_slot = fn_ui_equip_get_item_slot_index(control);
                const int src_slot = ctrl_src != nullptr ? fn_ui_equip_get_item_slot_index(ctrl_src) : -1;
                const int count = fn_get_cumulate_count(item_a);
                const int result = fn_inven_move_item(item_a, count, bag, dst_slot);
                if (!result) return finish(0);
                if (virtual_bag_module_view_installed()) {
                    virtual_bag_sync_projected_slot(bag, src_slot);
                    virtual_bag_sync_projected_slot(bag, dst_slot);
                }
                if (fn_ui_equip_refresh_item_area != nullptr) fn_ui_equip_refresh_item_area();
                void* panel_ctrl = *reinterpret_cast<void**>(g_base + G_UIEQUIP_PANEL_CTRL_VMA);
                if (fn_touch_handle_set_cursor != nullptr && panel_ctrl != nullptr) {
                    fn_touch_handle_set_cursor(panel_ctrl, nullptr);
                }
                MOVE_LOG("move_merge: merged bag=%d src=%d dst=%d count=%d", bag, src_slot,
                         dst_slot, count);
                return finish(0);
            }
        }
    }
    if (fn_ui_equip_inven_item_control_event_proc == nullptr) return finish(0);
    const uint64_t result = fn_ui_equip_inven_item_control_event_proc(control, event, x2, param);
    if (virtual_bag_module_view_installed()) {
        virtual_bag_sync_projected_bag();
    }
    return finish(result);
}

bool set_move_merge_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_move_merge_mtx);
    if (g_base == 0) return false;
    void** slot = reinterpret_cast<void**>(g_base + G_UIEQUIP_INVEN_ITEM_PROC_GOT_VMA);
    if (slot == nullptr || *slot == nullptr) {
        MOVE_LOG("move_merge: invalid proc_got=%p value=%p", slot, slot == nullptr ? nullptr : *slot);
        return false;
    }
    const void* replacement = reinterpret_cast<void*>(&ui_equip_inven_item_proc_wrapper);
    if (enabled) {
        if (g_move_merge_hook.orig != nullptr) return true;
        const void* expected = reinterpret_cast<void*>(fn_ui_equip_inven_item_control_event_proc);
        if (expected == nullptr || *slot != expected) {
            MOVE_LOG("move_merge: unexpected proc_got=%p value=%p expected=%p", slot, *slot, expected);
            return false;
        }
    } else if (g_move_merge_hook.orig != nullptr && *slot != replacement) {
        MOVE_LOG("move_merge: hook slot changed externally proc_got=%p value=%p", slot, *slot);
        return false;
    }
    const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~uintptr_t{0xFFF};
    if (mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE) != 0) {
        MOVE_LOG("move_merge: mprotect failed errno=%d", errno);
        return false;
    }
    if (enabled) {
        bool ok = g_move_merge_hook.install_typed(slot, &ui_equip_inven_item_proc_wrapper);
        if (ok) {
            MOVE_LOG("move_merge: enabled proc_got=%p orig=%p replacement=%p", slot, g_move_merge_hook.orig,
                     reinterpret_cast<void*>(&ui_equip_inven_item_proc_wrapper));
        }
        return ok;
    }
    if (g_move_merge_hook.orig == nullptr) return true;
    g_move_merge_hook.uninstall();
    MOVE_LOG("move_merge: disabled");
    return true;
}

bool move_merge_enabled() {
    std::lock_guard<std::mutex> lock(g_move_merge_mtx);
    return g_move_merge_hook.orig != nullptr;
}

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
// 失败（配方/背包满/金币不足）安全停止；完成后一次性扣费并静默存档。
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
        fn_minus_money == nullptr || fn_save == nullptr || fn_get_bit == nullptr ||
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
    if (made > 0) fn_save();
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
