#include "table.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "rules.h"

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
    // 组位 + 配方书位（bit5）：后者是计数不变式所必需，见 table.h 的 kRbRecipeBookBit 注释。
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

uint8_t module_group_bits(const Def* cat, size_t n) {
    if (cat == nullptr) return 0;
    uint8_t bits = 0;
    for (size_t i = 0; i < n; ++i) bits |= recipe_group_bit(cat[i].group);
    return bits;
}

void restore_original_group_bits(uint8_t* buffer, uint16_t base_record_count, uint8_t recipe_size,
                                 const uint8_t* original_group_bits) {
    if (buffer == nullptr || original_group_bits == nullptr || recipe_size <= kRbGroup) return;
    for (uint16_t r = 0; r < base_record_count; ++r) {
        buffer[static_cast<size_t>(r) * recipe_size + kRbGroup] = original_group_bits[r];
    }
}

void clear_original_group_bits(uint8_t* buffer, uint16_t base_record_count, uint8_t recipe_size,
                               const uint8_t* original_group_bits, uint8_t module_bits) {
    if (buffer == nullptr || original_group_bits == nullptr || recipe_size <= kRbGroup) return;
    for (uint16_t r = 0; r < base_record_count; ++r) {
        uint8_t* slot = buffer + static_cast<size_t>(r) * recipe_size + kRbGroup;
        // 以快照为基（幂等：重复清除不累积），与 inject_into_buffers 的清位语义一致。
        *slot = static_cast<uint8_t>(original_group_bits[r] & ~module_bits);
    }
}

}  // namespace custom_recipe

