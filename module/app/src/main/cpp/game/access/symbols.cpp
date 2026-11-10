#include "game/access/symbols.h"

#include <atomic>
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
    {"F_CHAR_ACT_DD_VMA", F_CHAR_ACT_DD_VMA},
    {"F_CHAR_GET_NORMAL_DAMAGE_VMA", F_CHAR_GET_NORMAL_DAMAGE_VMA},
    {"F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA", F_CHAR_GET_NORMAL_MAGIC_DAMAGE_VMA},
    {"F_CHAR_GET_CRITICAL_DAMAGE_VMA", F_CHAR_GET_CRITICAL_DAMAGE_VMA},
    {"F_CHAR_CALC_MAGICAL_DEFENSE_VMA", F_CHAR_CALC_MAGICAL_DEFENSE_VMA},
    {"F_CHAR_CALC_PHYSICAL_DEFENSE_VMA", F_CHAR_CALC_PHYSICAL_DEFENSE_VMA},
    {"F_CHAR_ADD_DAMAGE_VMA", F_CHAR_ADD_DAMAGE_VMA},
    {"F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA", F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA},
    // 经验/金币的统一增量入口（owner=world_tuning）。
    {"F_ADD_EXP_VMA", F_ADD_EXP_VMA},
    {"F_ADD_MONEY_VMA", F_ADD_MONEY_VMA},
    // 全向移动入口（owner=better_move）：方向触摸与游戏内按键接收器。
    // 未登记时 preflight 报 symbol unresolved，插件状态会变成 unsupported（设置页显示
    // “当前版本不支持”），不是变体限制。
    {"F_UIPLAY_BUTTON_DIRECTION_VMA", F_UIPLAY_BUTTON_DIRECTION_VMA},
    {"F_GAMESTATE_PRESSKEYPLAY_VMA", F_GAMESTATE_PRESSKEYPLAY_VMA},
    {"F_UI_PLAY_PORTING_DRAW_VMA", F_UI_PLAY_PORTING_DRAW_VMA},
    {"F_CHAR_MOVE_ON_WAIT_COORD_VMA", F_CHAR_MOVE_ON_WAIT_COORD_VMA},
    {"F_MAP_IS_BLOCKING_BYPIXEL_VMA", F_MAP_IS_BLOCKING_BYPIXEL_VMA},
    // 教程文案抑制入口（owner=popup_blocker）：hook handler 短路原绘制函数并返回 0。
    {"F_TUTORIAL_DESCRIPTION_VMA", F_TUTORIAL_DESCRIPTION_VMA},
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
    // 模块 choice 首帧去暗化 wrapper（owner=native_choice）。未登记时资源解析静默返回 0，
    // 整个 core.custom_ui 因 fail-closed 资源安装失败，依赖 ui_panels 的 settings 随之 blocked。
    {"F_SCENE_DRAW_POPUP_SC_CHOICE_VMA", F_SCENE_DRAW_POPUP_SC_CHOICE_VMA},
    // 面板插件（D1-b0 前置）：UI 面板注入所复用的 PopupState 死条目/可开面板 enter 回调，
    // 供 hooks::popup_state_resolve_symbol 按名解析（禁止面板出现裸 VMA）。
    {"F_PANEL_UNK1_ENTER", F_PANEL_UNK1_ENTER},
    {"F_PANEL_UNK2_ENTER", F_PANEL_UNK2_ENTER},
    {"F_PANEL_UNK3_ENTER", F_PANEL_UNK3_ENTER},
    {"F_PANEL_INVENTORY_ENTER", F_PANEL_INVENTORY_ENTER},
    // 人物属性面板（owner=character_panel）：属性总览点击检测占用该面板 PopupState event 槽。
    {"F_PANEL_CHARACTER_INFO_ENTER", F_PANEL_CHARACTER_INFO_ENTER},
    // 扩展背包（D2a-3a）：两个入口 hook 目标 + 三个 PopupState 面板 enter（部分 wrap 定位）。
    {"F_SAVE_SAVE_INVENTORY_VMA", F_SAVE_SAVE_INVENTORY_VMA},
    // 扩展背包控件读取入口与容量读取入口（owner=extension_bag）。
    {"F_CONTROL_ITEM_SET_ITEM_VMA", F_CONTROL_ITEM_SET_ITEM_VMA},
    {"F_CONTROL_ITEM_GET_ITEM_VMA", F_CONTROL_ITEM_GET_ITEM_VMA},
    {"F_GET_BAG_SIZE_VMA", F_GET_BAG_SIZE_VMA},
    {"F_UISTORE_SELL_ITEM_VMA", F_UISTORE_SELL_ITEM_VMA},
    {"F_UISTORE_BUTTON_SELL_EXE_VMA", F_UISTORE_BUTTON_SELL_EXE_VMA},
    {"F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA", F_UISTORE_DRAW_INVEN_BAG_GROUP_VMA},
    {"F_PANEL_SAVE_SLOT_ENTER", F_PANEL_SAVE_SLOT_ENTER},
    {"F_PANEL_SHOP_ENTER", F_PANEL_SHOP_ENTER},
    {"F_PANEL_CRAFT_ENTER", F_PANEL_CRAFT_ENTER},
    // 调试端点 nativeSettingsUiOpenOption：主菜单环境设置 state 的按名定位。
    {"F_PANEL_OPTIONS_ENTER", F_PANEL_OPTIONS_ENTER},
    // 特殊装备解锁：+7 bit4 的两个判定入口（入口 hook，owner=special_equip）。
    {"F_CAN_EQUIP_VMA", F_CAN_EQUIP_VMA},
    {"F_CAN_UNEQUIP_VMA", F_CAN_UNEQUIP_VMA},
    // 库存行为域入口 hook（owner=core.item，B2a-1 声明式化）：库存 wrapper 与快捷键
    // 诊断 wrapper 的挂载点；handler 在 plugins/core.item/hooks/inventory_hooks.cpp 取址。
    {"F_CHAR_USE_ITEM_EX_VMA", F_CHAR_USE_ITEM_EX_VMA},
    {"F_CHAR_GET_ACTION_COOLTIME_VMA", F_CHAR_GET_ACTION_COOLTIME_VMA},
    {"F_CHAR_PROCESS_SHORTCUT_VMA", F_CHAR_PROCESS_SHORTCUT_VMA},
    {"F_SHORTCUT_GET_STATE_VMA", F_SHORTCUT_GET_STATE_VMA},
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
    {"F_INVEN_FIND_SAVE_SLOT_VMA", F_INVEN_FIND_SAVE_SLOT_VMA},
    {"F_INVEN_MOVE_ITEM_VMA", F_INVEN_MOVE_ITEM_VMA},
    {"F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA", F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA},
    {"F_UIEQUIP_REFRESH_ITEM_AREA_VMA", F_UIEQUIP_REFRESH_ITEM_AREA_VMA},
    {"F_INVEN_SAVE_ITEM_DIRECT_VMA", F_INVEN_SAVE_ITEM_DIRECT_VMA},
    {"F_ITEMSYSTEM_DIVIDE_VMA", F_ITEMSYSTEM_DIVIDE_VMA},
    {"F_MAKE_ITEM_VMA", F_MAKE_ITEM_VMA},
    // 掉落域入口（owner=world_tuning）：整次掉落结算 + 装备候选选取。
    {"F_CHARSYSTEM_DROP_ITEM_VMA", F_CHARSYSTEM_DROP_ITEM_VMA},
    {"F_MAKE_EQUIP_DETAIL_VMA", F_MAKE_EQUIP_DETAIL_VMA},
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
    // 世界传送入口（owner=world_teleport）：小地图点击（地图名入口）的函数级接管点；
    // 2026-10-19 由尾部 kCallPatch 迁移 kEntryHook 时漏登记，导致 c18 上解析静默返回 0、
    // 插件资源未安装（preflight 无日志、不计 install_failed）。
    {"F_UI_PLAY_CALL_MAP_NAME_VMA", F_UI_PLAY_CALL_MAP_NAME_VMA},
    // 地图和字体文件共用的资源读取入口（owner=custom_map）；wrapper 仅替换 m<数字>.dat。
    {"F_RES_LOAD_TO_POOL_VMA", F_RES_LOAD_TO_POOL_VMA},
    // 合成套装使用效果入口（owner=craft_lottery）：CHAR_ProcessRecipe wrap 实现保底与弹窗追加。
    {"F_CHAR_PROCESS_RECIPE_VMA", F_CHAR_PROCESS_RECIPE_VMA},
    // 存档写盘域入口（owner=core.save）：SAVE_Save 完整写盘入口、继续存档、新档边界、选角取消。
    {"F_SAVE_VMA", F_SAVE_VMA},
    {"F_GAME_START_RESUME_GAME_VMA", F_GAME_START_RESUME_GAME_VMA},
    {"F_SAVESLOT_GO_TO_NEW_GAME_VMA", F_SAVESLOT_GO_TO_NEW_GAME_VMA},
    {"F_SELECTCHAR_BUTTON_BACK_EXE_VMA", F_SELECTCHAR_BUTTON_BACK_EXE_VMA},
    // custom_map 的无 UI 数据读取直接调用 LZMA_Decode；仅解析地址，不声明 hook 资源。
    {"F_LZMA_DECODE_VMA", F_LZMA_DECODE_VMA},
};

