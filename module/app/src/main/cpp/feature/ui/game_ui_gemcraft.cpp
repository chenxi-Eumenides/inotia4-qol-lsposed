#include "feature/ui/game_ui_gemcraft.h"

#include "core/native/call_patch.h"
#include "core/native/qol_log.h"
#include "feature/custom_recipe/game_ui_custom_recipe.h"
#include "feature/gemcraft/gemcraft_rules.h"
#include "game_access.h"
#include "game_ptr_hook.h"
#include "game_symbols.h"

#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace {

constexpr int kStuffSlotCount = 3;
// UIMix_StartMix + 0x258 处原 `bl UIMix_ResetStuffItemControl` 指令字（0xc0ac8，反汇编核对）。
constexpr uint32_t kCraftResetStuffCallWord = 0x97fffdde;
constexpr size_t kPageSize = 0x1000;

std::atomic<bool> g_gemcraft_enabled{false};
std::atomic<bool> g_craft_anchor_installed{false};
// 四个按钮 ExecuteProc 初值 GOT 槽的指针覆盖（orig 由 install 捕获，供 call_orig）。
PtrHook g_desc_hook;
PtrHook g_menu_hook;
PtrHook g_recipe_hook;
PtrHook g_craft_hook;

void* uimix_slot(size_t offset) {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + offset);
}

// 原版放料函数地址只解析一次，避免 fn_resolve 反复写入 g_symbol_report。
uintptr_t original_place_execute_addr() {
    static uintptr_t cached = 0;
    if (cached == 0 && g_base != 0) {
        cached = g_base + fn_resolve("F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA",
                                     F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA);
    }
    return cached;
}

// 读取 [g_uimix+0xc8] 材料组前 3 个子控件的已填状态；返回已填数量。
int read_filled_slots(bool* filled) {
    int count = 0;
    void* group = uimix_slot(UIMIX_SLOT_STUFF_GROUP);
    for (int index = 0; index < kStuffSlotCount; ++index) {
        bool has_item = false;
        if (group != nullptr && fn_control_object_get_child != nullptr &&
            fn_control_object_get_data != nullptr) {
            void* child = fn_control_object_get_child(group, static_cast<uint32_t>(index));
            if (child != nullptr) {
                void* data = fn_control_object_get_data(child);
                if (data != nullptr && *reinterpret_cast<void**>(data) != nullptr) {
                    has_item = true;
                }
            }
        }
        filled[index] = has_item;
        if (has_item) ++count;
    }
    return count;
}

// 读取 [g_uimix+0xc8] 前 3 格的已放宝石 category（空格置 -1）；返回已填数量。
int read_slot_categories(int categories[kStuffSlotCount]) {
    int count = 0;
    void* group = uimix_slot(UIMIX_SLOT_STUFF_GROUP);
    for (int index = 0; index < kStuffSlotCount; ++index) {
        categories[index] = -1;
        void* item = nullptr;
        if (group != nullptr && fn_control_object_get_child != nullptr &&
            fn_control_object_get_data != nullptr) {
            void* child = fn_control_object_get_child(group, static_cast<uint32_t>(index));
            if (child != nullptr) {
                void* data = fn_control_object_get_data(child);
                if (data != nullptr) item = *reinterpret_cast<void**>(data);
            }
        }
        if (item == nullptr || fn_get_bit == nullptr) {
            if (item != nullptr) ++count;  // 已放但读不到类别：按已填计，类别 -1 走错误提示
            continue;
        }
        const uint16_t type = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
        categories[index] = fn_get_bit(static_cast<int>(type), 15, 6);
        ++count;
    }
    return count;
}

void write_selected_slot(int index) {
    if (g_uimix == nullptr) return;
    *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_SELECTED_STUFF) =
        static_cast<int64_t>(index);
}

void select_first_empty_or_none(const bool* filled) {
    write_selected_slot(gemcraft::first_empty_slot(filled, kStuffSlotCount));
}

// 进入宝石合成视图（type 1 且 state != 0）时选中第一个空格；否则不动。
void select_on_enter_or_none() {
    if (!g_gemcraft_enabled.load(std::memory_order_acquire)) return;
    if (g_uimix == nullptr || fn_uimix_get_type == nullptr) return;
    if (fn_uimix_get_type() != 1) return;
    const uint8_t state =
        *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STATE);
    if (state == 0) return;
    bool filled[kStuffSlotCount] = {false, false, false};
    read_filled_slots(filled);
    select_first_empty_or_none(filled);
}