// ---------------------------------------------------------------------------
// acquire / deactivate 便携核心（Android 入口与 host 测试共用同一实现）
// ---------------------------------------------------------------------------
namespace custom_recipe {

namespace {

// 定长格式化后交给 log_fn；log_fn 为 nullptr 时不记录。
void core_log(TableLogFn log_fn, int32_t level, const char* format, ...) {
    if (log_fn == nullptr || format == nullptr) return;
    char message[qol::contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length <= 0) return;
    log_fn(level, message);
}

void reset_log_dedupe(TableLogDedupe* dedupe) {
    if (dedupe == nullptr) return;
    dedupe->last_recipe_size = 0xFF;
    dedupe->last_mixture_size = 0xFF;
    dedupe->last_count = 0xFFFF;
}

// 注入前抓原版段 b11 快照；分配失败时清空快照状态（调用方按 Internal 处理本次注入）。
bool capture_original_group_bits(TableState* state, const uint8_t* orig_recipe,
                                 uint16_t base_count, uint8_t recipe_size, TableAllocFn alloc,
                                 TableFreeFn release) {
    if (state == nullptr || orig_recipe == nullptr || recipe_size <= kRbGroup || base_count == 0) {
        return true;
    }
    if (state->original_group_bits == nullptr || state->original_group_count < base_count ||
        state->original_group_recipe_size != recipe_size) {
        if (release != nullptr) release(state->original_group_bits);
        state->original_group_bits =
            alloc != nullptr ? static_cast<uint8_t*>(alloc(base_count)) : nullptr;
        if (state->original_group_bits == nullptr) {
            state->original_group_count = 0;
            state->original_group_recipe_size = 0;
            return false;
        }
        state->original_group_count = base_count;
        state->original_group_recipe_size = recipe_size;
    }
    for (uint16_t r = 0; r < base_count; ++r) {
        state->original_group_bits[r] =
            orig_recipe[static_cast<size_t>(r) * recipe_size + kRbGroup];
    }
    return true;
}

}  // namespace

QolResult table_status_result(TableAcquireStatus status) {
    // 消息为 TU 内静态字面量，长度用 sizeof-1 精确取得（不写死、不猜）。
    static constexpr char kNotReadyMessage[] = "table not loaded";
    static constexpr char kUnsupportedMessage[] = "table layout unsupported";
    static constexpr char kInternalMessage[] = "table internal error";
    static constexpr char kBusyMessage[] = "table acquire busy";
    switch (status) {
        case TableAcquireStatus::kOk:
            return qol::contracts::ok();
        case TableAcquireStatus::kNotReady:
            return qol::contracts::error(kQolErrNotReady, kNotReadyMessage,
                                         sizeof(kNotReadyMessage) - 1);
        case TableAcquireStatus::kUnsupported:
            return qol::contracts::error(kQolErrUnsupported, kUnsupportedMessage,
                                         sizeof(kUnsupportedMessage) - 1);
        case TableAcquireStatus::kInternal:
            return qol::contracts::error(kQolErrInternal, kInternalMessage,
                                         sizeof(kInternalMessage) - 1);
        case TableAcquireStatus::kBusy:
            return qol::contracts::error(kQolErrBusy, kBusyMessage, sizeof(kBusyMessage) - 1);
    }
    return qol::contracts::error(kQolErrInternal, kInternalMessage,
                                 sizeof(kInternalMessage) - 1);
}

TableAcquireStatus classify_table_probe(const TableProbe& probe) {
    // ① 非零但越界的布局优先判永久错误：不得因另一字段为 0 被当作「等待加载」。
    if (probe.recipe_size > kMaxRecipeRecordSize || probe.mixture_size > kMaxMixtureRecordSize ||
        probe.record_count > kMaxTableRecordCount) {
        return TableAcquireStatus::kUnsupported;
    }
    // ② 未加载：base / 指针 / count 缺失，或尺寸为 0。
    if (!probe.base_ready || !probe.recipe_ptr_present || !probe.mixture_ptr_present ||
        probe.record_count == 0 || probe.recipe_size == 0 || probe.mixture_size == 0) {
        return TableAcquireStatus::kNotReady;
    }
    // ③ 目录不可用：配置/目录错误，不是等着游戏加载表。
    if (!probe.catalog_available) return TableAcquireStatus::kUnsupported;
    return TableAcquireStatus::kOk;
}

TableAcquireStatus table_acquire_core(const TableAccess& access, TableState* state,
                                      const Def* catalog, size_t catalog_len,
                                      TableAllocFn alloc, TableFreeFn release, TableLogFn log_fn,
                                      TableLogDedupe* dedupe) {
    if (state == nullptr || access.recipe_ptr_slot == nullptr ||
        access.mixture_ptr_slot == nullptr || access.recipe_size_slot == nullptr ||
        access.mixture_size_slot == nullptr || access.recipe_count_slot == nullptr ||
        alloc == nullptr) {
        return TableAcquireStatus::kInternal;
    }
    // 重入判定与置位/复位都在核心内完成（RAII guard）：调用方不得提前置位，
    // 否则真实入口会自我拒绝；注入的 alloc/log 回调内重入也会正确得到 Busy。
    if (state->busy) return TableAcquireStatus::kBusy;
    struct BusyGuard {
        TableState* state;
        ~BusyGuard() { state->busy = false; }
    } busy_guard{state};
    state->busy = true;

    // 1) 先分类探测值：缺失字段 → NotReady、永久非法布局/目录 → Unsupported。
    //    **分类在任何模块状态改动之前**：不得让非法 probe 通过「已有 buffer」捷径判 OK，
    //    也不得在拒绝路径上清空/改写模块状态。
    TableProbe probe{};
    probe.base_ready = access.base != 0;
    probe.recipe_ptr_present = *access.recipe_ptr_slot != nullptr;
    probe.mixture_ptr_present = *access.mixture_ptr_slot != nullptr;
    probe.recipe_size = *access.recipe_size_slot;
    probe.mixture_size = *access.mixture_size_slot;
    probe.record_count = *access.recipe_count_slot;
    probe.catalog_available = catalog != nullptr && catalog_len > 0;

    const TableAcquireStatus verdict = classify_table_probe(probe);
    if (verdict != TableAcquireStatus::kOk) {
        if (verdict == TableAcquireStatus::kNotReady && dedupe != nullptr &&
            table_invalid_log_needed(probe.recipe_size, probe.mixture_size, probe.record_count,
                                     dedupe->last_recipe_size, dedupe->last_mixture_size,
                                     dedupe->last_count)) {
            dedupe->last_recipe_size = probe.recipe_size;
            dedupe->last_mixture_size = probe.mixture_size;
            dedupe->last_count = probe.record_count;
            core_log(log_fn, 2, "recipe table not ready recipe_size=%u mixture_size=%u count=%u",
                     probe.recipe_size, probe.mixture_size, probe.record_count);
        }
        return verdict;
    }

    // 2) 合法重新激活：已注入且游戏侧指针仍指向模块缓冲 → 记录数归位 + 重新清除原版段组位。
    if (state->recipe_buffer != nullptr && *access.recipe_ptr_slot == state->recipe_buffer) {
        const uint16_t want =
            static_cast<uint16_t>(state->base_record_count_cache + catalog_len);
        if (*access.recipe_count_slot != want) {
            *access.recipe_count_slot = want;
            core_log(log_fn, 1, "recipe table reactivated records=%u", want);
        }
        if (state->original_group_recipe_size == *access.recipe_size_slot &&
            state->original_group_bits != nullptr) {
            clear_original_group_bits(state->recipe_buffer, state->base_record_count_cache,
                                      *access.recipe_size_slot, state->original_group_bits,
                                      module_group_bits(catalog, catalog_len));
        }
        if (!catalog_ready()) bind_base_record_count(state->base_record_count_cache);
        reset_log_dedupe(dedupe);
        return catalog_ready() ? TableAcquireStatus::kOk : TableAcquireStatus::kInternal;
    }

    // 3) 游戏重新装载了静态表（指针被改写回原表）→ 用新原表重新注入。
    //    刻意不释放旧缓冲：游戏可能仍持有指向模块缓冲内部的局部指针，泄漏一份远优于悬空访问。
    if (state->recipe_buffer != nullptr) {
        core_log(log_fn, 2, "recipe table replaced by game, re-inject count=%u",
                 *access.recipe_count_slot);
        state->recipe_buffer = nullptr;
        state->mixture_buffer = nullptr;
    }

    const uint8_t* recipe_data = static_cast<const uint8_t*>(*access.recipe_ptr_slot);
    const uint8_t* mixture_data = static_cast<const uint8_t*>(*access.mixture_ptr_slot);
    const uint8_t recipe_size = probe.recipe_size;
    const uint8_t mixture_size = probe.mixture_size;
    const uint16_t base_record_count = probe.record_count;

    // 书名册大小不变式：注入 N 条后 (count5+7)/8 必须不变，否则永久拒绝注入
    // （书名册缓冲按注入前大小分配、存档位图长度按它动态）。
    // 先做永久性校验再改动任何模块状态，避免拒绝路径留下未被注入对应的快照副作用。
    const uint32_t book_count =
        count_recipe_book_records(recipe_data, base_record_count, recipe_size);
    const uint32_t book_bytes_before = recipe_book_bytes(book_count);
    const uint32_t book_bytes_after =
        recipe_book_bytes(book_count + static_cast<uint32_t>(catalog_len));
    if (book_bytes_before != book_bytes_after) {
        core_log(log_fn, 3,
                 "recipe book byte size would change (%u -> %u, bit5=%u n=%u); injection refused",
                 book_bytes_before, book_bytes_after, book_count,
                 static_cast<unsigned>(catalog_len));
        return TableAcquireStatus::kUnsupported;
    }

    // 校验通过后才抓原版段 b11 快照（停用时逐字节还原）。
    if (!capture_original_group_bits(state, recipe_data, base_record_count, recipe_size, alloc,
                                     release)) {
        return TableAcquireStatus::kInternal;
    }

    const uint32_t base_material_count =
        derive_material_count(recipe_data, base_record_count, recipe_size);
    const uint32_t material_total_count = material_total(catalog, catalog_len);
    const size_t new_recipe_bytes = static_cast<size_t>(base_record_count + catalog_len) * recipe_size;
    const size_t new_mixture_bytes =
        static_cast<size_t>(base_material_count + material_total_count) * mixture_size;

    uint8_t* new_recipe = static_cast<uint8_t*>(alloc(new_recipe_bytes));
    // 第一块失败时不再申请第二块（不浪费分配，也不留下半注入状态）。
    uint8_t* new_mixture =
        new_recipe != nullptr ? static_cast<uint8_t*>(alloc(new_mixture_bytes)) : nullptr;
    if (new_recipe == nullptr || new_mixture == nullptr) {
        if (release != nullptr) {
            if (new_recipe != nullptr) release(new_recipe);
            if (new_mixture != nullptr) release(new_mixture);
        }
        core_log(log_fn, 3, "table alloc failed bytes=%zu/%zu", new_recipe_bytes,
                 new_mixture_bytes);
        return TableAcquireStatus::kInternal;
    }

    inject_into_buffers(recipe_data, base_record_count, recipe_size, mixture_data, mixture_size,
                        catalog, catalog_len, new_recipe, new_mixture);

    *access.recipe_ptr_slot = new_recipe;
    *access.mixture_ptr_slot = new_mixture;
    *access.recipe_count_slot = static_cast<uint16_t>(base_record_count + catalog_len);

    state->recipe_buffer = new_recipe;
    state->mixture_buffer = new_mixture;
    state->base_record_count_cache = base_record_count;
    bind_base_record_count(base_record_count);
    core_log(log_fn, 1, "recipe table injected base=%u records=%u materials=%u book_bytes=%u",
             base_record_count, static_cast<unsigned>(base_record_count + catalog_len),
             static_cast<unsigned>(base_material_count + material_total_count),
             book_bytes_after);
    reset_log_dedupe(dedupe);
    return TableAcquireStatus::kOk;
}

TableAcquireStatus table_deactivate_core(const TableAccess& access, TableState* state) {
    if (state == nullptr || access.recipe_ptr_slot == nullptr ||
        access.recipe_size_slot == nullptr || access.recipe_count_slot == nullptr) {
        return TableAcquireStatus::kInternal;
    }
    if (access.base == 0 || state->recipe_buffer == nullptr) return TableAcquireStatus::kOk;
    // 指针已被游戏改写 → 注入本就失效：只清模块状态，绝不把（可能陈旧的）原表指针写回。
    if (*access.recipe_ptr_slot != state->recipe_buffer) {
        state->recipe_buffer = nullptr;
        state->mixture_buffer = nullptr;
        return TableAcquireStatus::kOk;
    }
    // 还原原版段组位（撤销 inject 的清位）→ 各页面回到原版配方列表。
    if (state->original_group_recipe_size == *access.recipe_size_slot &&
        state->original_group_bits != nullptr) {
        restore_original_group_bits(state->recipe_buffer, state->base_record_count_cache,
                                    *access.recipe_size_slot, state->original_group_bits);
    }
    // 只收记录数，指向模块缓冲的指针保持不变：所有下标仍在缓冲界内（面板残留数组安全）。
    *access.recipe_count_slot = state->base_record_count_cache;
    return TableAcquireStatus::kOk;
}

}  // namespace custom_recipe

