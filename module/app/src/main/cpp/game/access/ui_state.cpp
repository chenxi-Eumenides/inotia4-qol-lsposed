#include "game/access/ui_state.h"
#include "game/access/world.h"

#include "game/access/game_access.h"

#include "contracts/popup_enter_alias.h"
#include "contracts/runtime_hooks.h"
#include "game/adapter_ports/lifecycle_participant_facade.h"
#include "game/access/save.h"
#include "game/json/game_json.h"
#include "contracts/game_cache.h"
#include "game/ops/game_ops_common.h"
#include "game/state/game_state.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace qol::game {
namespace {

thread_local char g_popup_snapshot_failure[256]{};

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
    // 生命周期参与者识别扩展 inventory enter；无可用参与者视为非扩展 enter。
    if (lifecycle_participant_is_inventory_enter(enter)) {
        return F_PANEL_INVENTORY_ENTER;
    }
    // 模块替换过 enter 回调的面板（商店/合成/存档槽等）在栈里存的是模块 wrapper 地址，
    // 「enter - g_base」会得到负值而被当成无面板，须先经反查表还原成原版 VMA。
    const uintptr_t aliased = popup_enter_alias_resolve(enter);
    if (aliased != kPopupEnterAliasNone) return aliased;
    return enter > g_base ? enter - g_base : 0;
}

bool popup_stack_snapshot(uint32_t* out_count, uintptr_t* out_top_enter_vma) {
    g_popup_snapshot_failure[0] = '\0';
    if (out_count == nullptr || out_top_enter_vma == nullptr || g_popup_stack == nullptr || g_base == 0) {
        std::snprintf(g_popup_snapshot_failure, sizeof(g_popup_snapshot_failure),
                      "g_base=0x%lx g_popup_stack=0x%lx output_valid=%d",
                      static_cast<unsigned long>(g_base),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_popup_stack)),
                      out_count != nullptr && out_top_enter_vma != nullptr ? 1 : 0);
        return false;
    }
    auto* stack = reinterpret_cast<uint8_t*>(g_popup_stack);
    const bool count_readable = game_memory_accessible(stack + POPUP_STACK_COUNT,
                                                       sizeof(uint32_t), 'r');
    const bool data_readable = game_memory_accessible(stack + POPUP_STACK_DATA,
                                                      sizeof(uint64_t), 'r');
    if (!count_readable || !data_readable) {
        std::snprintf(g_popup_snapshot_failure, sizeof(g_popup_snapshot_failure),
                      "g_base=0x%lx g_popup_stack=0x%lx count_readable=%d data_readable=%d",
                      static_cast<unsigned long>(g_base),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_popup_stack)),
                      count_readable ? 1 : 0, data_readable ? 1 : 0);
        return false;
    }
    const uint32_t count = *reinterpret_cast<uint32_t*>(stack + POPUP_STACK_COUNT);
    if (count > 27) {
        std::snprintf(g_popup_snapshot_failure, sizeof(g_popup_snapshot_failure),
                      "g_base=0x%lx g_popup_stack=0x%lx count=%u upper_bound=27",
                      static_cast<unsigned long>(g_base),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_popup_stack)), count);
        return false;
    }
    if (count == 0) {
        *out_count = 0;
        *out_top_enter_vma = 0;
        return true;
    }
    const uint64_t data = *reinterpret_cast<uint64_t*>(stack + POPUP_STACK_DATA);
    if (data == 0) {
        std::snprintf(g_popup_snapshot_failure, sizeof(g_popup_snapshot_failure),
                      "g_base=0x%lx g_popup_stack=0x%lx count=%u data=0",
                      static_cast<unsigned long>(g_base),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_popup_stack)), count);
        return false;
    }
    auto* top = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data)) +
                (count - 1) * POPUP_ENTRY_SIZE;
    const bool top_readable = game_memory_accessible(top + POPUP_ENTRY_ENTER,
                                                      sizeof(uintptr_t), 'r');
    if (!top_readable) {
        std::snprintf(g_popup_snapshot_failure, sizeof(g_popup_snapshot_failure),
                      "g_base=0x%lx g_popup_stack=0x%lx count=%u data=0x%lx top_readable=0",
                      static_cast<unsigned long>(g_base),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_popup_stack)), count,
                      static_cast<unsigned long>(data));
        return false;
    }
    const uintptr_t enter = *reinterpret_cast<uintptr_t*>(top + POPUP_ENTRY_ENTER);
    uintptr_t vma = 0;
    if (lifecycle_participant_is_inventory_enter(enter)) {
        vma = F_PANEL_INVENTORY_ENTER;
    } else {
        const uintptr_t aliased = popup_enter_alias_resolve(enter);
        vma = aliased != kPopupEnterAliasNone ? aliased : (enter > g_base ? enter - g_base : 0);
    }
    *out_count = count;
    *out_top_enter_vma = vma;
    return true;
}

