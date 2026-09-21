#pragma once

#include <cstdint>

// 游戏状态层：全局状态检测 + 跨域查询原语 + 跨域遍历原语（无构建/导航依赖）。

bool game_in_world();
int current_save_slot();
const char* ui_blocked();
int tutorial_state();
void tutorial_cancel();
const char* tutorial_block_error();

void* member_or_null(int role);
void* lead_member();
void* find_char_by_merc_slot(int slot);

// HP/MP 上限缓存读取（数据层原语）：直接读 [ch+C_MAX_HP]/[ch+C_MAX_MP]（属性数组 attr 0x1e/0x1f
// 的缓存槽），不调用 CHAR_GetAttr——其 attr=0x1e 分支在 HP>maxHP 时会写回 HP，离线程调用会与
// 游戏主线程属性重算竞争并永久钳低角色 HP。
// 兜底：缓存值 <= 0（世界未就绪/属性失效）时回退当前 C_HP/C_MP，保证输出数值合理。
int32_t char_max_hp(const void* ch);
int32_t char_max_mp(const void* ch);

// 主属性总属性（数据层原语）：直读 [ch+C_STAT_BASE]/[ch+C_STAT_MAIN]/[ch+C_STAT_BONUS]/[ch+C_STAT_SUB]
// 四项求和，与 CHAR_GetStat(0xdf8d0)（Base+Main+Bonus+Sub，无 clamp）返回值等价。
// 不调用 CHAR_GetStat：它经 CHAR_GetStatSub(0xdf888) 在动态派生脏位 C_STAT_CALC_FLAG 置位时会
// 调 CHAR_CalculateStatus 重算并写回 [ch+0x266]/SV，属写操作，非游戏线程调用会与主线程竞争。
// 代价：脏位刚置位、游戏尚未重算时，sub 为上一次缓存值（避免离线程写回的取舍）。
// 兜底：ch 为空或 index 越界（合法 0..4，共 5 项主属性）返回 0——游戏函数无越界检查，
// 越界会读到相邻字段；此处以 0 明确表达「非法索引无意义」。
int32_t char_stat_total(const void* ch, int index);

// 升级所需经验（next_exp）游戏线程帧缓存。
// CHAR_GetNextExperience(0xd9b68) 读 [ch+0x320]：非 0 直接返回；为 0 时用 CAL_Calculate 按等级公式
// 现算并写回 [ch+0x320]，CHAR_SetLevel(0xe05a0) 会把该缓存清零失效。故 [ch+0x320] 不是恒等直读字段
// （刚升完级或新角色时为 0），且该函数非纯读、不能在预取/HTTP 线程调用。
// 这里在游戏主线程每逻辑帧对 3 名队员调用一次并缓存，JSON 只读缓存；未被帧任务覆盖（尚未进 world
// / 未安装帧宿主）时回退直读 [ch+0x320]（游戏自身缓存）。
int64_t char_next_exp_cached(const void* ch);

// 注册 next_exp 游戏线程帧任务（幂等）。由 nativeInit 在 bridge/帧宿主就绪后调用一次。
void char_next_exp_cache_start();

enum class InventoryItemKind : uint8_t {
    kOriginal,
    kExtension,
};

struct InventoryItemRef {
    InventoryItemKind kind = InventoryItemKind::kOriginal;
    int bag = -1;
    int slot = -1;
    int category = 0;
    int count = 0;
    void* native_item = nullptr;
};

using InventoryItemFn = bool (*)(const InventoryItemRef& item, void* ctx);

int inventory_count();
int inventory_quantity(int category);
void* find_inventory_item(int category);
void* inventory_item_at(int bag, int slot);
bool find_inventory_item_ref(int category, InventoryItemRef* out);
bool inventory_item_ref_at(int bag, int slot, InventoryItemRef* out);
void for_each_inventory_item(InventoryItemFn fn, void* ctx);

using BagSlotFn = bool (*)(void* item, int bag, int slot, void* ctx);
void for_each_bag_slot(BagSlotFn fn, void* ctx);
bool pool_obj_valid(const uint8_t* obj);
