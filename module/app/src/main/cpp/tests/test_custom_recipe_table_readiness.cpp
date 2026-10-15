// custom_recipe 表 acquire/deactivate 便携核心的就绪分类与失败域测试。
//
// 直接驱动 `table.cpp` 中与 Android 入口共用的同一实现（table_acquire_core /
// classify_table_probe / table_deactivate_core），因此这里验证的是真实分类与注入路径，
// 不是把 mock 布尔值改名的替身：
//   · 零表 / 缺字段 → NotReady；
//   · 非零非法布局（含与 0 混用）→ Unsupported（永久，优先于「等待加载」）；
//   · 目录不可用、书名册长度不变式不满足 → Unsupported；
//   · 分配失败 → Internal（不留下半注入状态）；
//   · 重入 busy → Busy（不因已有 buffer 报告成功）；
//   · 成功注入 / 合法重新激活 → OK；
//   · deactivate 保留模块缓冲、只收记录数、还原原版段组位，且不写回陈旧原表指针。

#include "plugins/custom_recipe/catalog.h"
#include "plugins/custom_recipe/rules.h"
#include "plugins/custom_recipe/table.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#ifdef __ANDROID__
// 生产 Android 入口（table.cpp 的 __ANDROID__ 分支）需要该全局；测试从 .bss 槽位 VMA 造可控内存。
uintptr_t g_base = 0;
#endif

namespace cr = custom_recipe;

namespace {

// ---- 分配器接缝（注入以便覆盖分配失败）----
int g_alloc_calls = 0;
int g_free_calls = 0;
void* g_last_freed = nullptr;
int g_fail_alloc_at = -1;  // 1-based；<0 表示不失败

void* counting_alloc(size_t bytes) {
    ++g_alloc_calls;
    if (g_fail_alloc_at > 0 && g_alloc_calls == g_fail_alloc_at) return nullptr;
    return std::malloc(bytes);
}

void counting_free(void* ptr) {
    if (ptr != nullptr) {
        ++g_free_calls;
        g_last_freed = ptr;
    }
    std::free(ptr);
}
void reset_alloc() {
    g_alloc_calls = 0;
    g_free_calls = 0;
    g_last_freed = nullptr;
    g_fail_alloc_at = -1;
}

// ---- 重入探针：在注入的 alloc/log 回调内再入 core（真重入必须 Busy，外层仍完成）----
struct ReentryProbe {
    cr::TableAccess access{};
    cr::TableState* state = nullptr;
    const cr::Def* catalog = nullptr;
    size_t catalog_len = 0;
    bool reenter = false;
    int nested_calls = 0;
    cr::TableAcquireStatus nested_status = cr::TableAcquireStatus::kOk;
};

ReentryProbe* g_reentry = nullptr;

cr::TableAcquireStatus nested_core_call() {
    return cr::table_acquire_core(g_reentry->access, g_reentry->state, g_reentry->catalog,
                                  g_reentry->catalog_len, &counting_alloc, &counting_free, nullptr,
                                  nullptr);
}

void* reentrant_alloc(size_t bytes) {
    ++g_alloc_calls;
    if (g_reentry != nullptr && g_reentry->reenter && g_reentry->nested_calls == 0) {
        ++g_reentry->nested_calls;
        g_reentry->nested_status = nested_core_call();
    }
    if (g_fail_alloc_at > 0 && g_alloc_calls == g_fail_alloc_at) return nullptr;
    return std::malloc(bytes);
}

void reentrant_log(int32_t, const char*) {
    if (g_reentry != nullptr && g_reentry->reenter && g_reentry->nested_calls == 0) {
        ++g_reentry->nested_calls;
        g_reentry->nested_status = nested_core_call();
    }
}

// ---- 伪造原版 RECIPEBASE：record_size=12，b11 按需置 bit5 ----
constexpr uint8_t kRecordSize = 12;
constexpr uint8_t kMixtureSize = 3;
constexpr size_t kMaxRecords = 12;

struct ForgedTable {
    uint8_t recipe[kMaxRecords * kRecordSize];
    uint8_t mixture[kMaxRecords * kMixtureSize];
    uint16_t record_count;
};

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>(v >> 8);
}