const char* popup_stack_snapshot_failure() {
    return g_popup_snapshot_failure;
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
    // 无可用参与者等同旧“能力缺失”，跳过清理放行。
    if (!lifecycle_participant_prepare_main_menu()) {
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
    // 生命周期参与者识别扩展 inventory enter；无可用参与者视为非扩展 enter。
    if (lifecycle_participant_is_inventory_enter(enter)) {
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
        // 无可用参与者返回 -1，继续原有 27 项扫描。
        state_id = lifecycle_participant_inventory_state_id();
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


// ---------------------------------------------------------------------------
// T2l-d：对话写原语（迁自 game_dialog_operations.inc）。story 判定用本文件本地直读
// （等价原 data_story_active：g_gamestate==1）。
// ---------------------------------------------------------------------------
namespace {

DialogWriteResult pack_dialog(const std::string& body) {
    DialogWriteResult out{};
    const size_t size = body.size() < sizeof(out.text) ? body.size() : sizeof(out.text);
    if (size > 0) std::memcpy(out.text, body.data(), size);
    out.size = static_cast<uint32_t>(size);
    return out;
}

}  // namespace

DialogWriteResult npc_interact() {
    if (!game_in_world()) return pack_dialog(op_err("not in game"));
    if (const char* ui = ui_blocked()) {
        std::string err = "ui occupied: ";
        err += ui;
        return pack_dialog(op_err(err.c_str()));
    }
    if (fn_player_check_near_npc == nullptr || fn_uinpc_init == nullptr ||
        fn_check_function_display == nullptr)
        return pack_dialog(op_err("symbol not resolved"));
    fn_player_check_near_npc();
    void* near_npc = *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA);
    // v0.4.52：NearNPC 空时模拟触摸交互链——扫描角色池找玩家面前/附近的 type==2 可交互物
    // （路障/宝箱）。官方触摸链不经过 NearNPC 24px 检测，直接检测点击处对象；玩家距路障 32px
    // 超阈值但触摸可交互（b54 真机：手动点击成功建立路障对话）。这里放宽到 3 格（48px）且朝向匹配。
    if (near_npc == nullptr && g_base != 0) {
        void* hero = lead_member();
        if (hero != nullptr) {
            int hx = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_X);
            int hy = *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(hero) + C_POS_Y);
            int hdir = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(hero) + C_DIRECTION);
            uint8_t* pool = *reinterpret_cast<uint8_t**>(g_base + G_CHAR_POOL_VMA);
            if (pool != nullptr) {
                int best_dist = 60, best_slot = -1;
                for (int i = 0; i < C_CHARSYSTEM_POOL_SLOTS; ++i) {
                    uint8_t* obj = pool + i * C_OBJ_SIZE;
                    int16_t x = *reinterpret_cast<int16_t*>(obj + C_POS_X);
                    int16_t y = *reinterpret_cast<int16_t*>(obj + C_POS_Y);
                    int type = static_cast<int>(reinterpret_cast<int8_t*>(obj)[C_TYPE]);
                    if (type != 2) continue;
                    if (x <= 0 || x >= 1500 || y <= 0 || y >= 1500) continue;
                    int dx = x - hx, dy = y - hy;
                    int dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    if (dist > 60) continue;
                    // 朝向匹配：dir 0=下 1=左 2=上 3=右（dx/dy 符号对应）
                    bool facing = (hdir == 3 && dx > 0) || (hdir == 1 && dx < 0) ||
                                  (hdir == 0 && dy > 0) || (hdir == 2 && dy < 0);
                    if (!facing) continue;
                    if (dist < best_dist) { best_dist = dist; best_slot = i; }
                }
                if (best_slot >= 0) {
                    near_npc = pool + best_slot * C_OBJ_SIZE;
                    *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA) = near_npc;
                }
            }
        }
        if (near_npc == nullptr) {
            if (fn_evtsystem_do_check_all_event == nullptr) return pack_dialog(op_err("no npc nearby"));
            fn_evtsystem_do_check_all_event(2);
            return pack_dialog(op_ok());
        }
    }
    // 官方交互链（GAMESTATE_PressKeyPlay 交互分支）：读 npc+0xa u16 funcDisplay，
    // NPCSYSTEM_CheckFunctionDisplay 返回 0=弹 UI / 1=直接执行任务 / 2=不可交互。
    uint16_t func_display = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(near_npc) + C_FUNC_DISPLAY);
    int fd = fn_check_function_display(static_cast<int32_t>(func_display));
    if (fd > 1) return pack_dialog(op_err("not interactable"));
    uint8_t r = fn_uinpc_init();
    if (!r) return pack_dialog(op_err("interact failed"));
    frame_cache_force_refresh();
    if (fd == 0) {
        int state_id = -1;
        uint8_t* list = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
        if (list != nullptr) {
            for (int i = 0; i < 27; ++i) {
                uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * POPUP_ENTRY_SIZE + POPUP_ENTRY_ENTER);
                if (enter == g_base + F_PANEL_NPC_ENTER) { state_id = i; break; }
            }
        }
        if (state_id < 0) return pack_dialog(op_err("npc panel state not found"));
        if (fn_ui_set_popup_process_info == nullptr) return pack_dialog(op_err("symbol not resolved"));
        fn_ui_set_popup_process_info(1, state_id);
        return pack_dialog("{\"ok\":true,\"result\":\"dialog_shown\"}");
    }
    if (fn_uinpc_exe_current_task == nullptr) return pack_dialog(op_err("symbol not resolved"));
    fn_uinpc_exe_current_task();
    return pack_dialog("{\"ok\":true,\"result\":\"task_executed\"}");
}

