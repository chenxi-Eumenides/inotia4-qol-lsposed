#pragma once

#include <cstdint>

// 游戏层：存档域语义化访问（ARCHITECTURE.md §5）。
// 存档槽结构偏移（SAVESLOT_*）、加解密链函数指针与删档回调 GOT 槽地址收口在本层；
// 上层只调用语义函数，不出现裸地址/裸偏移、不直接 include 符号头。
//
// 线程纪律：save_slot_* 与 save_hero_* 只应在游戏主线程调用（依赖槽结构全局）；
// save_load_data/encrypt_process2 为存档文件加解密原语，调用方自行保证线程与锁。
// 未就绪/未解析一律按各函数返回值给「空/失败」，不解引用空指针。

namespace qol::game {

// ---- 存档槽结构（SAVESLOT_*）----

// 槽结构元数据依赖（SAVE_GetSaveSlot / SAVESLOT_GetHero / SAVE_DestroySaveSlot /
// SAVE_CreateSaveSlot 四个函数指针）是否全部解析；与迁移前 read_hero_meta 守卫一致。
bool save_slot_meta_deps_ready();

// 常规读取入口：主菜单停留内尚未重载或槽文件被改动时重载三槽（策略见 game_access.h）。
void save_slots_refresh_if_stale();
// 模块或游戏改动槽文件后调用，令下一次读取重新重载。
void save_slots_mark_stale();

// 槽结构指针；未解析返回 nullptr。
void* save_slot_struct(int32_t slot);
// 槽结构 +0 SAVESLOT_EXISTS（u8 非 0 表示存在）。
bool save_slot_exists(const void* slot_struct);
// 槽结构 +0 SAVESLOT_MAP_ID（u16）。
int save_slot_map_id(const void* slot_struct);
// 槽结构 +SAVESLOT_HERO_INDEX（s8）。
int save_slot_hero_index(const void* slot_struct);
// 槽内主角对象；未解析返回 nullptr。
void* save_slot_hero(void* slot_struct);
// hero + C_LEVEL / C_CLASS（s8 零扩展口径，与迁移前 static_cast<int8_t> 一致）。
int save_hero_level(const void* hero);
int save_hero_class(const void* hero);

// ---- 存档文件加解密链 ----

// SAVE_LoadData 与 MEM_Free 是否都已解析。
bool save_load_deps_ready();
// SAVE_LoadData(slot, &buf, &len) 原值（1=成功）；成功得到的 buf 须经 save_mem_free 归还。
int save_load_data(int32_t slot, void** out_buf, int* out_len);
void save_mem_free(void* buf);
// 当前设备密钥（HUB_Save_GetKey）；未解析/不可得返回 nullptr。
const char* save_hub_get_key();
// ENCRYPT_Process2 是否可用。
bool save_encrypt_available();
// ENCRYPT_Process2(buf, len, mode, key) 原值（mode=0 加密就地写 len+3，mode=1 解密返回 1 成功）。
int save_encrypt_process2(void* buf, int len, int mode, const char* key);

// ---- 删档回调（SaveSlot_Delete）PtrHook 目标 ----

// 解析 F_SAVESLOT_DELETE_VMA 与 G_SAVESLOT_DELETE_GOT_VMA：
// out_slot 为 GOT 槽地址，out_expected 为安装前应存的 SaveSlot_Delete 入口地址。
// 基址未就绪或符号未解析返回 false。
bool save_slot_delete_hook_target(void** out_slot, uintptr_t* out_expected);

}  // namespace qol::game