// 背包当前选中物品指针（[g_uimix+0xd8] 组 cursor → GetData → *data）。
void* selected_inven_item() {
    void* group = uimix_slot(UIMIX_SLOT_ITEM_GROUP);
    if (group == nullptr || fn_control_object_get_cursor == nullptr ||
        fn_control_object_get_data == nullptr) {
        return nullptr;
    }
    void* cursor = fn_control_object_get_cursor(group);
    if (cursor == nullptr) return nullptr;
    void* data = fn_control_object_get_data(cursor);
    if (data == nullptr) return nullptr;
    return *reinterpret_cast<void**>(data);
}

// 放料链终点 wrapper：启用且 type==1 时，把 stuffList[0] 的 32 位档位临时改成当前
// 选中宝石 category 再转调原函数，使原版 category 比较必然通过；其余校验（非宝石弹
// 0x61、重复弹 0x63、未选中直接返回、UIDesc_SetOff 等）全部保留。调用后比较前后已填
// 格数，增加则选中下一空格（全满 -1）——即原函数 0xc2538 SetItem 之后的终点。
void gemcraft_place_execute(void* ctrl) {
    if (fn_uimix_button_inven_item_select_exe == nullptr) return;
    if (!g_gemcraft_enabled.load(std::memory_order_acquire) || g_uimix == nullptr ||
        fn_uimix_get_type == nullptr || fn_uimix_get_type() != 1) {
        fn_uimix_button_inven_item_select_exe(ctrl);
        return;
    }
    bool before_filled[kStuffSlotCount] = {false, false, false};
    const int before_count = read_filled_slots(before_filled);

    uint32_t* stuff_first = nullptr;
    uint32_t saved_category = 0;
    void* item = selected_inven_item();
    if (item != nullptr && fn_get_bit != nullptr && fn_is_jewel != nullptr) {
        const uint16_t type = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
        const int category = fn_get_bit(static_cast<int>(type), 15, 6);
        if (fn_is_jewel(category)) {
            void* stuff = uimix_slot(UIMIX_SLOT_STUFF_LIST);
            if (stuff != nullptr) {
                stuff_first = reinterpret_cast<uint32_t*>(stuff);
                saved_category = *stuff_first;
                *stuff_first = static_cast<uint32_t>(category);
            }
        }
    }

    fn_uimix_button_inven_item_select_exe(ctrl);

    if (stuff_first != nullptr) *stuff_first = saved_category;

    bool after_filled[kStuffSlotCount] = {false, false, false};
    const int after_count = read_filled_slots(after_filled);
    if (after_count > before_count) select_first_empty_or_none(after_filled);
}

// 进入视图链终点 wrapper：菜单/配方按钮原函数尾部写 [+0x128]=-1，调原函数后再选中第一空格。
void gemcraft_menu_execute(void* ctrl) {
    if (g_menu_hook.orig == nullptr) return;
    g_menu_hook.call_orig<void>(ctrl);
    select_on_enter_or_none();
}

void gemcraft_recipe_execute(void* ctrl) {
    if (g_recipe_hook.orig == nullptr) return;
    g_recipe_hook.call_orig<void>(ctrl);
    select_on_enter_or_none();
}

// 合成链终点 gate：BL patch UIMix_StartMix 内 bl UIMix_ResetStuffItemControl（清空填入格）。
void gemcraft_craft_reset_gate() {
    if (fn_uimix_reset_stuff_item_control == nullptr) return;
    fn_uimix_reset_stuff_item_control();
    select_on_enter_or_none();
}

// 错误提示：复用游戏原生提示框 + 既有文本项 0x62（原版「宝石与配方材料档位不符」，
// 语义与「三格必须同档/混沌不可合成」一致；用户裁决 D3）。
void gemcraft_show_level_error() {
    if (fn_popup_create_ok_from_textdata == nullptr) {
        QOL_LOG_ERROR(QolDomain::kGemCraft, "popup symbol not resolved");
        return;
    }
    fn_popup_create_ok_from_textdata(0x62, 0, 0, 0);
}