DialogWriteResult npc_dialog_select(int index) {
    if (!game_in_world()) return pack_dialog(op_err("not in game"));
    if (fn_uinpc_exe_current_task == nullptr) return pack_dialog(op_err("symbol not resolved"));
    uint8_t count = *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA);
    if (index < 0 || index >= count) return pack_dialog(op_err("bad index"));
    *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_INDEX_VMA) = static_cast<uint8_t>(index);
    fn_uinpc_exe_current_task();
    return pack_dialog(op_ok());
}

DialogWriteResult dialog_select(const std::string& action, int index) {
    if (!game_in_world()) return pack_dialog(op_err("not in game"));
    // 先按当前态确定合法 action 集合（与 dialog content 判定一致：popup 最优先）
    if (popup_top_vma() == F_PANEL_WIPEOUT_ENTER) {
        // wipeout 的专用场景处理不兼容通用 popup 按钮执行函数。
        if (action != "game_over") return pack_dialog(op_err("no such option in wipeout"));
    } else if (g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on)) {
        if (action != "ok" && action != "cancel") return pack_dialog(op_err("no such option in popup"));
    } else if (story_active()) {
        if (action != "next" && action != "skip") return pack_dialog(op_err("no such option in story"));
    } else if (popup_top_vma() == F_PANEL_NPC_QUEST_ENTER) {
        // npc_quest 面板：仅接受 complete（完成任务）/ close（关闭面板）
        if (action != "complete" && action != "close") return pack_dialog(op_err("no such option in npc_quest"));
    } else if (g_base != 0 &&
               (*reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA) > 0 ||
                *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA) > 0)) {
        if (action != "next" && action != "close" && index < 0) return pack_dialog(op_err("no such option in npc"));
    } else if (top_panel_name() != nullptr) {
        // 面板态（v0.5.6 U1）：save_slot 接受 save/close，其余面板仅 close（panel/close 官方流程3）
        if (action != "close" && !(action == "save" && strcmp(top_panel_name(), "save_slot") == 0)) {
            std::string err = "no such option in ";
            err += top_panel_name();
            return pack_dialog(op_err(err.c_str()));
        }
    } else {
        return pack_dialog(op_err("no dialog"));
    }
    if (action == "next") {
        if (story_active()) return story_next();
        // 非 story 的 next（NPC/任务列表对话）：用与 get-content 同源的安全文本读取
        // （G_NPCTASKLIST_DESCTEXT_VMA 指向当前对话文本），避免 fn_npctasklist_make_dlg
        // 对非 NPC 对象（如路障交互）返回悬垂指针导致 json_escape 越界崩溃（v0.4.48 修复）
        char* desc = nullptr;
        if (g_base != 0) desc = *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA);
        if (desc == nullptr || desc[0] == 0) return pack_dialog(op_err("no dialog"));
        return pack_dialog("{\"ok\":true,\"text\":\"" + json_escape(desc) + "\"}");
    }
    if (action == "skip") {
        if (!story_active()) return pack_dialog(op_err("no story"));
        return story_skip();
    }
    if (action == "ok") return dialog_ok();
    if (action == "cancel") return dialog_cancel();
    // npc_quest 面板动作（v0.4.55）：complete=完成任务（UINpcQuest_ButtonOKExe 官方链：
    // questIdx→stateTbl 判定→完成态走 UI_SetPopupProcessInfo(3,0)+ChangeQuestState+DoCheckAllEvent），
    // close=复用面板关闭（panel/close 官方流程3）。
    if (popup_top_vma() == F_PANEL_NPC_QUEST_ENTER) {
        if (action == "complete") {
            if (fn_uinpc_quest_button_ok_exe == nullptr) return pack_dialog(op_err("symbol not resolved"));
            fn_uinpc_quest_button_ok_exe();
            return pack_dialog(op_ok());
        }
        if (action == "close") return pack_dialog(ui_panel_close().str());
    }
    // wipeout 死亡面板动作：仅接受 game_over（revive/special_revive 走网络链，离线无作用，已移除）。
    if (action == "game_over") {
        if (popup_top_vma() != F_PANEL_WIPEOUT_ENTER) return pack_dialog(op_err("not in wipeout"));
        if (fn_wipeout_button_gameover == nullptr) return pack_dialog(op_err("symbol not resolved"));
        fn_wipeout_button_gameover();
        return pack_dialog(op_ok());
    }
    if (action == "close") return pack_dialog(ui_panel_close().str());
    if (index >= 0) return npc_dialog_select(index);
    // 面板态动作（v0.5.6 U1）：save=存档落盘、close=关闭面板（panel/close 官方流程3）
    if (top_panel_name() != nullptr) {
        if (action == "save") return pack_dialog(save_game().str());
        if (action == "close") return pack_dialog(ui_panel_close().str());
    }
    return pack_dialog(op_err("bad action"));
}