// ---------------------------------------------------------------------------
// Android 专属入口：把真实 .bss 槽位装配成 TableAccess 后调用便携核心。
// host 单测不编译本段（仅使用上面的核心与纯构造器），故不产生 game_access 链接依赖。
// ---------------------------------------------------------------------------
#ifdef __ANDROID__
#include <cstdlib>

extern uintptr_t g_base;

namespace custom_recipe {

namespace {

constexpr uintptr_t kRecipeDataSlot = 3152304;
constexpr uintptr_t kRecipeSizeSlot = 3152312;
constexpr uintptr_t kRecipeCountSlot = 3152314;
constexpr uintptr_t kMixtureDataSlot = 3152288;
constexpr uintptr_t kMixtureSizeSlot = 3152296;

const qol::contracts::QolGameHost* g_game = nullptr;
const qol::contracts::QolHostSdk* g_sdk = nullptr;

// 模块侧状态与日志去重（本 TU 单一实例；核心按状态机处理重入与快照）。
TableState g_state;
TableLogDedupe g_dedupe;

void table_log_line(int32_t level, const char* message) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || message == nullptr) return;
    const size_t length = std::strlen(message);
    if (length == 0) return;
    const uint32_t size = length >= qol::contracts::kMessageCap
                              ? qol::contracts::kMessageCap
                              : static_cast<uint32_t>(length);
    (void)g_sdk->log.write(level, 15, "plugins/custom_recipe/table", message, size);
}

