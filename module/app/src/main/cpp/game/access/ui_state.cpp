#include "game/access/ui_state.h"

#include "game/access/game_access.h"

#include "contracts/extension_bag_port.h"
#include "contracts/popup_enter_alias.h"
#include "contracts/runtime_hooks.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"

#include <chrono>
#include <string>
#include <thread>

namespace qol::game {
namespace {

// 栈顶 enter VMA 是否为「模块可安全 Pop 的面板」（栏位与 ui_panel_close 一致）。
bool popup_vma_is_panel(uintptr_t vma) {
    switch (vma) {
        case F_PANEL_CHARACTER_INFO_ENTER: case F_PANEL_CHOICE_ENTER: case F_PANEL_INVENTORY_ENTER: case F_PANEL_INPUT_COUNT_ENTER:
        case F_PANEL_MERCENARY_ENTER: case F_PANEL_CRAFT_ENTER: case F_PANEL_NPC_ENTER: case F_PANEL_NPC_QUEST_ENTER:
        case F_PANEL_NPC_REST_ENTER: case F_PANEL_NPC_REVIVE_ENTER: case F_PANEL_OPTIONS_ENTER: case F_PANEL_QUESTS_ENTER:
        case F_PANEL_SAVE_SLOT_ENTER: case F_PANEL_CHAR_SELECT_ENTER: case F_PANEL_SHORTCUT_ENTER: case F_PANEL_SKILLS_ENTER:
        case F_PANEL_SHOP_ENTER: case F_PANEL_SETTINGS_ENTER: case F_PANEL_WIPEOUT_ENTER: case F_PANEL_WORLD_MAP_ENTER:
        case F_PANEL_IN_APP_ENTER: case F_PANEL_DAILY_REWARD_ENTER:
        case F_PANEL_UNK1_ENTER: case F_PANEL_UNK2_ENTER: case F_PANEL_UNK3_ENTER:
        case F_PANEL_UNK4_ENTER: case F_PANEL_UNK5_ENTER:
            return true;
        default:
            return false;
    }
}

uint32_t popup_stack_count() {
    if (g_popup_stack == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g_popup_stack) + 8);
}

bool wait_for_popup_stack_empty() {
    constexpr int kMaxWaitMs = 1000;
    for (int waited = 0; waited < kMaxWaitMs; waited += 16) {
        if (popup_stack_count() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return popup_stack_count() == 0;
}

UiWriteResult pack_result(const std::string& body) {
    UiWriteResult out{};
    const size_t size = body.size() < sizeof(out.text) ? body.size() : sizeof(out.text);
    if (size > 0) std::memcpy(out.text, body.data(), size);
    out.size = static_cast<uint32_t>(size);
    return out;
}

// 转场结果渲染：executed=true 时 out 为完整 op JSON；false 时为 busy/超时纯文本
//（迁移前调用点 `return op_err(result.c_str())`，空串同样进 error 字段，逐字保持）。
std::string render_transition(const QolResult& result, const char* body, uint32_t size) {
    const std::string text(body, size);
    if (!contracts::succeeded(result)) return op_err(text.c_str());
    return text;
}

void main_menu_body(void*, char* out, uint32_t capacity, uint32_t* out_len) {
    fn_gamestate_set_state(4);
    const std::string body = op_ok();
    const size_t size = body.size() < capacity ? body.size() : capacity;
    if (size > 0) std::memcpy(out, body.data(), size);
    *out_len = static_cast<uint32_t>(size);
}

}  // namespace

uintptr_t popup_top_vma() {
    if (g_popup_stack == nullptr || g_base == 0) return 0;
    uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
    uint32_t count = *reinterpret_cast<uint32_t*>(stk + POPUP_STACK_COUNT);
    if (count == 0 || count > 27) return 0;
    uint64_t data = *reinterpret_cast<uint64_t*>(stk + POPUP_STACK_DATA);
    if (data == 0) return 0;
    uint8_t* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) + (count - 1) * POPUP_ENTRY_SIZE;
    uintptr_t enter = *reinterpret_cast<uintptr_t*>(top + POPUP_ENTRY_ENTER);
    // D2b：扩展袋 inventory enter 识别经能力接口现取；能力缺失 = EB disabled，视为非扩展 enter。
    const contracts::ExtensionBagApi* eb = extension_bag_capability();
    if (eb != nullptr && eb->is_inventory_enter != nullptr && eb->is_inventory_enter(enter)) {
        return F_PANEL_INVENTORY_ENTER;
    }
    // 模块替换过 enter 回调的面板（商店/合成/存档槽等）在栈里存的是模块 wrapper 地址，
    // 「enter - g_base」会得到负值而被当成无面板，须先经反查表还原成原版 VMA。
    const uintptr_t aliased = popup_enter_alias_resolve(enter);
    if (aliased != kPopupEnterAliasNone) return aliased;
    return enter > g_base ? enter - g_base : 0;
}

const char* top_panel_name() {
    uintptr_t vma = popup_top_vma();
    switch (vma) {
        case F_PANEL_CHARACTER_INFO_ENTER: return "character_info";
        case F_PANEL_CHOICE_ENTER: return "choice";
        case F_PANEL_INVENTORY_ENTER: return "inventory";
        case F_PANEL_INPUT_COUNT_ENTER: return "input_count";
        case F_PANEL_MERCENARY_ENTER: return "mercenary";
        case F_PANEL_CRAFT_ENTER: return "craft";
        case F_PANEL_NPC_ENTER: return "npc";
        case F_PANEL_NPC_QUEST_ENTER: return "npc_quest";
        case F_PANEL_NPC_REST_ENTER: return "npc_rest";
        case F_PANEL_NPC_REVIVE_ENTER: return "npc_revive";
        case F_PANEL_OPTIONS_ENTER: return "options";
        case F_PANEL_QUESTS_ENTER: return "quests";
        case F_PANEL_SAVE_SLOT_ENTER: return "save_slot";
        case F_PANEL_CHAR_SELECT_ENTER: return "character_select";
        case F_PANEL_SHORTCUT_ENTER: return "shortcut";
        case F_PANEL_SKILLS_ENTER: return "skills";
        case F_PANEL_SHOP_ENTER: return "shop";
        case F_PANEL_SETTINGS_ENTER: return "settings";
        case F_PANEL_WIPEOUT_ENTER: return "wipeout";
        case F_PANEL_WORLD_MAP_ENTER: return "world_map";
        case F_PANEL_IN_APP_ENTER:
        case F_PANEL_UNK1_ENTER:
        case F_PANEL_UNK2_ENTER:
        case F_PANEL_UNK3_ENTER:
        case F_PANEL_UNK4_ENTER:
        case F_PANEL_UNK5_ENTER: return "in_app";
        case F_PANEL_DAILY_REWARD_ENTER: return "daily_reward";
        default: return nullptr;
    }
}

UiWriteResult ui_main_menu(bool direct) {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    if (fn_gamestate_set_state == nullptr || fn_ui_set_popup_process_info == nullptr) {
        return pack_result(op_err("symbol not resolved"));
    }
    if (g_base == 0 || g_popup_stack == nullptr) return pack_result(op_err("libgame not ready"));
    // 面板守卫：带着面板切 GAME_Exit 会触发游戏「terminate 面板后又画一帧」的空指针崩溃
    // （ControlItem_Draw(NULL) ← UIEquip_DrawEquip）。栈顶是面板则先走官方 Pop 并等栈清空；
    // 非面板弹窗（对话/剧情/数量输入等脚本态）fail-closed，不由模块代关。
    const uintptr_t top = popup_top_vma();
    if (top != 0) {
        if (!popup_vma_is_panel(top)) return pack_result(op_err("ui occupied: close current dialog first"));
        fn_ui_set_popup_process_info(3, 0);
        uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
        if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
        if (!wait_for_popup_stack_empty()) return pack_result(op_err("panel close pending"));
    }
    // 扩展背包清理保持原调用时机（切换前）；游戏状态机切换走逻辑帧点。
    // D2b：经能力接口现取；能力缺失 = EB disabled，无扩展态需清理，放行。
    const contracts::ExtensionBagApi* eb = extension_bag_capability();
    if (eb != nullptr && eb->prepare_main_menu != nullptr && !eb->prepare_main_menu()) {
        return pack_result(op_err("extension bag cleanup failed; main menu transition blocked"));
    }
    // 游戏状态机切换（GAMESTATE_SetState -> GAME_Exit）必须在游戏主线程的逻辑帧点执行；
    // HTTP worker 线程直调会与渲染线程并发破坏游戏内存分配器（见 transition_dispatch.h）。
    char body[256]{};
    uint32_t size = 0;
    if (direct) {
        const QolResult result = contracts::qol_runtime_hooks().transition_run_inline(
            &main_menu_body, nullptr, body, sizeof(body), &size);
        return pack_result(render_transition(result, body, size));
    }
    const QolResult result = contracts::qol_runtime_hooks().transition_run(
        &main_menu_body, nullptr, 5000, body, sizeof(body), &size);
    return pack_result(render_transition(result, body, size));
}

UiWriteResult ui_panel_close() {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    if (fn_ui_set_popup_process_info == nullptr) return pack_result(op_err("symbol not resolved"));
    // 前置：栈顶必须是面板（enter 匹配 PANELS），非空弹窗栈或弹窗（G_POPUP_ON）不处理
    if (g_base == 0 || g_popup_stack == nullptr) return pack_result(op_err("libgame not ready"));
    uint8_t* stk = reinterpret_cast<uint8_t*>(g_popup_stack);
    uint32_t count = *reinterpret_cast<uint32_t*>(stk + POPUP_STACK_COUNT);
    if (count == 0 || count > 27) return pack_result(op_err("no panel open"));
    uint64_t data = *reinterpret_cast<uint64_t*>(stk + POPUP_STACK_DATA);
    if (data == 0) return pack_result(op_err("no panel open"));
    uint8_t* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) + (count - 1) * POPUP_ENTRY_SIZE;
    uintptr_t enter = *reinterpret_cast<uintptr_t*>(top + POPUP_ENTRY_ENTER);
    uintptr_t vma = enter > g_base ? enter - g_base : 0;
    // D2b：扩展袋 inventory enter 识别经能力接口现取；能力缺失 = EB disabled，视为非扩展 enter。
    const contracts::ExtensionBagApi* eb = extension_bag_capability();
    if (eb != nullptr && eb->is_inventory_enter != nullptr && eb->is_inventory_enter(enter)) {
        vma = F_PANEL_INVENTORY_ENTER;
    }
    // 模块 wrapper 的 enter 不是 libgame 地址，上面的换算会归 0；经反查表还原原版 VMA。
    if (vma == 0) vma = popup_enter_alias_resolve(enter);
    if (!popup_vma_is_panel(vma)) return pack_result(op_err("top of stack is not a panel"));
    // 官方 ButtonBackExe 链：SOUNDSYSTEM_Play(0) + 流程3 + HUD 开关恢复
    fn_ui_set_popup_process_info(3, 0);
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
    return pack_result(op_ok());
}

