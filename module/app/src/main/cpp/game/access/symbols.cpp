#include "game/access/symbols.h"

#include <cstring>

#include "game/access/qol_log_seam.h"
#include "game/symbols/game_symbols.h"
#include "game/symbols/symbol_resolver.h"
#include "game/access/game_access.h"

namespace qol::game {
namespace {

// 声明符号名 → 常量唯一来源（game_symbols.h）。运行时经 fn_resolve 优先按 .dynsym
// 符号名解析，未命中回退 VMA。
struct HookEntrySymbol {
    const char* name;
    uintptr_t vma;
};

constexpr HookEntrySymbol kHookEntrySymbols[] = {
    {"F_CHAR_ADD_DAMAGE_VMA", F_CHAR_ADD_DAMAGE_VMA},
    {"F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA", F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA},
    // 属性显示范围：详情渲染三点（MATH_GetRandom 截获 / 选项行改写 / 构造入口缓存）。
    {"F_MATH_GET_RANDOM_VMA", F_MATH_GET_RANDOM_VMA},
    {"F_UIDESC_ADD_OPTION_VMA", F_UIDESC_ADD_OPTION_VMA},
    {"F_UIDESC_MAKE_ITEM_VMA", F_UIDESC_MAKE_ITEM_VMA},
    // 文本层：全游戏唯一文本读取热点（共享资源，owner=text_layer）。
    {"F_MEMORYTEXT_GET_TEXT_VMA", F_MEMORYTEXT_GET_TEXT_VMA},
    // 角色面板绘制窗口（owner=text_layer，D1-b4 声明式化）：作用域标记 wrapper 的挂载点。
    {"F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA", F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA},
    // 原生选择框（owner=native_choice，D1-b1 声明式化）：标题改写 wrapper 的挂载点。
    {"F_UI_CHOICE_INIT_VMA", F_UI_CHOICE_INIT_VMA},
    // 面板插件（D1-b0 前置）：UI 面板注入所复用的 PopupState 死条目/可开面板 enter 回调，
    // 供 hooks::popup_state_resolve_symbol 按名解析（禁止面板出现裸 VMA）。
    {"F_PANEL_UNK1_ENTER", F_PANEL_UNK1_ENTER},
    {"F_PANEL_UNK2_ENTER", F_PANEL_UNK2_ENTER},
    {"F_PANEL_UNK3_ENTER", F_PANEL_UNK3_ENTER},
    {"F_PANEL_INVENTORY_ENTER", F_PANEL_INVENTORY_ENTER},
    // 扩展背包（D2a-3a）：两个入口 hook 目标 + 三个 PopupState 面板 enter（部分 wrap 定位）。
    {"F_SAVE_SAVE_INVENTORY_VMA", F_SAVE_SAVE_INVENTORY_VMA},
    {"extension.save_inventory", F_SAVE_SAVE_INVENTORY_VMA},
    {"F_UISTORE_SELL_ITEM_VMA", F_UISTORE_SELL_ITEM_VMA},
    {"extension.store_sell_item", F_UISTORE_SELL_ITEM_VMA},
    {"F_UISTORE_BUTTON_SELL_EXE_VMA", F_UISTORE_BUTTON_SELL_EXE_VMA},
    {"extension.store_button_sell_exe", F_UISTORE_BUTTON_SELL_EXE_VMA},
    {"F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA", F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA},
    {"extension.store_draw_inven_bag_group", F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA},
    {"F_PANEL_SAVE_SLOT_ENTER", F_PANEL_SAVE_SLOT_ENTER},
    {"F_PANEL_SHOP_ENTER", F_PANEL_SHOP_ENTER},
    {"F_PANEL_CRAFT_ENTER", F_PANEL_CRAFT_ENTER},
    // 调试端点 nativeSettingsUiOpenOption：主菜单环境设置 state 的按名定位。
    {"F_PANEL_OPTIONS_ENTER", F_PANEL_OPTIONS_ENTER},
    // 特殊装备解锁：+7 bit4 的两个判定入口（入口 hook，owner=special_equip）。
    {"F_CAN_EQUIP_VMA", F_CAN_EQUIP_VMA},
    {"F_CAN_UNEQUIP_VMA", F_CAN_UNEQUIP_VMA},
    // 库存行为域入口 hook（owner=inventory_hooks，B2a-1 声明式化）：24 条 wrapper 的
    // 挂载点；handler 在 plugins/inventory_hooks/native_inventory_hook.cpp 取址。
    {"F_FIND_ITEM_VMA", F_FIND_ITEM_VMA},
    {"F_INVEN_HAVE_ITEM_VMA", F_INVEN_HAVE_ITEM_VMA},
    {"F_INVEN_GET_ITEM_COUNT_VMA", F_INVEN_GET_ITEM_COUNT_VMA},
    {"F_GET_CUMULATE_COUNT_VMA", F_GET_CUMULATE_COUNT_VMA},
    {"F_CONSUME_ITEM_VMA", F_CONSUME_ITEM_VMA},
    {"F_REMOVE_ITEM_VMA", F_REMOVE_ITEM_VMA},
    {"F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA", F_EQUIP_ITEM_FROM_INVEN_TO_SLOT_VMA},
    {"F_PUT_JEWEL_VMA", F_PUT_JEWEL_VMA},
    {"F_INVEN_IS_HAVING_EMPTY_SLOT_VMA", F_INVEN_IS_HAVING_EMPTY_SLOT_VMA},
    {"F_UNEQUIP_VMA", F_UNEQUIP_VMA},
    {"F_UIEQUIP_BUTTON_EQUIP_EXE_VMA", F_UIEQUIP_BUTTON_EQUIP_EXE_VMA},
    {"F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA", F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA},
    {"F_UIEQUIP_BUTTON_DESTROY_EXE_VMA", F_UIEQUIP_BUTTON_DESTROY_EXE_VMA},
    {"F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA", F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA},
    {"F_UIEQUIP_OK_DESTROY_ITEM_VMA", F_UIEQUIP_OK_DESTROY_ITEM_VMA},
    {"F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA", F_UIEQUIP_BUTTON_USE_MERCENARY_SEAL_EXE_VMA},
    {"F_INVEN_SAVE_ITEM_VMA", F_INVEN_SAVE_ITEM_VMA},
    {"F_INVEN_MOVE_ITEM_VMA", F_INVEN_MOVE_ITEM_VMA},
    {"F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA", F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA},
    {"F_UIEQUIP_REFRESH_ITEM_AREA_VMA", F_UIEQUIP_REFRESH_ITEM_AREA_VMA},
    {"F_INVEN_SAVE_ITEM_DIRECT_VMA", F_INVEN_SAVE_ITEM_DIRECT_VMA},
    {"F_ITEMSYSTEM_DIVIDE_VMA", F_ITEMSYSTEM_DIVIDE_VMA},
    {"F_MAKE_ITEM_VMA", F_MAKE_ITEM_VMA},
    {"F_INVEN_REMOVE_ITEM_DATA_VMA", F_INVEN_REMOVE_ITEM_DATA_VMA},
    // 合成器界面层入口 hook（owner=craft_ui，C2 Step 3 声明式化）：12 条 wrapper 的挂载点；
    // handler 在 plugins/custom_recipe/ui/runtime.cpp 取址。
    {"F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA", F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA},
    {"F_UIMIX_BUTTON_MIXING_EXE_VMA", F_UIMIX_BUTTON_MIXING_EXE_VMA},
    {"F_MAKE_MIX_VMA", F_MAKE_MIX_VMA},
    {"F_UIMIX_BUTTON_MENU_LIST_EXE_VMA", F_UIMIX_BUTTON_MENU_LIST_EXE_VMA},
    {"F_UIMIX_BUTTON_RECIPE_EXE_VMA", F_UIMIX_BUTTON_RECIPE_EXE_VMA},
    {"F_XTEXTCTRL_SET_TEXT_CONTROL_VMA", F_XTEXTCTRL_SET_TEXT_CONTROL_VMA},
    {"F_CONTROL_ITEM_DRAW_VMA", F_CONTROL_ITEM_DRAW_VMA},
    {"F_ITEM_DRAW_PORTING_VMA", F_ITEM_DRAW_PORTING_VMA},
    {"F_UIMIX_BUTTON_RECIPE_DRAW_VMA", F_UIMIX_BUTTON_RECIPE_DRAW_VMA},
    {"F_UIMIX_DRAW_VMA", F_UIMIX_DRAW_VMA},
    {"F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA", F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA},
    {"F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA", F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA},
};

// 解析可安装 hook 的入口地址。
// monster 全系把 CHAR_AddDamage 入口改写成无条件 b 跳板（v20 `b 0x14e280`；v23-v27 `b 0x7450b8`，
// 落于第二可执行段）。跳板入口先 `stp x0..x8` 保存 x0-x7 再修改寄存器，故跳板目标处 ABI 与函数
// 入口完全一致，在目标处 hook 与原位等价；而 inline hook 若需重定位一条 26 位 b，跨 ±128MB 的
// 偏移会失效。因此这里主动跟随一次，目标必须 4 字节对齐且落在已加载的可执行映射内。
uintptr_t hookable_entry(uintptr_t symbol_entry, bool* out_followed) {
    *out_followed = false;
    if (symbol_entry == 0 || !game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'r')) {
        return symbol_entry;
    }
    const uint32_t word = *reinterpret_cast<const uint32_t*>(symbol_entry);
    constexpr uint32_t k_branch_mask = 0xFC000000u;   // b/bl/ret 等 opcode 掩码
    constexpr uint32_t k_b_opcode = 0x14000000u;      // B（无条件、不写 x30）
    if ((word & k_branch_mask) != k_b_opcode) {
        // 非跳板：入口必须落在可执行映射内，否则按未解析处理（fail-closed）。
        // 旧自研安装引擎曾对每个目标做同一判定（引擎已在 B2a-1 删除）；声明式安装改为在
        // 解析层收口，避免把不可执行地址交给 hook 原语。
        return game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'x')
                   ? symbol_entry
                   : 0;
    }