// book5 = b11 带 bit5 的记录数；material_end = max(b4-5 + b6)。
void build_forged(ForgedTable* table, uint16_t record_count, uint16_t book5_count) {
    std::memset(table, 0, sizeof(*table));
    table->record_count = record_count;
    for (uint16_t r = 0; r < record_count; ++r) {
        uint8_t* rec = table->recipe + static_cast<size_t>(r) * kRecordSize;
        put_u16(rec + cr::kRbMaterialStart, 0);
        rec[cr::kRbMaterialCount] = 1;
        rec[cr::kRbFlag7] = cr::kRbFlag7Value;
        // bit5 = 配方书条目（kRbRecipeBookBit）；未计入 book5 的记录 b11 为 0。
        rec[cr::kRbGroup] = (r < book5_count) ? cr::kRbRecipeBookBit : 0;
    }
    // 原版材料段：每条记录 count=1 且 start=0 → material_end = 1；把最后一条改为 end=4。
    uint8_t* last = table->recipe + static_cast<size_t>(record_count - 1) * kRecordSize;
    put_u16(last + cr::kRbMaterialStart, 0);
    last[cr::kRbMaterialCount] = 4;
    for (size_t i = 0; i < 4; ++i) {
        table->mixture[i * kMixtureSize + 0] = 0x21;
        table->mixture[i * kMixtureSize + 1] = 0x00;
        table->mixture[i * kMixtureSize + 2] = 1;
    }
}

struct Slots {
    void* recipe_ptr = nullptr;
    void* mixture_ptr = nullptr;
    uint8_t recipe_size = kRecordSize;
    uint8_t mixture_size = kMixtureSize;
    uint16_t record_count = 0;

    cr::TableAccess access() {
        cr::TableAccess out{};
        out.base = 0x1000;
        out.recipe_ptr_slot = &recipe_ptr;
        out.mixture_ptr_slot = &mixture_ptr;
        out.recipe_size_slot = &recipe_size;
        out.mixture_size_slot = &mixture_size;
        out.recipe_count_slot = &record_count;
        return out;
    }
};

struct CatalogRef {
    const cr::Def* defs = nullptr;
    size_t count = 0;
};

CatalogRef catalog_ref() {
    CatalogRef ref{};
    ref.defs = cr::catalog(&ref.count);
    return ref;
}

bool same_pointer(const void* a, const void* b) { return a == b; }

