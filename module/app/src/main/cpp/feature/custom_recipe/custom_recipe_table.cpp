#include "custom_recipe_table.h"

#include <cstring>

namespace custom_recipe {

namespace {

inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

}  // namespace

void build_record_bytes(const Def& def, uint16_t material_start, uint8_t out[kRecipeRecordSize]) {
    std::memset(out, 0, kRecipeRecordSize);
    put_u16(out + kRbLabel, def.label_word_id);
    put_u16(out + kRbResultId, 0);  // 结果物品 id = 0（MakeItem 被 hook 拦截，§4.3）
    put_u16(out + kRbMaterialStart, material_start);
    out[kRbMaterialCount] = def.material_count;
    out[kRbFlag7] = kRbFlag7Value;
    put_u16(out + kRbCostWord, def.cost_word_id);
    out[kRbUnlockGate] = kRbUnlockGateValue;
    // 组位 + 配方书位（bit5）：后者是计数不变式所必需，见 custom_recipe_table.h 的 kRbRecipeBookBit 注释。
    out[kRbGroup] = static_cast<uint8_t>(recipe_group_bit(def.group) | kRbRecipeBookBit);
}

uint32_t derive_material_count(const uint8_t* recipe, uint16_t record_count, uint8_t record_size) {
    if (recipe == nullptr || record_size < kRbMaterialCount + 1) return 0;
    uint32_t max_end = 0;
    for (uint16_t i = 0; i < record_count; ++i) {
        const uint8_t* rec = recipe + static_cast<size_t>(i) * record_size;
        const uint32_t start = (record_size >= kRbMaterialStart + 2)
                                   ? get_u16(rec + kRbMaterialStart)
                                   : 0;
        const uint32_t count = rec[kRbMaterialCount];
        const uint32_t end = start + count;
        if (end > max_end) max_end = end;
    }
    return max_end;
}

uint32_t count_recipe_book_records(const uint8_t* recipe, uint16_t record_count,
                                   uint8_t record_size) {
    if (recipe == nullptr || record_size <= kRbGroup) return 0;
    uint32_t n = 0;
    for (uint16_t i = 0; i < record_count; ++i) {
        const uint8_t flags = recipe[static_cast<size_t>(i) * record_size + kRbGroup];
        if ((flags & kRbRecipeBookBit) != 0) ++n;
    }
    return n;
}

uint32_t inject_into_buffers(const uint8_t* orig_recipe, uint16_t base_record_count,
                             uint8_t recipe_size, const uint8_t* orig_mixture,
                             uint8_t mixture_size, const Def* cat, size_t n,
                             uint8_t* out_recipe, uint8_t* out_mixture) {
    if (out_recipe == nullptr || out_mixture == nullptr || recipe_size == 0 ||
        mixture_size == 0) {
        return base_record_count;
    }
    // 1) RECIPEBASE 前段复制，并把模块配方占用的组位从**原版记录**上清掉（只动 b11）：
    //    使模块注入记录成为该页（如 group 3 = 宝石强化页）的唯一拥有者——原版 12..15 的
    //    b11 由 0x08 变 0x00，不再从该页配方列表出现。其余字段（b0-3 / b4-9 / b10）必须
    //    原样保留：gemcraft 仍把 [+0x48] 改写为 12..15 走原版合成，MIXSYSTEM_MakeItem /
    //    CheckMixture 都按下标读记录，与组位无关。
    if (orig_recipe != nullptr) {
        std::memcpy(out_recipe, orig_recipe,
                    static_cast<size_t>(base_record_count) * recipe_size);
    }
    uint8_t module_group_bits = 0;
    for (size_t i = 0; i < n; ++i) {
        module_group_bits |= recipe_group_bit(cat[i].group);
    }
    if (orig_recipe != nullptr && module_group_bits != 0 && recipe_size >= kRbGroup + 1) {
        for (uint16_t r = 0; r < base_record_count; ++r) {
            uint8_t* rec = out_recipe + static_cast<size_t>(r) * recipe_size;
            rec[kRbGroup] = static_cast<uint8_t>(rec[kRbGroup] & ~module_group_bits);
        }
    }
    // 2) 原版材料尾下标（= 被引用的最大 b4-5 + b6）。
    const uint32_t base_material_count =
        derive_material_count(orig_recipe, base_record_count, recipe_size);
    // 3) MIXTUREBASE 前段原样复制。
    if (orig_mixture != nullptr) {
        std::memcpy(out_mixture, orig_mixture,
                    static_cast<size_t>(base_material_count) * mixture_size);
    }
    // 4) 逐配方追加记录 + 材料条目。
    for (size_t i = 0; i < n; ++i) {
        const uint16_t mix_material_start = static_cast<uint16_t>(
            material_start_at(static_cast<uint16_t>(base_material_count), cat, n, i));
        // 注入记录：recipe_size 可能 >= kRecipeRecordSize，先清整格再写 12B 有效字段。
        uint8_t* slot = out_recipe + (static_cast<size_t>(base_record_count) + i) * recipe_size;
        std::memset(slot, 0, recipe_size);
        uint8_t record[kRecipeRecordSize];
        build_record_bytes(cat[i], mix_material_start, record);
        const size_t copy = (recipe_size < kRecipeRecordSize) ? recipe_size : kRecipeRecordSize;
        std::memcpy(slot, record, copy);
        // 材料条目。
        for (uint8_t j = 0; j < cat[i].material_count; ++j) {
            uint8_t* mslot = out_mixture +
                             static_cast<size_t>(mix_material_start + j) * mixture_size;
            std::memset(mslot, 0, mixture_size);
            const uint16_t item_id = cat[i].materials[j].item_id;
            mslot[0] = static_cast<uint8_t>(item_id & 0xff);
            mslot[1] = static_cast<uint8_t>(item_id >> 8);
            mslot[2] = cat[i].materials[j].count;
        }
    }
    return static_cast<uint32_t>(base_record_count) + static_cast<uint32_t>(n);
}

}  // namespace custom_recipe