    int64_t offset = static_cast<int64_t>(word & 0x03FFFFFFu);
    if ((offset & 0x02000000) != 0) offset -= 0x04000000;  // 26 位有符号立即数
    const uintptr_t target = symbol_entry + static_cast<uintptr_t>(offset * 4);
    if ((target & 0x3u) != 0) return symbol_entry;
    if (!game_memory_accessible(reinterpret_cast<void*>(target), 8, 'x')) return symbol_entry;
    *out_followed = true;
    return target;
}

}  // namespace

uintptr_t resolve_hook_entry(const char* symbol_name, bool* out_followed) {
    // hookable_entry 始终解引用 out_followed；调用方允许传 nullptr 时用局部量承接。
    bool followed_local = false;
    bool* followed = out_followed != nullptr ? out_followed : &followed_local;
    if (symbol_name == nullptr || g_base == 0) {
        *followed = false;
        return 0;
    }
    const char* export_name = elf_export_name(symbol_name);
    if (export_name != nullptr) {
        *followed = false;
        if (export_name[0] == '\0') return 0;
        const uintptr_t offset = g_resolver.resolveFunctionByName(export_name);
        if (offset == 0) return 0;
        const uintptr_t address = g_base + offset;
        if ((address & 0x3u) != 0 ||
            !game_memory_accessible(reinterpret_cast<void*>(address), 4, 'x')) return 0;
        // 临时诊断：入口解析结果与入口指令字（定位 UIPlay_CallMapName 入口不生效）。
        const bool word_readable =
            game_memory_accessible(reinterpret_cast<void*>(address), 4, 'r');
        const uint32_t word =
            word_readable ? *reinterpret_cast<const uint32_t*>(address) : 0;
        QOL_LOG_SEAM_INFO(kQolLogDomainPlatform,
                          "hook entry resolved export=%s addr=%p word=0x%08x", export_name,
                          reinterpret_cast<void*>(address), word);
        return hookable_entry(address, followed);
    }    for (const HookEntrySymbol& entry : kHookEntrySymbols) {
        if (std::strcmp(entry.name, symbol_name) != 0) continue;
        const uintptr_t symbol_entry = g_base + fn_resolve(entry.name, entry.vma);
        // 临时诊断：同 export 路径。
        const bool word_readable =
            game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'r');
        const uint32_t word =
            word_readable ? *reinterpret_cast<const uint32_t*>(symbol_entry) : 0;
        QOL_LOG_SEAM_INFO(kQolLogDomainPlatform,
                          "hook entry resolved symbol=%s addr=%p word=0x%08x", symbol_name,
                          reinterpret_cast<void*>(symbol_entry), word);
        return hookable_entry(symbol_entry, followed);
    }
    *followed = false;
    return 0;
}

}  // namespace qol::game