DialogWriteResult dialog_ok() {
    if (g_base == 0) return pack_dialog(op_err("libgame not loaded"));
    if (popup_top_vma() == F_PANEL_WIPEOUT_ENTER)
        return pack_dialog(op_err("no such option in wipeout"));
    if (*reinterpret_cast<uint8_t*>(g_base + G_POPUP_ON_VMA) == 0) return pack_dialog(op_err("no dialog"));
    reinterpret_cast<void (*)()>(g_base + F_BUTTON_OK_EXE_VMA)();
    return pack_dialog(op_ok());
}

DialogWriteResult dialog_cancel() {
    if (g_base == 0) return pack_dialog(op_err("libgame not loaded"));
    if (popup_top_vma() == F_PANEL_WIPEOUT_ENTER)
        return pack_dialog(op_err("no such option in wipeout"));
    if (*reinterpret_cast<uint8_t*>(g_base + G_POPUP_ON_VMA) == 0) return pack_dialog(op_err("no dialog"));
    reinterpret_cast<void (*)()>(g_base + F_BUTTON_CANCEL_EXE_VMA)();
    return pack_dialog(op_ok());
}

DialogWriteResult story_next() {
    if (g_base == 0) return pack_dialog(op_err("not in game"));
    if (fn_event_button_ok_exe == nullptr) return pack_dialog(op_err("symbol not resolved"));
    fn_event_button_ok_exe();
    return pack_dialog(op_ok());
}

DialogWriteResult story_skip() {
    if (g_base == 0) return pack_dialog(op_err("not in game"));
    if (fn_event_button_skip_exe == nullptr) return pack_dialog(op_err("symbol not resolved"));
    fn_event_button_skip_exe();
    return pack_dialog(op_ok());
}