// ---------------------------------------------------------------------------
// Android 专属：读 .bss 表指针全局 → 分配模块自有缓冲 → 注入 → 写回字段全局。
// host 单测不编译本段（仅覆盖上方纯构造器），故不产生 game_access 链接依赖。
// ---------------------------------------------------------------------------
#ifdef __ANDROID__
#include <atomic>
#include <cstdlib>

#include "core/native/qol_log.h"
#include "game_access.h"
#include "game_symbols.h"

namespace custom_recipe {

namespace {

// 模块自有表缓冲。游戏若重新装载静态表会把指针全局改回原表，ensure_impl 会据此重新注入。
uint8_t* g_recipe_buffer = nullptr;
uint8_t* g_mixture_buffer = nullptr;
uint16_t g_base_record_count_cache = 0;
std::atomic<bool> g_busy{false};

// 读 .bss 全局。
inline uintptr_t bss(uintptr_t vma) { return g_base + vma; }

bool ensure_impl() {
    if (g_base == 0) return false;

    auto* recipe_ptr_global = reinterpret_cast<void**>(bss(G_RECIPEBASE_DATA_VMA));
    auto* recipe_size_global = reinterpret_cast<uint8_t*>(bss(G_RECIPEBASE_SIZE_VMA));
    auto* recipe_count_global = reinterpret_cast<uint16_t*>(bss(G_RECIPEBASE_COUNT_VMA));
    auto* mixture_ptr_global = reinterpret_cast<void**>(bss(G_MIXTUREBASE_DATA_VMA));
    auto* mixture_size_global = reinterpret_cast<uint8_t*>(bss(G_MIXTUREBASE_SIZE_VMA));

    // 1) 已注入且游戏侧指针仍指向模块缓冲 → 只需把记录数归位（可能被 deactivate 改回原值）。
    if (g_recipe_buffer != nullptr && *recipe_ptr_global == g_recipe_buffer) {
        size_t n = 0;
        catalog(&n);
        const uint16_t want = static_cast<uint16_t>(g_base_record_count_cache + n);
        if (*recipe_count_global != want) {
            *recipe_count_global = want;
            QOL_LOG_INFO(QolDomain::kCustomRecipe, "recipe table reactivated records=%u", want);
        }
        if (!catalog_ready()) bind_base_record_count(g_base_record_count_cache);
        return catalog_ready();
    }
    // 2) 游戏重新装载了静态表（指针被改写回原表）→ 用新原表重新注入。
    //    刻意不释放旧缓冲：游戏可能仍持有指向模块缓冲内部的局部指针，泄漏一份远优于悬空访问。
    if (g_recipe_buffer != nullptr) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe,
                     "recipe table replaced by game, re-inject count=%u", *recipe_count_global);
        g_recipe_buffer = nullptr;
        g_mixture_buffer = nullptr;
    }

    uint8_t* recipe_data = static_cast<uint8_t*>(*recipe_ptr_global);
    const uint8_t recipe_size = *recipe_size_global;
    uint8_t* mixture_data = static_cast<uint8_t*>(*mixture_ptr_global);
    const uint8_t mixture_size = *mixture_size_global;
    const uint16_t cur_count = *recipe_count_global;
    // fail-closed：这 5 个全局只能走 VMA 兜底（无名 .bss），跨版本可能错位。
    // 先用窄域校验挡住明显非法值——宁可跳过注入，也不要带着垃圾指针 memcpy 让游戏崩溃。
    constexpr uint8_t kMaxRecipeSize = 64;
    constexpr uint8_t kMaxMixtureSize = 16;
    constexpr uint16_t kMaxRecordCount = 4096;
    if (recipe_data == nullptr || mixture_data == nullptr || recipe_size == 0 ||
        recipe_size > kMaxRecipeSize || mixture_size == 0 || mixture_size > kMaxMixtureSize ||
        cur_count == 0 || cur_count > kMaxRecordCount) {
        QOL_LOG_ERROR(QolDomain::kCustomRecipe,
                      "table globals invalid recipe_size=%u mixture_size=%u count=%u",
                      recipe_size, mixture_size, cur_count);
        return false;
    }

