#pragma once

#include <cstdint>
#include <string>

// 缓存层：读取已发布快照；builders 只在逻辑帧任务或 scheduler LogicPre 回调内执行。

// host↔infra 内部注入面（A1 形态）：文档化的 slot 与 CacheSlot 表一致；仅 host 装配期调用。
// 只接收轻量函数指针，不反向依赖 host（infra 不 include host）。
void frame_cache_set_json_builder(int32_t slot, std::string (*fn)());

// host↔infra 内部注入面（A1 形态）：world 判定由 host 装配期注入（game 层事实）。
// 尚未由安全逻辑帧采样时，world_only 槽返回 `{"error":"not ready"}`；确认非 world 后返回
// `{"error":"not in game"}`。gamestate/snapshot（world_only=false）不受该门禁影响。
void frame_cache_set_world_gate(bool (*fn)());

// host 帧源注入主线程判定；未注入时所有调用方都按非主线程处理。
void frame_cache_set_main_thread_gate(bool (*fn)());

void frame_cache_start();
void frame_cache_stop();
// 标记已请求的槽待刷新；后续逻辑帧按固定预算处理，安全逻辑相位内的目标槽读取可单次同步刷新。
void frame_cache_force_refresh();
bool frame_cache_ready();

std::string data_player_json();
std::string data_party_json();
std::string data_inventory_json();
std::string data_map_json();
std::string data_units_json();
std::string data_gamestate_json();
std::string data_snapshot_json();
std::string data_skills_json();
std::string data_mercenaries_json();
std::string data_drops_json();
std::string data_enemies_json();
std::string data_interactives_json();
