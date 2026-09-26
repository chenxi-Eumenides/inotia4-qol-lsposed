#pragma once

#include <cstdint>
#include <string>

#include "game/symbols/game_symbols.h"

// 主线程世界事实 JSON 构造器；由 world_facts 的帧快照调用，HTTP/预取线程不得直接调用。
// 实现仍在 legacy 的 api/native/game_world_readers.inc（收口前先声明，避免 game 层 include legacy 头）。
std::string build_units_json();
std::string build_enemies_json();
std::string build_interactives_json();
std::string build_drops_json();

// 游戏层：世界/地图域语义化访问（ARCHITECTURE.md §5）。
// MAPINFOBASE 记录数/记录、当前地图 id、地图名解析、切图调用、主控朝向、金币/弹窗/横幅
// 等游戏原语收口在本层；上层只调用语义函数，不出现裸地址/裸偏移、不直接 include 符号头。
//
// 线程纪律：MAPINFOBASE/弹窗/玩家相关函数只应在游戏主线程调用（读游戏全局与控件状态）；
// 调用方须保证 g_base 已定位（world_map_change_ready/world_map_name 等内部按未就绪返回
// 明确失败值，不解引用空指针）。

namespace qol::game {

// 静态地图 ID 上限（原版/改版共同；MAPINFOBASE_STATIC_MAX_MAP_ID 的 game 层别名）。
// 纯逻辑规则（plugins/world_teleport/rules.h）经本常量取上限，避免直接 include 符号头。
inline constexpr int kWorldStaticMaxMapId = MAPINFOBASE_STATIC_MAX_MAP_ID;

// ---- 世界状态 ----

// g_base 是否已定位（其余世界访问器的总前置）。未就绪时各函数返回明确失败值。
bool world_ready();

// g_state 指向的当前 state（u16）；未就绪返回 -1。世界态值由调用方判定。
int world_state();

// g_gamestate 门禁原语；空指针按原扫描实现解释为 0，其他值为游戏中的原始状态。
// 调用方将原始值等于 0 解释为 autosell 的可扫描门禁。
int world_game_state_gate();

// ---- 地图信息（MAPINFOBASE + MEMORYTEXT）----

// 运行时地图记录数（u16）；未就绪/不可读返回 0。
int world_map_record_count();

// 地图中文名（MAPINFOBASE 记录 + MEMORYTEXT）；未命中/未就绪/不可读返回 nullptr，
// 由调用方走兜底文案。返回指针指向游戏内存，调用方不得释放。
const char* world_map_name(int map_id);

// 当前地图 id；未就绪返回 0（与迁移前 current_map_id 口径一致）。
int world_current_map_id();

// ---- 切图调用 ----

// MAPCHANGE_Set 与 GAMESTATE_SetState 是否都已解析。
bool world_map_change_ready();
// 发起切图：MAPCHANGE_Set(map_id,x,y,dir) + GAMESTATE_SetState(MAP_CHANGE)。
// 调用方须先经 world_map_change_ready 判定且处于游戏主线程。
void world_request_map_change(int map_id, int x, int y, int dir);

// ---- 主控角色朝向 ----

// 主控角色 +C_DIRECTION（u8）；未就绪/不可读返回 -1。
int world_active_player_direction();

// ---- 金币 ----

// INVEN_GetMoney / INVEN_MinusMoney 是否都已解析。
bool world_money_ready();
int64_t world_money();
// 扣钱；函数未解析返回 false；成功（游戏返回值非 0）返回 true。
bool world_minus_money(int64_t amount);

// ---- 弹窗 / 横幅 / HUD ----

// UIPopupMsg_CreateYesNo 是否已解析。
bool world_create_yesno_ready();
// YesNo 确认框（dispType=0, type=2）；未解析返回 false，否则调用并返回 true。
bool world_create_yesno(const char* text, uint32_t len, void* ok, void* cancel, void* param);

// UI_SetPopupProcessInfo 是否已解析。
bool world_close_popup_ready();
// 关闭当前 popup：UI_SetPopupProcessInfo(3, 0)。未解析返回 false。
bool world_close_popup_process();
// 恢复 HUD 显示开关（G_HUD_GATE_GOT_VMA 双层解引用写 1）；槽不可写时静默。
void world_restore_hud_gate();

// 非阻塞横幅：原版 UIPlay_CallMapName 的精确参数 INSTANTMSGSYSTEM_Add(3,text,0,0,5,0x15,0,0)。
// 未解析返回 false。
bool world_show_banner(const char* text);

// choice 面板的 popup state id（扫描 g_sPopupStateList 取 enter==UIChoice_Init 的条目）；
// 未就绪/未命中返回 -1。仅用于日志与诊断。
int world_choice_state_id();

// ---- 世界写原语（T2l-w-write）----
//
// 迁移自 api/native 的世界写实现（teleport/teleport_direct/move/walk/walk_stop/interact）：
// 内部经 contracts/runtime_hooks.h 的过渡注入面使用 infra 帧任务/转场机制（game 层不 include
// infra/host）。返回完整 op JSON 信封（成功 `{"ok":true}` / 失败 `{"ok":false,"error":...}`），
// 与迁移前逐字一致；定长 POD（签名无 STL），由调用方（bridge wrapper/game_system）透传。
struct WorldWriteResult {
    uint32_t size = 0;
    char text[512] = {};

    const char* data() const { return text; }
    std::string str() const { return std::string(text, size); }
};

// 传送：blocking（transition_run 跨帧）与 direct（transition_run_inline，owner 逻辑相位内联）。
WorldWriteResult world_teleport(int32_t map_id, int32_t x, int32_t y, bool force);
WorldWriteResult world_teleport_direct(int32_t map_id, int32_t x, int32_t y, bool force);
// 寻路移动 / 方向走 60 帧 / 停走 / 交互事件检查（成功 refresh 恰一次，经 op_ok）。
WorldWriteResult world_move(int32_t x, int32_t y);
WorldWriteResult world_walk(int32_t direction);
WorldWriteResult world_walk_stop();
WorldWriteResult world_interact();

// ---- 世界传送入口 patch 元信息（UIPlay_CallMapName 尾部指令点）----

// patch_addr：待改写指令地址；return_addr：trampoline 跳回地址（epilogue）；
// expected_word：原版指令字 mov x0,#1；legacy_target：改版注入块目标（其等价格式由
// 调用方自行编码）。g_base 未就绪返回 false。
struct WorldTeleportPatch {
    uintptr_t patch_addr = 0;
    uintptr_t return_addr = 0;
    uint32_t expected_word = 0;
    uintptr_t legacy_target = 0;
};
bool world_teleport_patch_info(WorldTeleportPatch* out);

}  // namespace qol::game