// 解析可安装 hook 的入口地址。
// 不再跟随入口处的无条件 b 跳板：monster 改版把入口改写成 `b <隐藏段桩>`，桩返回时会跳到
// 入口 +8；而 LSPosed 的 hook 原语在远址时固定写 12 字节 `adrp/add/br`，会覆盖该落点。
// 安装层改为对 b 跳板入口使用 4 字节短补丁（patch/short_hook），只改写入口第一条指令。
// out_followed 仍上报“入口是 b 跳板”，供诊断使用。
uintptr_t hookable_entry(uintptr_t symbol_entry, bool* out_followed) {
    *out_followed = false;
    if (symbol_entry == 0 || !game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'r')) {
        return symbol_entry;
    }
    const uint32_t word = *reinterpret_cast<const uint32_t*>(symbol_entry);
    constexpr uint32_t k_branch_mask = 0xFC000000u;   // b/bl/ret 等 opcode 掩码
    constexpr uint32_t k_b_opcode = 0x14000000u;      // B（无条件、不写 x30）
    if (!game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'x')) {
        // 非跳板入口必须落在可执行映射内，否则按未解析处理（fail-closed）。
        return 0;
    }
    if ((word & k_branch_mask) == k_b_opcode) *out_followed = true;
    return symbol_entry;
}