// 合成按钮 ExecuteProc wrapper（阶段2）：填满且同档 28..31 → 前置改写 mixType 并重算费用；
// 填满但混档/混沌/非宝石段 → 弹错误提示、不进入合成（不弹确认、不扣料、不产物）；
// 未填满或非宝石视图/功能关闭 → 转调原函数（沿用原版行为）。
void gemcraft_craft_execute(void* ctrl) {
    if (g_craft_hook.orig == nullptr) return;
    // 3 格自定义配方模式让路（custom-craft-recipe §4.12）：该模式的匹配/产出由
    // custom_recipe 在同一函数的 inline hook 里自实现。若继续走下面的「填满但混档」分支，
    // 会先弹 98「该宝石不能进行合成。」并 return，使它的合成 hook 永远轮不到
    //（真机实证：2 恢复药水（小）+1 低级武器强化卷轴 应命中 {5,5,16}→顶级宝石，却弹 98）。
    // 故此处直接转调原函数 —— 原函数入口即 custom_recipe 的 inline hook。
    if (custom_recipe_three_slot_mode_active()) {
        g_craft_hook.call_orig<void>(ctrl);
        return;
    }
    if (!g_gemcraft_enabled.load(std::memory_order_acquire) || g_uimix == nullptr ||
        fn_uimix_get_type == nullptr || fn_uimix_get_type() != 1) {
        g_craft_hook.call_orig<void>(ctrl);
        return;
    }
    int categories[kStuffSlotCount] = {-1, -1, -1};
    if (read_slot_categories(categories) < kStuffSlotCount) {
        g_craft_hook.call_orig<void>(ctrl);  // 未填满：沿用原版
        return;
    }
    const int category = categories[0];
    if (category != categories[1] || category != categories[2] || category < 28 ||
        category > 31) {
        // 档位不一致 / 混沌(32) / 不在 28..31：原生提示后拦截，不进入合成。
        gemcraft_show_level_error();
        return;
    }
    const uint32_t mix_type = static_cast<uint32_t>(12 + (category - 28));
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_MIXTYPE) = mix_type;
    if (fn_uimix_init_mixing_state != nullptr) {
        fn_uimix_init_mixing_state();  // 依实际档位重算 stuffList 与费用 [+0xf8]
    }
    g_craft_hook.call_orig<void>(ctrl);
}

// 精确安装按钮 ExecuteProc：仅当仍为 expected 原函数时替换为 wrapper（幂等）。守卫照抄
// extension_bag_store.inc store_make_desc_gate：控件对象与 ExecuteProc 数据块可访问才读写。
void try_install_execute_wrapper(void* button, void* wrapper, uintptr_t expected) {
    if (button == nullptr || wrapper == nullptr || expected == 0 ||
        fn_control_object_get_data == nullptr) {
        return;
    }
    if (!game_memory_accessible(button, CO_DATA + sizeof(void*), 'r')) return;
    void* data = fn_control_object_get_data(button);
    if (data == nullptr ||
        !game_memory_accessible(data, CB_EXECUTE_PROC + sizeof(void*), 'w')) {
        return;
    }
    void** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(data) + CB_EXECUTE_PROC);
    if (!game_memory_accessible(slot, sizeof(void*), 'w')) return;
    if (*slot == reinterpret_cast<void*>(expected)) *slot = wrapper;
}

void try_install_place_wrapper(void* button) {
    try_install_execute_wrapper(button, reinterpret_cast<void*>(&gemcraft_place_execute),
                                original_place_execute_addr());
}

void try_install_menu_wrapper(void* button) {
    try_install_execute_wrapper(button, reinterpret_cast<void*>(&gemcraft_menu_execute),
                                reinterpret_cast<uintptr_t>(fn_uimix_button_menu_list_exe));
}

void try_install_recipe_wrapper(void* button) {
    try_install_execute_wrapper(button, reinterpret_cast<void*>(&gemcraft_recipe_execute),
                                reinterpret_cast<uintptr_t>(fn_uimix_button_recipe_exe));
}

// 合成按钮原 ExecuteProc 地址只解析一次。
uintptr_t original_craft_execute_addr() {
    static uintptr_t cached = 0;
    if (cached == 0 && g_base != 0) {
        cached = g_base + fn_resolve("F_UIMIX_BUTTON_MIXING_EXE_VMA",
                                     F_UIMIX_BUTTON_MIXING_EXE_VMA);
    }
    return cached;
}

void try_install_craft_wrapper(void* button) {
    try_install_execute_wrapper(button, reinterpret_cast<void*>(&gemcraft_craft_execute),
                                original_craft_execute_addr());
}

// GOT 槽一次性覆盖：新建按钮即从该槽读取 ExecuteProc（fail-closed：值与 expected 不符不改）。
// 参考 game_patch_move_merge.inc update_move_merge_hook_locked 的 mprotect + PtrHook 范式。
bool install_got_hook(uintptr_t got_vma, const char* name, PtrHook& hook,
                      void (*wrapper)(void*), uintptr_t expected) {
    (void)name;  // host 日志桩丢弃该参数，避免 -Wunused-parameter
    if (hook.installed()) return true;
    if (g_base == 0 || wrapper == nullptr || expected == 0) return false;
    void** slot = reinterpret_cast<void**>(g_base + got_vma);
    if (!game_memory_accessible(slot, sizeof(void*), 'r')) {
        QOL_LOG_ERROR(QolDomain::kGemCraft, "%s got not accessible slot=%p", name, slot);
        return false;
    }
    if (*slot != reinterpret_cast<void*>(expected)) {
        QOL_LOG_ERROR(QolDomain::kGemCraft,
                            "%s got unexpected slot=%p value=%p expected=%p", name, slot, *slot,
                            reinterpret_cast<void*>(expected));
        return false;
    }
    const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~(kPageSize - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE) != 0) {
        QOL_LOG_ERROR(QolDomain::kGemCraft, "%s mprotect failed errno=%d", name, errno);
        return false;
    }
    if (!hook.install_typed(slot, wrapper)) {
        QOL_LOG_ERROR(QolDomain::kGemCraft, "%s install failed slot=%p", name, slot);
        return false;
    }
    QOL_LOG_INFO(QolDomain::kGemCraft, "%s hook installed slot=%p orig=%p", name, slot,
                        hook.orig);
    return true;
}

