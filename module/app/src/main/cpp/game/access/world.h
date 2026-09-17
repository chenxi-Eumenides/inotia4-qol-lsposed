#pragma once

#include <cstdint>

#include "data/native/game_symbols.h"

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
