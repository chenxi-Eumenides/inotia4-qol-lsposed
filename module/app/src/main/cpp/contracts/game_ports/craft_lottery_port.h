#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

struct QolCraftLotteryWritePort {
    QolVersion abi_version;
    // 池：use_type 在 ITEMMIXLINKBASE 中匹配的全部 RECIPEBASE 下标（原版 DecideRecipeID 同款全表扫描）
    QolResult (*pool_read)(uint32_t use_type, uint16_t* out_recipe_idx, uint32_t cap,
                           uint32_t* out_count);
    // 配方书位图判定（位偏移 = recipe_idx + GetRecipeCount(5) - GetRecipeCount(0)，Game Adapter 内完成）
    QolResult (*recipe_learned)(uint16_t recipe_idx, bool* out_learned);
    // 原版纯随机决策（ITEMSYSTEM_DecideRecipeID），负值 = 池空
    QolResult (*decide_recipe_pure)(uint32_t use_type, int32_t* out_recipe_idx);
    // 原版 MIXSYSTEM_AddRecipeBook
    QolResult (*add_recipe_book)(uint16_t recipe_idx);
    // 原版 MATH_GetRandom(n)
    QolResult (*get_random)(int32_t n, int32_t* out);
    // 弹窗组合：第一行保持原版不动——fmt=MEM_ReadUint16(RECIPEBASE_data + recsize*17) 经
    // MEMORYTEXT_GetText，name=RECIPEBASE[recipe_idx] 首 u16 经 MEMORYTEXT_GetText，
    // CS_knlSprintk(buf,fmt,name)。第二行追加 "&N" + ITEMDATABASE[use_type] 首 u16 文本
    // （套装名，文本 id = 物品 id）+ extra_utf8（如「已获得：3/5」，追加失败静默截断），
    // 之后 UIPopupMsg_CreateOK(buf,2,0,0,0)，MEM_Free。
    QolResult (*show_recipe_popup)(uint16_t recipe_idx, uint32_t use_type, const char* extra_utf8);
};

static_assert(std::is_standard_layout<QolCraftLotteryWritePort>::value,
              "QolCraftLotteryWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCraftLotteryWritePort>::value,
              "QolCraftLotteryWritePort must remain trivially copyable");

}  // namespace qol::contracts