// 覆盖「启用时面板/按钮已存在」：对当前 5 个菜单按钮、配方组子按钮、desc 按钮各补挂一次。
void install_current_panel_wrappers() {
    if (g_uimix == nullptr) return;
    try_install_place_wrapper(uimix_slot(UIMIX_SLOT_DESC_MENU));
    try_install_craft_wrapper(uimix_slot(UIMIX_SLOT_CRAFT_BUTTON));
    for (int index = 0; index < UIMIX_MENU_BUTTON_COUNT; ++index) {
        try_install_menu_wrapper(uimix_slot(UIMIX_SLOT_MENU_BUTTON_BASE +
                                            static_cast<size_t>(index) * sizeof(void*)));
    }
    void* recipe_group = uimix_slot(UIMIX_SLOT_RECIPE_GROUP);
    if (recipe_group != nullptr && fn_control_object_get_child != nullptr &&
        fn_ctrl_get_count != nullptr) {
        const uint32_t count = fn_ctrl_get_count(recipe_group);
        for (uint32_t index = 0; index < count; ++index) {
            try_install_recipe_wrapper(fn_control_object_get_child(recipe_group, index));
        }
    }
}

}  // namespace

bool set_gemcraft_enabled(bool enabled) {
    g_gemcraft_enabled.store(enabled, std::memory_order_release);
    if (!enabled) return true;
    if (!gemcraft_install_if_ready()) {
        QOL_LOG_WARN(QolDomain::kGemCraft,
                            "install deferred (bridge/base not ready)");
    }
    return true;
}

bool gemcraft_enabled() {
    return g_gemcraft_enabled.load(std::memory_order_acquire);
}

// 全部锚点都成功才返回 true；各自幂等、失败可重试。禁用时不回滚已覆盖指针，
// wrapper 一律由 g_gemcraft_enabled 门控 no-op（保持已选值/原版行为）。
bool gemcraft_install_if_ready() {
    if (!bridge_ready() || g_base == 0) return false;

    bool ok = true;
    if (!install_got_hook(G_UIMIX_DESC_EXE_GOT_VMA, "desc", g_desc_hook,
                          &gemcraft_place_execute,
                          reinterpret_cast<uintptr_t>(fn_uimix_button_inven_item_select_exe))) {
        ok = false;
    }
    if (!install_got_hook(G_UIMIX_MENU_EXE_GOT_VMA, "menu", g_menu_hook, &gemcraft_menu_execute,
                          reinterpret_cast<uintptr_t>(fn_uimix_button_menu_list_exe))) {
        ok = false;
    }
    if (!install_got_hook(G_UIMIX_RECIPE_EXE_GOT_VMA, "recipe", g_recipe_hook,
                          &gemcraft_recipe_execute,
                          reinterpret_cast<uintptr_t>(fn_uimix_button_recipe_exe))) {
        ok = false;
    }
    if (!install_got_hook(G_UIMIX_CRAFT_EXE_GOT_VMA, "craft", g_craft_hook,
                          &gemcraft_craft_execute, original_craft_execute_addr())) {
        ok = false;
    }

    if (!g_craft_anchor_installed.load(std::memory_order_acquire)) {
        const uintptr_t call_addr = g_base +
            fn_resolve("F_UIMIX_START_MIX_VMA", F_UIMIX_START_MIX_VMA) +
            F_UIMIX_START_MIX_RESET_STUFF_CALL_OFF;
        if (!call_patch_install_bl(call_addr, kCraftResetStuffCallWord,
                                   reinterpret_cast<void*>(&gemcraft_craft_reset_gate))) {
            QOL_LOG_ERROR(QolDomain::kGemCraft,
                                "craft anchor install failed call=%p expected=0x%08x",
                                reinterpret_cast<void*>(call_addr), kCraftResetStuffCallWord);
            ok = false;
        } else {
            g_craft_anchor_installed.store(true, std::memory_order_release);
            QOL_LOG_INFO(QolDomain::kGemCraft, "craft anchor installed call=%p",
                                reinterpret_cast<void*>(call_addr));
        }
    }

    if (!ok) return false;
    install_current_panel_wrappers();
    return true;
}