UiWriteResult ui_panel_open(const std::string& panel) {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    if (fn_ui_set_popup_process_info == nullptr) return pack_result(op_err("symbol not resolved"));
    if (g_base == 0) return pack_result(op_err("libgame not ready"));
    // v0.5.42：兼容裸名与 panel_ 前缀（screen 输出 panel_inventory，输入可两者皆可）
    std::string p = panel;
    if (p.rfind("panel_", 0) == 0) p = p.substr(6);
    // 面板名 → enter VMA（与 ui_screen 的 PANELS 映射一致）
    uintptr_t target = 0;
    if (p == "character_info") target = F_PANEL_CHARACTER_INFO_ENTER;
    else if (p == "choice") target = F_PANEL_CHOICE_ENTER;
    else if (p == "inventory") target = F_PANEL_INVENTORY_ENTER;
    else if (p == "input_count") target = F_PANEL_INPUT_COUNT_ENTER;
    else if (p == "mercenary") target = F_PANEL_MERCENARY_ENTER;
    else if (p == "craft") target = F_PANEL_CRAFT_ENTER;
    else if (p == "npc") target = F_PANEL_NPC_ENTER;
    else if (p == "npc_quest") target = F_PANEL_NPC_QUEST_ENTER;
    else if (p == "npc_rest") target = F_PANEL_NPC_REST_ENTER;
    else if (p == "npc_revive") target = F_PANEL_NPC_REVIVE_ENTER;
    else if (p == "options") target = F_PANEL_OPTIONS_ENTER;
    else if (p == "quests") target = F_PANEL_QUESTS_ENTER;
    else if (p == "save_slot") target = F_PANEL_SAVE_SLOT_ENTER;
    else if (p == "character_select") target = F_PANEL_CHAR_SELECT_ENTER;
    else if (p == "shortcut") target = F_PANEL_SHORTCUT_ENTER;
    else if (p == "skills") target = F_PANEL_SKILLS_ENTER;
    else if (p == "shop") target = F_PANEL_SHOP_ENTER;
    else if (p == "settings") target = F_PANEL_SETTINGS_ENTER;
    else if (p == "wipeout") target = F_PANEL_WIPEOUT_ENTER;
    else if (p == "world_map") target = F_PANEL_WORLD_MAP_ENTER;
    else if (p == "in_app") target = F_PANEL_IN_APP_ENTER;
    else if (p == "daily_reward") target = F_PANEL_DAILY_REWARD_ENTER;
    else return pack_result(op_err("unknown panel"));
    // 面板可开白名单（v0.4.34 真机实测收紧）：仅允许不依赖外部上下文的独立面板。
    // 崩溃记录（全部 SIGSEGV，tombstone 已验证）：
    //   options      → GAMELOADER_DrawBackGround→GRPX_DrawPart（主菜单/GAMELOADER 场景专属）
    //   craft/shop   → CHAR_GetName 空指针（需 NPC 交互对象 [0x2f6000+0xc20]→[x0] 就绪）
    //   input_count  → ControlObject_GetActive 空控件（需 inventory 物品数量输入上下文）
    // 语义不正确的面板（v0.4.34 移除）：
    //   choice       → 游戏内由事件/剧情驱动的选择框，API 打开语义不符
    //   world_map    → 由游戏内事件（如保存点）驱动的世界地图，API 打开语义不符
    //   wipeout      → 角色死亡时游戏自动打开，非用户可操作面板
    // 其余未实证面板（npc 系列/shortcut/in_app 等）同样拒绝，避免 API 直接 Push 崩溃。
    bool openable = (p == "character_info" || p == "inventory" ||
                     p == "mercenary" || p == "quests" || p == "settings" ||
                     p == "skills");
    if (!openable) return pack_result(op_err("panel requires in-game context"));
    // 扫描 state list 找 enter == g_base+target 的 state id
    uint8_t* list = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (list == nullptr) return pack_result(op_err("state list not ready"));
    int state_id = -1;
    if (p == "inventory") {
        // D2b：扩展袋 inventory state id 经能力接口现取；能力缺失 = EB disabled，返回 -1。
        const contracts::ExtensionBagApi* eb = extension_bag_capability();
        if (eb != nullptr && eb->inventory_state_id != nullptr) state_id = eb->inventory_state_id();
    }
    for (int i = 0; state_id < 0 && i < 27; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * POPUP_ENTRY_SIZE + POPUP_ENTRY_ENTER);
        if (enter == g_base + target) { state_id = i; break; }
    }
    if (state_id < 0) return pack_result(op_err("panel state not found"));

    // UI_SetPopupProcessInfo(1, id) 是 Push，不会替换已有栈顶。相同面板直接幂等返回；
    // 切换面板先走官方 Pop，并等待主循环真正清空栈，避免 Pop/Open 同帧再次叠加。
    uintptr_t top = popup_top_vma();
    if (top != 0) {
        if (top == target) return pack_result(op_ok());
        bool top_is_panel = false;
        switch (top) {
            case F_PANEL_CHARACTER_INFO_ENTER: case F_PANEL_INVENTORY_ENTER:
            case F_PANEL_MERCENARY_ENTER: case F_PANEL_QUESTS_ENTER:
            case F_PANEL_CRAFT_ENTER: case F_PANEL_NPC_REST_ENTER:
            case F_PANEL_NPC_REVIVE_ENTER: case F_PANEL_OPTIONS_ENTER:
            case F_PANEL_SAVE_SLOT_ENTER: case F_PANEL_CHAR_SELECT_ENTER:
            case F_PANEL_SHORTCUT_ENTER: case F_PANEL_SKILLS_ENTER:
            case F_PANEL_SHOP_ENTER: case F_PANEL_SETTINGS_ENTER:
            case F_PANEL_WORLD_MAP_ENTER: case F_PANEL_IN_APP_ENTER:
            case F_PANEL_DAILY_REWARD_ENTER:
            case F_PANEL_UNK1_ENTER: case F_PANEL_UNK2_ENTER:
            case F_PANEL_UNK3_ENTER: case F_PANEL_UNK4_ENTER:
            case F_PANEL_UNK5_ENTER:
                top_is_panel = true;
                break;
            default: break;
        }
        if (!top_is_panel) return pack_result(op_err("top of stack is not a panel"));
        fn_ui_set_popup_process_info(3, 0);
        if (!wait_for_popup_stack_empty()) return pack_result(op_err("panel close pending"));
    }
    fn_ui_set_popup_process_info(1, state_id);
    return pack_result(op_ok());
}

}  // namespace qol::game