std::string npc_dialog_options_json() {
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    std::string out = "{\"count\":" +
        std::to_string(static_cast<int>(*reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA)));
    out += ",\"focus\":" +
        std::to_string(static_cast<int>(*reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_FOCUS_VMA)));
    out += ",\"options\":[";
    void** texts = reinterpret_cast<void**>(g_base + G_UICHOICE_ITEMTEXT_VMA);
    for (int i = 0; i < 6; ++i) {
        if (i > 0) out += ",";
        char* t = reinterpret_cast<char*>(texts[i]);
        if (t != nullptr) {
            out += "\"" + json_escape(t) + "\"";
        } else {
            out += "null";
        }
    }
    out += "]}";
    return out;
}

std::string dialog_content_json() {
    // 弹窗最优先（v0.4.39 修复）：剧情段结束弹任务简报时 gs=1 残留但 UIPopupMsg 激活，
    // 若 story 优先会遮蔽弹窗 → select ok 无法确认任务简报。弹窗会阻塞一切下层交互。
    if (g_base != 0 && g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on)) {
        std::string out = "{\"type\":\"popup\"";
        uint8_t* pt = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_TEXT_VMA);
        if (pt != nullptr) {
            std::string dtext;
            for (int i = 0; i < 256 && pt[i] != 0; ++i) dtext += static_cast<char>(pt[i]);
            out += ",\"text\":\"" + json_escape(dtext.c_str()) + "\"";
        }
        bool has_ok = *reinterpret_cast<uint64_t*>(g_base + G_POPUP_FPOK_VMA) != 0;
        bool has_cancel = *reinterpret_cast<uint64_t*>(g_base + G_POPUP_FPCANCEL_VMA) != 0;
        out += ",\"options\":[";
        bool first = true;
        if (has_ok) { out += "{\"id\":\"ok\",\"label\":\"确认\"}"; first = false; }
        if (has_cancel) { if (!first) out += ","; out += "{\"id\":\"cancel\",\"label\":\"取消\"}"; }
        out += "]}";
        return out;
    }
    if (!game_in_world()) return "{\"error\":\"not in game\"}";
    if (story_active()) {
        // story 态：统一 type 字段 + 剧情推进/跳过作为选项暴露（v0.4.31 修复）
        std::string sj = story_json();
        if (sj.size() > 1 && sj[0] == '{') {
            sj.insert(1, "\"type\":\"story\",\"options\":[{\"id\":\"next\",\"label\":\"下一句\"},{\"id\":\"skip\",\"label\":\"跳过\"}],");
        }
        return sj;
    }
    if (g_base == 0) return "{\"type\":\"none\",\"options\":[]}";
    // NPC 对话（UICHOICE 选项优先）
    uint8_t choice_count = *reinterpret_cast<uint8_t*>(g_base + G_UICHOICE_COUNT_VMA);
    uint8_t task_count = *reinterpret_cast<uint8_t*>(g_base + G_NPCTASKLIST_COUNT_VMA);
    // wipeout 死亡面板（v0.4.35）：栈顶 enter == F_PANEL_WIPEOUT_ENTER 时优先于 NPC 对话报告
    uintptr_t top_vma = qol::game::popup_top_vma();
    if (top_vma == F_PANEL_WIPEOUT_ENTER) {
        // 仅暴露「游戏结束」（game_over）一项：revive/special_revive 走网络链
        // （CS_netGetActiveNetwork），离线/无网络时无作用，故不再作为可选项提供。
        std::string out = "{\"type\":\"wipeout\",\"options\":["
                          "{\"id\":\"game_over\",\"label\":\"游戏结束\"}]}";
        return out;
    }
    // NPC 任务完成面板（v0.4.55）：栈顶 enter == F_PANEL_NPC_QUEST_ENTER（npc_quest）时报告任务完成态，
    // 选项 complete=完成任务（UINpcQuest_ButtonOKExe 官方链）close=关闭面板（panel/close）。
    if (top_vma == F_PANEL_NPC_QUEST_ENTER) {
        std::string out = "{\"type\":\"npc_quest\"";
        int quest_id = -1;
        if (g_base != 0) {
            uint8_t** idx_ptr = reinterpret_cast<uint8_t**>(g_base + G_NPC_QUEST_IDX_GOT_VMA);
            if (idx_ptr != nullptr && *idx_ptr != nullptr)
                quest_id = *reinterpret_cast<int16_t*>(*idx_ptr);
        }
        out += ",\"quest_id\":" + std::to_string(quest_id);
        uint8_t state = 0xFF;
        if (g_base != 0 && quest_id >= 0) {
            uint8_t*** st_got = reinterpret_cast<uint8_t***>(g_base + G_NPC_QUEST_STATE_GOT_VMA);
            if (st_got != nullptr && *st_got != nullptr)
                state = (**st_got)[quest_id];
        }
        out += ",\"state\":" + std::to_string(static_cast<int>(state));
        out += ",\"options\":[{\"id\":\"complete\",\"label\":\"完成任务\"},"
               "{\"id\":\"close\",\"label\":\"关闭\"}]}";
        return out;
    }
    if (choice_count > 0 || task_count > 0) {
        std::string out = "{\"type\":\"npc\"";
        // displayed：区分「UI 对话框已显示」（popup 栈顶 npc 面板）vs「数据已建立但未渲染」
        // （如 start_interact 前的数据态、路过不可交互装饰物残留的 NEAR_NPC 数据）。
        out += ",\"displayed\":" +
               std::string(qol::game::popup_top_vma() == F_PANEL_NPC_ENTER ? "true" : "false");
        void* near_npc = *reinterpret_cast<void**>(g_base + G_PLAYER_NEAR_NPC_VMA);
        if (near_npc != nullptr && fn_get_name != nullptr) {
            char* nm = fn_get_name(near_npc);
            if (nm != nullptr) out += ",\"speaker\":\"" + json_escape(nm) + "\"";
        }
        char* desc = *reinterpret_cast<char**>(g_base + G_NPCTASKLIST_DESCTEXT_VMA);
        if (desc != nullptr && desc[0] != 0) {
            out += ",\"text\":\"" + json_escape(desc) + "\"";
        }
        out += ",\"options\":[";
        if (choice_count > 0) {
            void** texts = reinterpret_cast<void**>(g_base + G_UICHOICE_ITEMTEXT_VMA);
            for (int i = 0; i < choice_count && i < 6; ++i) {
                if (i > 0) out += ",";
                char* t = reinterpret_cast<char*>(texts[i]);
                out += "{\"id\":\"" + std::to_string(i) + "\",\"label\":" +
                       (t != nullptr ? "\"" + json_escape(t) + "\"" : "\"\"") + "}";
            }
        } else if (task_count > 0) {
            // NPC 选项列表（NPCTASKLIST 槽数组：32×16B，+0 type/+2 id/+8 文本指针）。
            // 选择框型 NPC（如商人）选项在此，非线性 next（v0.6.6 修复：原误判为 next）。
            void* slots = *reinterpret_cast<void**>(g_base + G_NPCTASKLIST_PDATA_VMA);
            bool first = true;
            for (int i = 0; i < task_count && i < 32; ++i) {
                char* label = slots != nullptr
                    ? *reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(slots) + i * 16 + 8)
                    : nullptr;
                if (!first) out += ",";
                out += "{\"id\":\"" + std::to_string(i) + "\",\"label\":" +
                       (label != nullptr ? "\"" + json_escape(label) + "\"" : "\"\"") + "}";
                first = false;
            }
            if (!first) out += ",";
            out += "{\"id\":\"close\",\"label\":\"关闭\"}";
        }
        out += "]}";
        return out;
    }
    // 面板态（v0.5.6 U1）：无弹窗/剧情/死亡/NPC 对话时，若栈顶是面板则报面板类型 + 动作。
    // save_slot 额外暴露 save（存档落盘，官方 save 链）；其余面板仅 close（panel/close 官方流程3）。
    const char* pname = qol::game::top_panel_name();
    if (pname != nullptr) {
        std::string out = "{\"type\":\"" + std::string(pname) + "\",\"options\":[";
        if (strcmp(pname, "save_slot") == 0)
            out += "{\"id\":\"save\",\"label\":\"存档\"},";
        out += "{\"id\":\"close\",\"label\":\"关闭\"}]}";
        return out;
    }
    return "{\"type\":\"none\",\"options\":[]}";
}


}  // namespace qol::game