// b 的 imm26 符号扩展后按 4 字节缩放为相对偏移。
uintptr_t decode_b_target(uintptr_t from, uint32_t word) {
    int64_t imm = static_cast<int64_t>(word & 0x03FFFFFFu);
    if ((imm & 0x02000000) != 0) imm -= 0x04000000;
    return from + static_cast<uintptr_t>(imm * 4);
}

// 首条指令属于短桩/控制流转时拒绝跟随：这类目标的 +8 附近常是「桩返回入口+8」的落点，
// Dobby 的 12/16 字节补丁会把它一起覆盖（DropItem 事故形态）。正常函数序言（stp/mov/sub 等）
// 不在本判定内。
bool is_control_flow_first_instruction(uint32_t word) {
    if ((word & 0xFC000000u) == 0x14000000u) return true;  // b
    if ((word & 0xFC000000u) == 0x94000000u) return true;  // bl
    if ((word & 0xFF000010u) == 0x54000000u) return true;  // b.cond
    if ((word & 0x7E000000u) == 0x34000000u) return true;  // cbz/cbnz
    if ((word & 0x7E000000u) == 0x36000000u) return true;  // tbz/tbnz
    constexpr uint32_t kBrMask = 0xFFFFFC1Fu;
    if ((word & kBrMask) == 0xD61F0000u) return true;      // br
    if ((word & kBrMask) == 0xD63F0000u) return true;      // blr
    if ((word & kBrMask) == 0xD65F0000u) return true;      // ret
    return false;
}

std::atomic<bool> g_follow_refused_logged{false};

}  // namespace