// ---------------------------------------------------------------------------
// 分类：非零非法上界优先于缺失字段。
// ---------------------------------------------------------------------------
void test_classify_priority() {
    cr::TableProbe probe{};
    const auto classify = [](cr::TableProbe p) { return cr::classify_table_probe(p); };

    // 空表：base/指针/count/尺寸 任一缺失 → NotReady。
    assert(classify({}) == cr::TableAcquireStatus::kNotReady);
    probe = cr::TableProbe{};
    probe.base_ready = true;
    probe.recipe_ptr_present = true;
    probe.mixture_ptr_present = true;
    probe.recipe_size = kRecordSize;
    probe.mixture_size = kMixtureSize;
    probe.record_count = 3;
    probe.catalog_available = true;
    assert(classify(probe) == cr::TableAcquireStatus::kOk);

    probe.recipe_ptr_present = false;
    assert(classify(probe) == cr::TableAcquireStatus::kNotReady);
    probe.recipe_ptr_present = true;

    probe.record_count = 0;
    assert(classify(probe) == cr::TableAcquireStatus::kNotReady);
    probe.record_count = 3;

    probe.recipe_size = 0;
    assert(classify(probe) == cr::TableAcquireStatus::kNotReady);
    probe.recipe_size = kRecordSize;

    // 非零非法上界：即便另一个字段为 0，也必须判永久错误而不是「等待加载」。
    probe.recipe_size = cr::kMaxRecipeRecordSize + 1;
    assert(classify(probe) == cr::TableAcquireStatus::kUnsupported);
    probe.recipe_size = cr::kMaxRecipeRecordSize + 1;
    probe.record_count = 0;  // 混零
    assert(classify(probe) == cr::TableAcquireStatus::kUnsupported);
    probe.recipe_size = kRecordSize;
    probe.record_count = 3;
    probe.mixture_size = cr::kMaxMixtureRecordSize + 1;
    probe.recipe_ptr_present = false;  // 混另一个缺失字段
    assert(classify(probe) == cr::TableAcquireStatus::kUnsupported);
    probe.mixture_size = kMixtureSize;
    probe.recipe_ptr_present = true;
    probe.record_count = static_cast<uint16_t>(cr::kMaxTableRecordCount + 1);
    probe.recipe_size = 0;  // 混零
    assert(classify(probe) == cr::TableAcquireStatus::kUnsupported);
    probe.record_count = 3;
    probe.recipe_size = kRecordSize;

    // 布局合法但目录不可用 → 永久错误（配置问题，不是等游戏加载）。
    probe.catalog_available = false;
    assert(classify(probe) == cr::TableAcquireStatus::kUnsupported);

    // 状态 → QolResult 的错误域映射。
    assert(cr::table_status_result(cr::TableAcquireStatus::kOk).ok == kQolOk);
    assert(cr::table_status_result(cr::TableAcquireStatus::kNotReady).code == kQolErrNotReady);
    assert(cr::table_status_result(cr::TableAcquireStatus::kUnsupported).code ==
           kQolErrUnsupported);
    assert(cr::table_status_result(cr::TableAcquireStatus::kInternal).code == kQolErrInternal);
    assert(cr::table_status_result(cr::TableAcquireStatus::kBusy).code == kQolErrBusy);
    for (cr::TableAcquireStatus status :
         {cr::TableAcquireStatus::kNotReady, cr::TableAcquireStatus::kUnsupported,
          cr::TableAcquireStatus::kInternal, cr::TableAcquireStatus::kBusy}) {
        assert(cr::table_status_result(status).ok != kQolOk);
    }
}

