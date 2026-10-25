#include "game/adapter_ports/recipe_table_port.h"

#include "contracts/game_ports/ports.h"
#include "game/symbols/game_symbols.h"

// 与 game/access/game_access.h 同一声明；不引入该头以免 host 测试目标依赖 jni.h。
extern uintptr_t g_base;

// RECIPEBASE / MIXTUREBASE 槽位的唯一装配点：VMA 只在 game/ 出现（ARCHITECTURE.md §5），
// 插件经端口拿到装好的指针。base 未就绪时整组字段置 0，由插件表核心分类为 NotReady。
namespace qol::game {
namespace {

using Slots = contracts::QolRecipeTableSlots;

void resolve_slots(Slots* out) {
    if (out == nullptr) return;
    *out = Slots{};
    if (g_base == 0) return;
    out->base = g_base;
    out->recipe_ptr_slot = reinterpret_cast<void**>(g_base + G_RECIPEBASE_DATA_VMA);
    out->mixture_ptr_slot = reinterpret_cast<void**>(g_base + G_MIXTUREBASE_DATA_VMA);
    out->recipe_size_slot = reinterpret_cast<uint8_t*>(g_base + G_RECIPEBASE_SIZE_VMA);
    out->mixture_size_slot = reinterpret_cast<uint8_t*>(g_base + G_MIXTUREBASE_SIZE_VMA);
    out->recipe_count_slot = reinterpret_cast<uint16_t*>(g_base + G_RECIPEBASE_COUNT_VMA);
}

}  // namespace

const contracts::QolRecipeTablePort& recipe_table_port() {
    static const contracts::QolRecipeTablePort port{
        contracts::kQolPortRecipeTableVersion, &resolve_slots};
    return port;
}

}  // namespace qol::game
