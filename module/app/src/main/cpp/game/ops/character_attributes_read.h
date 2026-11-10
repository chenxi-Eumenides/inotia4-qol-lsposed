#pragma once

#include <cstdint>

#include "contracts/character_attributes.h"

namespace qol::game::ops {

// 角色引用合法性：party 0..2 / mercenary 0..20。
bool character_ref_valid(const contracts::QolCharacterRef& ref);

// 只查活 CHAR（party/mercenary 槽），不物化、不读存档；要求游戏主线程。
bool character_attributes_present(const contracts::QolCharacterRef& ref);

// 活 CHAR 优先；佣兵槽未命中时按当前存档槽调用 SAVE_LoadCharacterEx 物化。
// 返回 nullptr = 无角色/物化失败/非主线程。
void* resolve_character(const contracts::QolCharacterRef& ref);

// attr 单槽读取（0..114）；要求游戏主线程。
bool read_character_attr_value(const contracts::QolCharacterRef& ref, int32_t attr_id,
                               int32_t* out_value);
// 主属性（0..4，Base+Main+Bonus+Sub）。
bool read_character_main_stat_value(const contracts::QolCharacterRef& ref, int32_t index,
                                    int32_t* out_value);

// 全量快照：main_stats 5 + attrs 115 + attr_valid 115。
bool read_character_attribute_snapshot(const contracts::QolCharacterRef& ref,
                                       contracts::QolCharacterAttributeSnapshot* out);

// 面板派生值（QolCharacterDerivedKind）。
bool read_character_derived(const contracts::QolCharacterRef& ref, int32_t kind,
                            int32_t* out_value);

}  // namespace qol::game::ops