// 读 .bss 全局。
inline uintptr_t bss(uintptr_t vma) { return ::g_base + vma; }

TableAccess android_table_access() {
    TableAccess access{};
    access.base = ::g_base;
    access.recipe_ptr_slot = reinterpret_cast<void**>(bss(kRecipeDataSlot));
    access.mixture_ptr_slot = reinterpret_cast<void**>(bss(kMixtureDataSlot));
    access.recipe_size_slot = reinterpret_cast<uint8_t*>(bss(kRecipeSizeSlot));
    access.mixture_size_slot = reinterpret_cast<uint8_t*>(bss(kMixtureSizeSlot));
    access.recipe_count_slot = reinterpret_cast<uint16_t*>(bss(kRecipeCountSlot));
    return access;
}

}  // namespace

// 表日志绑定：宿主在 install/激活期经 PluginContext 注入（feature.cpp set_context 转发）。
// 表状态日志经 table_log_line 使用此绑定；未绑定时静默。
void set_context(const qol::contracts::QolGameHost* game, const qol::contracts::QolHostSdk* sdk) {
    g_game = game;
    g_sdk = sdk;
}

QolResult custom_recipe_table_acquire() {
    // g_base 未就绪时槽位地址本身无意义，必须先判（不得带着垃圾指针读取）。
    if (::g_base == 0) return table_status_result(TableAcquireStatus::kNotReady);
    size_t catalog_len = 0;
    const Def* catalog_ptr = catalog(&catalog_len);
    const TableAccess access = android_table_access();
    // busy 检测/置位/复位全部在 core 内（RAII），本入口不重复管理，避免自我拒绝。
    return table_status_result(table_acquire_core(access, &g_state, catalog_ptr, catalog_len,
                                                  &std::malloc, &std::free, &table_log_line,
                                                  &g_dedupe));
}

bool custom_recipe_table_ensure() { return custom_recipe_table_acquire().ok == kQolOk; }

void custom_recipe_table_deactivate() {
    if (::g_base == 0) return;
    const TableAccess access = android_table_access();
    (void)table_deactivate_core(access, &g_state);
}

}  // namespace custom_recipe
#endif  // __ANDROID__