// ---------------------------------------------------------------------------
// acquire：busy / NotReady / 永久错误 / 分配失败 / 成功 / 重新激活 / 游戏换表重注入。
// ---------------------------------------------------------------------------
void test_acquire_core() {
    const CatalogRef cat = catalog_ref();
    assert(cat.defs != nullptr && cat.count == 2);

    // busy：即便已有 buffer 也必须 Busy（不得报告成功），且不触碰槽位。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = forged.record_count;
        cr::TableState state{};
        state.busy = true;
        state.recipe_buffer = forged.recipe;  // 已有 buffer 也不行
        cr::TableAccess access = slots.access();
        reset_alloc();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kBusy);
        assert(g_alloc_calls == 0 && slots.record_count == forged.record_count);
        state.recipe_buffer = nullptr;
    }

    // NotReady：count 为 0 → 不分配、不写槽位。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = 0;
        cr::TableState state{};
        cr::TableAccess access = slots.access();
        reset_alloc();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kNotReady);
        assert(g_alloc_calls == 0 && slots.recipe_ptr == forged.recipe && state.recipe_buffer == nullptr);
    }

    // 永久错误：非零非法尺寸（即使另字段为 0）→ Unsupported，不分配。
    {
        ForgedTable forged{};
        build_forged(&forged, 0, 0);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.recipe_size = cr::kMaxRecipeRecordSize + 1;
        slots.record_count = 0;  // 混零：仍必须 Unsupported
        cr::TableState state{};
        cr::TableAccess access = slots.access();
        reset_alloc();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kUnsupported);
        assert(g_alloc_calls == 0 && state.recipe_buffer == nullptr);
    }

    // 分配失败：第 1（快照）/ 第 2（recipe）/ 第 3（mixture）次分配失败 → Internal。
    for (int fail_at = 1; fail_at <= 3; ++fail_at) {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = forged.record_count;
        cr::TableState state{};
        cr::TableAccess access = slots.access();
        reset_alloc();
        g_fail_alloc_at = fail_at;
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kInternal);
        // 槽位可写，但指针必须仍指向原表；模块状态与 base 缓存不得被当作成功写入。
        assert(slots.recipe_ptr == forged.recipe && slots.mixture_ptr == forged.mixture);
        assert(state.recipe_buffer == nullptr && state.mixture_buffer == nullptr);
        assert(state.base_record_count_cache == 0);
        if (fail_at == 1) {
            assert(g_alloc_calls == 1 && g_free_calls == 0);
        } else if (fail_at == 2) {
            assert(g_alloc_calls == 2 && g_free_calls == 0);
        } else {
            assert(g_alloc_calls == 3 && g_free_calls == 1);  // 已分配部分被释放
        }
    }

    // 成功注入：真实 core 路径（记录数 = base + 目录条数，注入行由 build_record_bytes 派生）。
    ForgedTable forged{};
    build_forged(&forged, 3, 3);
    const uint32_t base_material_count =
        cr::derive_material_count(forged.recipe, forged.record_count, kRecordSize);
    Slots slots{};
    slots.recipe_ptr = forged.recipe;
    slots.mixture_ptr = forged.mixture;
    slots.record_count = forged.record_count;
    cr::TableState state{};
    {
        cr::TableAccess access = slots.access();
        reset_alloc();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kOk);
        assert(slots.recipe_ptr == state.recipe_buffer && slots.mixture_ptr == state.mixture_buffer);
        assert(slots.record_count == forged.record_count + cat.count);
        assert(state.base_record_count_cache == forged.record_count);
        assert(cr::bound_base_record_count() == forged.record_count);
        assert(cr::catalog_ready());
        // 注入行逐字节等于目录派生记录（复用真实构建器，不复制算法）。
        const uint8_t* injected =
            static_cast<const uint8_t*>(state.recipe_buffer) +
            static_cast<size_t>(forged.record_count) * kRecordSize;
        uint8_t expected[kRecordSize];
        for (size_t i = 0; i < cat.count; ++i) {
            const uint16_t start = static_cast<uint16_t>(
                cr::material_start_at(static_cast<uint16_t>(base_material_count), cat.defs,
                                      cat.count, i));
            cr::build_record_bytes(cat.defs[i], start, expected);
            assert(std::memcmp(injected + i * kRecordSize, expected, kRecordSize) == 0);
        }
    }

    // 合法重新激活：槽位仍指向模块缓冲 → 只归位记录数，不重新分配。
    {
        const int alloc_before = g_alloc_calls;
        slots.record_count = forged.record_count;  // 模拟 deactivate 收数 / 或游戏侧被改回
        cr::TableAccess access = slots.access();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kOk);
        assert(slots.record_count == forged.record_count + cat.count);
        assert(g_alloc_calls == alloc_before);  // 未重新分配
        assert(state.recipe_buffer == slots.recipe_ptr);
    }

    // 游戏换表（指针被改写回原表）→ 用新原表重新注入；旧缓冲刻意不释放（避免悬空）。
    {
        ForgedTable replaced{};
        build_forged(&replaced, 4, 4);
        const void* old_recipe_buffer = state.recipe_buffer;
        slots.recipe_ptr = replaced.recipe;
        slots.mixture_ptr = replaced.mixture;
        slots.record_count = replaced.record_count;
        cr::TableAccess access = slots.access();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kOk);
        assert(slots.record_count == replaced.record_count + cat.count);
        assert(state.recipe_buffer != old_recipe_buffer);      // 重新分配
        assert(state.base_record_count_cache == replaced.record_count);
        // 旧表缓冲刻意不 free（游戏可能仍持有内部指针）；快照可因 base 段变大而重分配。
        assert(g_last_freed != old_recipe_buffer);
    }

    // 书名册长度不变式：count5 + n 跨字节边界 → 永久拒绝，不分配、不写槽位。
    {
        ForgedTable books{};
        build_forged(&books, 8, 7);  // bit5=7 → (7+7)/8=1；(9+7)/8=2 → 拒绝
        Slots book_slots{};
        book_slots.recipe_ptr = books.recipe;
        book_slots.mixture_ptr = books.mixture;
        book_slots.record_count = books.record_count;
        cr::TableState book_state{};
        cr::TableAccess access = book_slots.access();
        reset_alloc();
        assert(cr::table_acquire_core(access, &book_state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kUnsupported);
        assert(g_alloc_calls == 0 && book_state.recipe_buffer == nullptr);
        assert(book_slots.recipe_ptr == books.recipe);
        assert(book_slots.record_count == books.record_count);
    }

    std::free(state.original_group_bits);
    std::free(state.recipe_buffer);
    std::free(state.mixture_buffer);
}

