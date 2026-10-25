#pragma once

// 游戏侧 RECIPEBASE / MIXTUREBASE 的 .bss 槽位端口。
//
// 槽位地址只在 game/ 的 adapter 实现中由 VMA 装配（ARCHITECTURE.md §5）；插件只接收装好的
// 指针，不持有 base 偏移或裸地址。本端口只表达「表在哪里」，注入算法仍归插件表核心。

#include <cstdint>
#include <type_traits>

#include "../version.h"

namespace qol::contracts {

// 字段语义与插件内部 TableAccess 一一对应：base 为模块基址，其余为各字段的槽位地址。
struct QolRecipeTableSlots {
    uintptr_t base;
    void** recipe_ptr_slot;
    void** mixture_ptr_slot;
    uint8_t* recipe_size_slot;
    uint8_t* mixture_size_slot;
    uint16_t* recipe_count_slot;
};

static_assert(std::is_standard_layout<QolRecipeTableSlots>::value,
              "QolRecipeTableSlots must remain standard layout");
static_assert(std::is_trivially_copyable<QolRecipeTableSlots>::value,
              "QolRecipeTableSlots must remain trivially copyable");

struct QolRecipeTablePort {
    QolVersion abi_version;
    // 全有或全无：base 未就绪时把 6 个字段全部置 0；否则 6 个字段必须同时有效。
    void (*resolve)(QolRecipeTableSlots* out);
};

static_assert(std::is_standard_layout<QolRecipeTablePort>::value,
              "QolRecipeTablePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolRecipeTablePort>::value,
              "QolRecipeTablePort must remain trivially copyable");

}  // namespace qol::contracts
