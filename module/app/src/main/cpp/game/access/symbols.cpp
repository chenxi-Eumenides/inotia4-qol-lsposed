#include "game/access/symbols.h"

#include <cstring>

#include "data/native/game_symbols.h"
#include "game_access.h"

namespace qol::game {
namespace {

// 声明符号名 → 常量唯一来源（game_symbols.h）。运行时经 fn_resolve 优先按 .dynsym
// 符号名解析，未命中回退 VMA。
struct HookEntrySymbol {
    const char* name;
    uintptr_t vma;
};

constexpr HookEntrySymbol kHookEntrySymbols[] = {
    {"F_CHAR_ADD_DAMAGE_VMA", F_CHAR_ADD_DAMAGE_VMA},
    {"F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA", F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA},
    // 属性显示范围：详情渲染三点（MATH_GetRandom 截获 / 选项行改写 / 构造入口缓存）。
    {"F_MATH_GET_RANDOM_VMA", F_MATH_GET_RANDOM_VMA},
    {"F_UIDESC_ADD_OPTION_VMA", F_UIDESC_ADD_OPTION_VMA},
    {"F_UIDESC_MAKE_ITEM_VMA", F_UIDESC_MAKE_ITEM_VMA},
    // 文本层：全游戏唯一文本读取热点（共享资源，owner=text_layer）。
    {"F_MEMORYTEXT_GET_TEXT_VMA", F_MEMORYTEXT_GET_TEXT_VMA},
};

// 解析可安装 hook 的入口地址。
// monster 全系把 CHAR_AddDamage 入口改写成无条件 b 跳板（v20 `b 0x14e280`；v23-v27 `b 0x7450b8`，
// 落于第二可执行段）。跳板入口先 `stp x0..x8` 保存 x0-x7 再修改寄存器，故跳板目标处 ABI 与函数
// 入口完全一致，在目标处 hook 与原位等价；而 inline hook 若需重定位一条 26 位 b，跨 ±128MB 的
// 偏移会失效。因此这里主动跟随一次，目标必须 4 字节对齐且落在已加载的可执行映射内。
uintptr_t hookable_entry(uintptr_t symbol_entry, bool* out_followed) {
    *out_followed = false;
    if (symbol_entry == 0 || !game_memory_accessible(reinterpret_cast<void*>(symbol_entry), 4, 'r')) {
        return symbol_entry;
    }
    const uint32_t word = *reinterpret_cast<const uint32_t*>(symbol_entry);
    constexpr uint32_t k_branch_mask = 0xFC000000u;   // b/bl/ret 等 opcode 掩码
    constexpr uint32_t k_b_opcode = 0x14000000u;      // B（无条件、不写 x30）
    if ((word & k_branch_mask) != k_b_opcode) return symbol_entry;

    int64_t offset = static_cast<int64_t>(word & 0x03FFFFFFu);
    if ((offset & 0x02000000) != 0) offset -= 0x04000000;  // 26 位有符号立即数
    const uintptr_t target = symbol_entry + static_cast<uintptr_t>(offset * 4);
    if ((target & 0x3u) != 0) return symbol_entry;
    if (!game_memory_accessible(reinterpret_cast<void*>(target), 8, 'x')) return symbol_entry;
    *out_followed = true;
    return target;
}

}  // namespace

uintptr_t resolve_hook_entry(const char* symbol_name, bool* out_followed) {
    // hookable_entry 始终解引用 out_followed；调用方允许传 nullptr 时用局部量承接。
    bool followed_local = false;
    bool* followed = out_followed != nullptr ? out_followed : &followed_local;
    if (symbol_name == nullptr || g_base == 0) {
        *followed = false;
        return 0;
    }
    for (const HookEntrySymbol& entry : kHookEntrySymbols) {
        if (std::strcmp(entry.name, symbol_name) != 0) continue;
        const uintptr_t symbol_entry = g_base + fn_resolve(entry.name, entry.vma);
        return hookable_entry(symbol_entry, followed);
    }
    *followed = false;
    return 0;
}

}  // namespace qol::game