// ---------------------------------------------------------------------------
// deactivate：no-op / 指针失配只清模块状态 / 正常还原并保留缓冲。
// ---------------------------------------------------------------------------
void test_deactivate_core() {
    const CatalogRef cat = catalog_ref();

    // base==0 或 buffer==null → no-op，不触碰槽位。
    {
        Slots slots{};
        uint16_t count = 7;
        slots.record_count = count;
        cr::TableAccess access = slots.access();
        access.base = 0;
        cr::TableState state{};
        assert(cr::table_deactivate_core(access, &state) == cr::TableAcquireStatus::kOk);
        assert(slots.record_count == count && slots.recipe_ptr == nullptr);

        access.base = 0x1000;
        assert(cr::table_deactivate_core(access, &state) == cr::TableAcquireStatus::kOk);
        assert(slots.record_count == count);
    }

    // 指针被游戏改写 → 只清模块状态，绝不写回（陈旧的）原表指针。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        ForgedTable game_table{};
        build_forged(&game_table, 3, 3);
        Slots slots{};
        slots.recipe_ptr = game_table.recipe;  // 游戏侧已改回原表
        slots.mixture_ptr = game_table.mixture;
        slots.record_count = game_table.record_count;
        cr::TableState state{};
        state.recipe_buffer = forged.recipe;
        state.base_record_count_cache = 3;
        cr::TableAccess access = slots.access();
        assert(cr::table_deactivate_core(access, &state) == cr::TableAcquireStatus::kOk);
        assert(slots.recipe_ptr == game_table.recipe);  // 未被模块指针覆盖
        assert(slots.record_count == game_table.record_count);
        assert(state.recipe_buffer == nullptr && state.mixture_buffer == nullptr);
    }

    // 正常停用：还原原版段 b11 + 收记录数；模块缓冲保留（后续可重新激活）。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        const uint8_t original_groups[3] = {forged.recipe[cr::kRbGroup],
                                            forged.recipe[kRecordSize + cr::kRbGroup],
                                            forged.recipe[2 * kRecordSize + cr::kRbGroup]};
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = forged.record_count;
        cr::TableState state{};
        cr::TableAccess access = slots.access();
        reset_alloc();
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kOk);
        // 注入态：原版段 b11 只被清掉模块占用的组位（bit5 保留）。
        const uint8_t* module = static_cast<const uint8_t*>(state.recipe_buffer);
        const uint8_t module_bits = cr::module_group_bits(cat.defs, cat.count);
        assert(module_bits != 0);
        assert(module[cr::kRbGroup] == static_cast<uint8_t>(original_groups[0] & ~module_bits));

        assert(cr::table_deactivate_core(access, &state) == cr::TableAcquireStatus::kOk);
        assert(slots.record_count == 3);                       // 收回记录数
        assert(slots.recipe_ptr == state.recipe_buffer);        // 缓冲保留为表体
        assert(state.recipe_buffer != nullptr);
        for (size_t r = 0; r < 3; ++r) {                        // 组位逐字节还原
            assert(module[r * kRecordSize + cr::kRbGroup] == original_groups[r]);
        }

        // 重新激活：同一缓冲、不重新分配。
        const int alloc_before = g_alloc_calls;
        assert(cr::table_acquire_core(access, &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kOk);
        assert(slots.record_count == 3 + cat.count);
        assert(g_alloc_calls == alloc_before);

        std::free(state.original_group_bits);
        std::free(state.recipe_buffer);
        std::free(state.mixture_buffer);
    }
}