uintptr_t resolve_hook_entry_follow(const char* symbol_name, bool* out_followed) {
    bool is_trampoline = false;
    const uintptr_t entry = resolve_hook_entry(symbol_name, &is_trampoline);
    if (out_followed != nullptr) *out_followed = false;
    if (entry == 0 || !is_trampoline) return entry;

    const uint32_t word = *reinterpret_cast<const uint32_t*>(entry);
    const uintptr_t target = decode_b_target(entry, word);
    const intptr_t delta = static_cast<intptr_t>(target) - static_cast<intptr_t>(entry);
    const bool distinct = delta > 16 || delta < -16;
    const bool aligned = (target & 0x3u) == 0;
    const bool executable =
        distinct && aligned &&
        game_memory_accessible(reinterpret_cast<void*>(target), 4, 'r') &&
        game_memory_accessible(reinterpret_cast<void*>(target), 4, 'x');
    if (!executable) {
        if (!g_follow_refused_logged.exchange(true)) {
            QOL_LOG_SEAM_WARN(kQolLogDomainCore,
                              "hook entry follow refused symbol=%s entry=0x%llx target=0x%llx "
                              "reason=target not distinct/executable",
                              symbol_name, static_cast<unsigned long long>(entry),
                              static_cast<unsigned long long>(target));
        }
        return entry;
    }

    if (is_control_flow_first_instruction(*reinterpret_cast<const uint32_t*>(target))) {
        if (!g_follow_refused_logged.exchange(true)) {
            QOL_LOG_SEAM_WARN(kQolLogDomainCore,
                              "hook entry follow refused symbol=%s entry=0x%llx target=0x%llx "
                              "reason=stub first instruction",
                              symbol_name, static_cast<unsigned long long>(entry),
                              static_cast<unsigned long long>(target));
        }
        return entry;
    }

    if (out_followed != nullptr) *out_followed = true;
    QOL_LOG_SEAM_INFO(kQolLogDomainCore,
                      "hook entry followed symbol=%s entry=0x%llx target=0x%llx",
                      symbol_name, static_cast<unsigned long long>(entry),
                      static_cast<unsigned long long>(target));
    return target;
}

uintptr_t resolve_hook_entry(const char* symbol_name, bool* out_followed) {
    // hookable_entry 始终解引用 out_followed；调用方允许传 nullptr 时用局部量承接。
    bool followed_local = false;
    bool* followed = out_followed != nullptr ? out_followed : &followed_local;
    if (symbol_name == nullptr) {
        *followed = false;
        return 0;
    }
    if (g_base == 0) {
        *followed = false;
        QOL_LOG_SEAM_WARN(kQolLogDomainCore,
                          "hook entry resolve deferred symbol=%s reason=game base not ready",
                          symbol_name);
        return 0;
    }
    const char* export_name = elf_export_name(symbol_name);
    if (export_name != nullptr) {
        *followed = false;
        if (export_name[0] == '\0') {
            QOL_LOG_SEAM_ERROR(kQolLogDomainCore,
                               "hook entry resolve failed elf-export=<empty> claim=%s",
                               symbol_name);
            return 0;
        }
        const uintptr_t offset = g_resolver.resolveFunctionByName(export_name);
        if (offset == 0) {
            QOL_LOG_SEAM_ERROR(kQolLogDomainCore,
                               "hook entry resolve failed elf-export=%s reason=export not found",
                               export_name);
            return 0;
        }
        const uintptr_t address = g_base + offset;
        if ((address & 0x3u) != 0 ||
            !game_memory_accessible(reinterpret_cast<void*>(address), 4, 'x')) {
            QOL_LOG_SEAM_ERROR(kQolLogDomainCore,
                               "hook entry resolve failed elf-export=%s reason=not executable "
                               "address=0x%llx",
                               export_name,
                               static_cast<unsigned long long>(address));
            return 0;
        }
        const uintptr_t entry = hookable_entry(address, followed);
        if (entry == 0) {
            QOL_LOG_SEAM_ERROR(kQolLogDomainCore,
                               "hook entry resolve failed elf-export=%s reason=hookable check",
                               export_name);
        }
        return entry;
    }
    for (const HookEntrySymbol& entry : kHookEntrySymbols) {
        if (std::strcmp(entry.name, symbol_name) != 0) continue;
        const uintptr_t symbol_entry = g_base + fn_resolve(entry.name, entry.vma);
        const uintptr_t resolved = hookable_entry(symbol_entry, followed);
        if (resolved == 0) {
            QOL_LOG_SEAM_ERROR(kQolLogDomainCore,
                               "hook entry resolve failed symbol=%s vma=0x%llx "
                               "reason=not executable (check game/access/symbols registration)",
                               symbol_name,
                               static_cast<unsigned long long>(symbol_entry));
        }
        return resolved;
    }
    *followed = false;
    QOL_LOG_SEAM_ERROR(kQolLogDomainCore,
                       "hook entry resolve failed symbol=%s reason=not registered in "
                       "game/access/symbols kHookEntrySymbols",
                       symbol_name);
    return 0;
}

}  // namespace qol::game