    size_t n = 0;
    const Def* cat = catalog(&n);
    if (cat == nullptr || n == 0) return false;

    const uint16_t base_record_count = cur_count;
    // 书名册大小不变式：注入 N 条后 GetRecipeCount(5) 增加 N（每条注入记录都带 kRbRecipeBookBit），
    // 而书名册缓冲是游戏按**注入前**的 (count5+7)/8 分配的、存档位图长度也按它决定 → 必须相等。
    // 不相等时 fail-closed 拒绝注入：宁可模块配方不出现，也不要越界读写书名册或改存档格式长度。
    const uint32_t book_count = count_recipe_book_records(recipe_data, base_record_count, recipe_size);
    const uint32_t book_bytes_before = recipe_book_bytes(book_count);
    const uint32_t book_bytes_after = recipe_book_bytes(book_count + static_cast<uint32_t>(n));
    if (book_bytes_before != book_bytes_after) {
        QOL_LOG_ERROR(QolDomain::kCustomRecipe,
                      "recipe book byte size would change (%u -> %u, bit5=%u n=%u); injection "
                      "refused to avoid out-of-bounds book access and save-format length change",
                      book_bytes_before, book_bytes_after, book_count, static_cast<unsigned>(n));
        return false;
    }
    const uint32_t base_material_count =
        derive_material_count(recipe_data, base_record_count, recipe_size);
    const uint32_t material_total_count = material_total(cat, n);

    const size_t new_recipe_bytes =
        static_cast<size_t>(base_record_count + n) * recipe_size;
    const size_t new_mixture_bytes =
        static_cast<size_t>(base_material_count + material_total_count) * mixture_size;

    uint8_t* new_recipe = static_cast<uint8_t*>(std::malloc(new_recipe_bytes));
    uint8_t* new_mixture = static_cast<uint8_t*>(std::malloc(new_mixture_bytes));
    if (new_recipe == nullptr || new_mixture == nullptr) {
        std::free(new_recipe);
        std::free(new_mixture);
        QOL_LOG_ERROR(QolDomain::kCustomRecipe, "table alloc failed bytes=%zu/%zu",
                      new_recipe_bytes, new_mixture_bytes);
        return false;
    }

    inject_into_buffers(recipe_data, base_record_count, recipe_size, mixture_data, mixture_size,
                        cat, n, new_recipe, new_mixture);

    // 写回可写 .bss（§3.8：无需 mprotect）。
    *recipe_ptr_global = new_recipe;
    *mixture_ptr_global = new_mixture;
    *recipe_count_global = static_cast<uint16_t>(base_record_count + n);

    g_recipe_buffer = new_recipe;
    g_mixture_buffer = new_mixture;
    g_base_record_count_cache = base_record_count;
    bind_base_record_count(base_record_count);
    QOL_LOG_INFO(QolDomain::kCustomRecipe,
                 "recipe table injected base=%u records=%u materials=%u book_bytes=%u", base_record_count,
                 static_cast<unsigned>(base_record_count + n),
                 static_cast<unsigned>(base_material_count + material_total_count),
                 book_bytes_after);
    return true;
}

}  // namespace

bool custom_recipe_table_ensure() {
    // 只做互斥，不缓存「已尝试」状态：表未装载 / 被游戏重装都必须允许后续重试。
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        return g_recipe_buffer != nullptr;
    }
    const bool ok = ensure_impl();
    g_busy.store(false, std::memory_order_release);
    return ok;
}

void custom_recipe_table_deactivate() {
    if (g_base == 0 || g_recipe_buffer == nullptr) return;
    auto* recipe_ptr_global = reinterpret_cast<void**>(bss(G_RECIPEBASE_DATA_VMA));
    auto* recipe_count_global = reinterpret_cast<uint16_t*>(bss(G_RECIPEBASE_COUNT_VMA));
    // 指针已被游戏改写 → 注入本就已失效，清模块状态即可，绝不把原表指针写回（可能已陈旧）。
    if (*recipe_ptr_global != g_recipe_buffer) {
        g_recipe_buffer = nullptr;
        g_mixture_buffer = nullptr;
        return;
    }
    // 只收记录数，指向模块缓冲的指针保持不变：所有下标都仍在缓冲界内，
    // 面板残留数组里的注入下标不会造成越界读；查询不会再命中注入记录。
    *recipe_count_global = g_base_record_count_cache;
    QOL_LOG_INFO(QolDomain::kCustomRecipe, "recipe table deactivated records=%u",
                 g_base_record_count_cache);
}

}  // namespace custom_recipe
#endif  // __ANDROID__