// ---------------------------------------------------------------------------
// busy 生命周期与真重入：检测/置位/复位都在 core；alloc/log 回调内重入必须 Busy，
// 外层仍能完成，且任何失败返回后 busy 都复位。
// ---------------------------------------------------------------------------
void test_busy_lifecycle_and_reentry() {
    const CatalogRef cat = catalog_ref();

    // 注入 alloc 回调内重入：内层 Busy，外层 OK，busy 复位。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = forged.record_count;
        cr::TableState state{};
        ReentryProbe probe{};
        probe.access = slots.access();
        probe.state = &state;
        probe.catalog = cat.defs;
        probe.catalog_len = cat.count;
        probe.reenter = true;
        g_reentry = &probe;
        reset_alloc();
        const cr::TableAcquireStatus outer =
            cr::table_acquire_core(probe.access, &state, cat.defs, cat.count, &reentrant_alloc,
                                   &counting_free, nullptr, nullptr);
        g_reentry = nullptr;
        assert(outer == cr::TableAcquireStatus::kOk);
        assert(probe.nested_calls == 1);
        assert(probe.nested_status == cr::TableAcquireStatus::kBusy);
        assert(!state.busy);
        std::free(state.original_group_bits);
        std::free(state.recipe_buffer);
        std::free(state.mixture_buffer);
    }

    // 注入 log 回调内重入（NotReady 首次会记录）：内层 Busy，外层 NotReady，busy 复位。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = 0;  // NotReady
        cr::TableState state{};
        cr::TableLogDedupe dedupe{};
        ReentryProbe probe{};
        probe.access = slots.access();
        probe.state = &state;
        probe.catalog = cat.defs;
        probe.catalog_len = cat.count;
        probe.reenter = true;
        g_reentry = &probe;
        const cr::TableAcquireStatus outer =
            cr::table_acquire_core(probe.access, &state, cat.defs, cat.count, &counting_alloc,
                                   &counting_free, &reentrant_log, &dedupe);
        g_reentry = nullptr;
        assert(outer == cr::TableAcquireStatus::kNotReady);
        assert(probe.nested_calls == 1 && probe.nested_status == cr::TableAcquireStatus::kBusy);
        assert(!state.busy);
        assert(state.recipe_buffer == nullptr);  // NotReady 不改动模块状态
    }

    // 失败路径一律复位 busy：Unsupported / Internal 之后再次调用不得是 Busy。
    {
        ForgedTable forged{};
        build_forged(&forged, 3, 3);
        Slots slots{};
        slots.recipe_ptr = forged.recipe;
        slots.mixture_ptr = forged.mixture;
        slots.record_count = forged.record_count;
        slots.recipe_size = cr::kMaxRecipeRecordSize + 1;
        cr::TableState state{};
        assert(cr::table_acquire_core(slots.access(), &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kUnsupported);
        assert(!state.busy);
        assert(cr::table_acquire_core(slots.access(), &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kUnsupported);

        slots.recipe_size = kRecordSize;
        reset_alloc();
        g_fail_alloc_at = 1;
        assert(cr::table_acquire_core(slots.access(), &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kInternal);
        assert(!state.busy);
        reset_alloc();
        assert(cr::table_acquire_core(slots.access(), &state, cat.defs, cat.count, &counting_alloc,
                                      &counting_free, nullptr, nullptr) ==
               cr::TableAcquireStatus::kOk);
        assert(!state.busy);
        std::free(state.original_group_bits);
        std::free(state.recipe_buffer);
        std::free(state.mixture_buffer);
    }
}

#ifdef __ANDROID__
// ---------------------------------------------------------------------------
// 真实 Android 入口（table.cpp 的 __ANDROID__ 分支）：按 .bss 槽位 VMA 造可控内存，
// 直接调用 production custom_recipe_table_acquire()/deactivate() 与 production
// custom_recipe::set_context()（测试**不**定义自己的 set_context 替身）。
// 槽位 VMA 与 table.cpp 的 slot 常量一致（独立声明，用于构造内存，不复制算法）。
// ---------------------------------------------------------------------------
constexpr uintptr_t kMixtureDataVma = 0x3019A0;
constexpr uintptr_t kMixtureSizeVma = 0x3019A8;
constexpr uintptr_t kRecipeDataVma = 0x3019B0;
constexpr uintptr_t kRecipeSizeVma = 0x3019B8;
constexpr uintptr_t kRecipeCountVma = 0x3019BA;
constexpr size_t kFakeBssSize = 0x3019C0;  // 覆盖到 count 字段之后（约 3.15MB）

uint8_t g_fake_bss[kFakeBssSize];

template <typename T>
T* bss_at(uintptr_t vma) {
    return reinterpret_cast<T*>(g_base + vma);
}

// 记录型 SDK 日志桩：核对 tag（source）、message 内容与回调次数（去重语义）。
struct FakeLog {
    int calls = 0;
    int32_t level = 0;
    int32_t domain = 0;
    const char* source = nullptr;
    uint32_t size = 0;
    char copy[256]{};
};

FakeLog g_log;

QolResult fake_log_write(int32_t level, int32_t domain, const char* source, const char* message,
                         uint32_t size) {
    ++g_log.calls;
    g_log.level = level;
    g_log.domain = domain;
    g_log.source = source;
    g_log.size = size;
    const size_t n = size < sizeof(g_log.copy) - 1 ? size : sizeof(g_log.copy) - 1;
    if (message != nullptr) std::memcpy(g_log.copy, message, n);
    g_log.copy[n] = '\0';
    return qol::contracts::ok();
}

qol::contracts::QolHostSdk g_fake_sdk{};

bool log_contains(const char* needle) { return std::strstr(g_log.copy, needle) != nullptr; }

void test_android_bridge() {
    // production set_context 绑定日志面（测试自定义的 g_fake_sdk，不是 set_context 替身）。
    g_fake_sdk = qol::contracts::QolHostSdk{};
    g_fake_sdk.abi = {sizeof(g_fake_sdk), qol::contracts::kHostSdkMajor,
                      qol::contracts::kHostSdkMinor};
    g_fake_sdk.log.write = &fake_log_write;
    cr::set_context(nullptr, &g_fake_sdk);

    // g_base==0 → NotReady（不得读取槽位，也不产生日志）。
    g_base = 0;
    assert(cr::custom_recipe_table_acquire().code == kQolErrNotReady);
    assert(!cr::custom_recipe_table_ensure());
    assert(g_log.calls == 0);

    // base 有效但槽位全空 → NotReady，并经 production 日志面记录一条（tag/message/次数）。
    std::memset(g_fake_bss, 0, sizeof(g_fake_bss));
    g_base = reinterpret_cast<uintptr_t>(g_fake_bss);
    const QolResult not_ready = cr::custom_recipe_table_acquire();
    assert(not_ready.code == kQolErrNotReady);
    const char* const not_ready_message = not_ready.message;  // 静态生命周期：后续仍须可读
    assert(not_ready_message != nullptr);
    assert(g_log.calls == 1);
    assert(g_log.level == 2);  // 未就绪记 WARN
    assert(g_log.domain == 15);
    assert(g_log.source != nullptr &&
           std::strcmp(g_log.source, "plugins/custom_recipe/table") == 0);
    assert(log_contains("recipe table not ready"));
    assert(g_log.size == std::strlen(g_log.copy));

    // 同因重复 NotReady → 去重：不再回调。
    assert(cr::custom_recipe_table_acquire().code == kQolErrNotReady);
    assert(g_log.calls == 1);

    // 真实小表 → OK 且完成注入；**关键回归：不是 Busy**（修前桥自置 busy 被 core 拒绝）。
    ForgedTable forged{};
    build_forged(&forged, 3, 3);
    *bss_at<void*>(kRecipeDataVma) = forged.recipe;
    *bss_at<void*>(kMixtureDataVma) = forged.mixture;
    *bss_at<uint8_t>(kRecipeSizeVma) = kRecordSize;
    *bss_at<uint8_t>(kMixtureSizeVma) = kMixtureSize;
    *bss_at<uint16_t>(kRecipeCountVma) = forged.record_count;
    const QolResult acquire = cr::custom_recipe_table_acquire();
    assert(acquire.ok == kQolOk);
    assert(acquire.code != kQolErrBusy);
    assert(g_log.calls == 2);  // 未就绪 1 条 + 注入成功 1 条
    assert(g_log.level == 1);
    assert(log_contains("injected"));
    assert(log_contains("records="));
    void* const module_recipe = *bss_at<void*>(kRecipeDataVma);
    assert(module_recipe != nullptr && module_recipe != forged.recipe);
    assert(*bss_at<void*>(kMixtureDataVma) != nullptr);
    assert(*bss_at<uint16_t>(kRecipeCountVma) == forged.record_count + 2);
    assert(cr::catalog_ready());
    assert(cr::custom_recipe_table_ensure());

    // 合法重新激活：记录数被改回后再次 acquire → 归位且指针不变（不重新分配）。
    *bss_at<uint16_t>(kRecipeCountVma) = forged.record_count;
    assert(cr::custom_recipe_table_acquire().ok == kQolOk);
    assert(*bss_at<uint16_t>(kRecipeCountVma) == forged.record_count + 2);
    assert(*bss_at<void*>(kRecipeDataVma) == module_recipe);

    // 非法 probe 不得经「已有 buffer」捷径判 OK：尺寸越界 → 永久 Unsupported，
    // 且不得把 metadata 错误记成「未就绪」（不产生新的日志回调）。
    const int log_calls_before_metadata_error = g_log.calls;
    *bss_at<uint8_t>(kRecipeSizeVma) = cr::kMaxRecipeRecordSize + 1;
    assert(cr::custom_recipe_table_acquire().code == kQolErrUnsupported);
    assert(*bss_at<void*>(kRecipeDataVma) == module_recipe);  // 非法分支不改动状态
    assert(g_log.calls == log_calls_before_metadata_error);
    *bss_at<uint8_t>(kRecipeSizeVma) = kRecordSize;

    // NotReady 不改动模块状态；恢复后仍走合法重新激活。
    *bss_at<uint16_t>(kRecipeCountVma) = 0;
    assert(cr::custom_recipe_table_acquire().code == kQolErrNotReady);
    *bss_at<uint16_t>(kRecipeCountVma) = forged.record_count;
    assert(cr::custom_recipe_table_acquire().ok == kQolOk);
    assert(*bss_at<void*>(kRecipeDataVma) == module_recipe);

    // deactivate：收回记录数、保留模块缓冲；再次 acquire 重新激活且指针不变。
    cr::custom_recipe_table_deactivate();
    assert(*bss_at<uint16_t>(kRecipeCountVma) == forged.record_count);
    assert(*bss_at<void*>(kRecipeDataVma) == module_recipe);
    assert(cr::custom_recipe_table_acquire().ok == kQolOk);
    assert(*bss_at<uint16_t>(kRecipeCountVma) == forged.record_count + 2);
    assert(*bss_at<void*>(kRecipeDataVma) == module_recipe);

    // 连续调用仍为 OK（busy 已在所有返回路径复位）。
    assert(cr::custom_recipe_table_acquire().ok == kQolOk);

    // 返回的 QolResult.message 为静态生命周期：经多次后续调用/状态变化后仍可读且内容不变。
    assert(not_ready_message != nullptr &&
           std::strcmp(not_ready_message, "table not loaded") == 0);

    // 解除绑定（set_context(nullptr,nullptr)）后，未就绪路径日志静默。
    cr::set_context(nullptr, nullptr);
    std::memset(g_fake_bss, 0, sizeof(g_fake_bss));  // 回到未就绪（尺寸/计数变化以绕过去重）
    *bss_at<void*>(kRecipeDataVma) = forged.recipe;
    *bss_at<void*>(kMixtureDataVma) = forged.mixture;
    const int calls_after_unbind = g_log.calls;
    assert(cr::custom_recipe_table_acquire().code == kQolErrNotReady);
    assert(g_log.calls == calls_after_unbind);

    g_base = 0;  // 复位，避免影响其它进程内用例
}
#endif

}  // namespace

int main() {
    test_classify_priority();
    test_acquire_core();
    test_busy_lifecycle_and_reentry();
    test_deactivate_core();
#ifdef __ANDROID__
    test_android_bridge();
    std::printf("custom_recipe_table_readiness: classify/acquire/deactivate core + android bridge verified\n");
#else
    std::printf("custom_recipe_table_readiness: classify/acquire/deactivate core verified\n");
#endif
    return 0;
}
