#include "contracts/inventory_stage4.h"
#include "contracts/s2_writeback.h"
#include "plugins/core.item/usecases/consume_item.h"
#include "plugins/core.item/hooks/inventory_hooks.h"
#include "plugins/extension_bag/model/equip_rules.h"

#include "game/symbols/game_symbols.h"

#include <cstdint>
#include <cstdio>

namespace inventory_stage = qol::contracts::inventory_stage;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void test_s2_writeback_applied_gate() {
    // Off 与 NotReady 都映射为 applied=false，故关闭态只由原版写 b，模块不回写 a+b。
    CHECK(!qol::contracts::s2_writeback::allowed(false, true));
    CHECK(!qol::contracts::s2_writeback::allowed(true, false));
    CHECK(qol::contracts::s2_writeback::allowed(true, true));
    // 128 倍数合并：b 段不变，只有源槽消失能证明原版完成合并。
    CHECK(qol::contracts::s2_writeback::source_removed_confirms_merge(640, true, false));
    CHECK(!qol::contracts::s2_writeback::source_removed_confirms_merge(640, true, true));
    CHECK(!qol::contracts::s2_writeback::source_removed_confirms_merge(641, true, false));
    CHECK(!qol::contracts::s2_writeback::source_removed_confirms_merge(640, false, false));
    CHECK(!qol::contracts::s2_writeback::source_removed_confirms_merge(0, true, false));
}

static int g_original_value = 0;
static int g_extension_value = 0;
static int g_original_calls = 0;
static int g_extension_calls = 0;
static bool* g_recursive_guard = nullptr;

static int original_count(int32_t) { ++g_original_calls; return g_original_value; }
static int extension_count(int32_t) { ++g_extension_calls; return g_extension_value; }
static int recursive_original(int32_t category) {
    ++g_original_calls;
    if (g_original_calls == 1 && g_recursive_guard != nullptr) {
        return inventory_stage::have_item(category, recursive_original, extension_count, *g_recursive_guard);
    }
    return 0;
}

static void test_original_only_queries() {    bool guard = false;
    g_original_value = 0;
    g_original_calls = 0;
    g_extension_calls = 0;
    CHECK(inventory_stage::have_item_original_only(1, original_count, guard) == 0);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);

    g_original_value = 4;
    g_original_calls = 0;
    CHECK(inventory_stage::get_item_count_original_only(1, original_count, guard) == 4);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);
}

static void* g_extension_item = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
static int g_identify_calls = 0;
static int g_extension_action_calls = 0;
static int g_backup_action_calls = 0;
static bool g_extension_consume_result = true;
static bool identify_item(void* item, int32_t* bag, int32_t* slot) {
    ++g_identify_calls;
    if (bag != nullptr) *bag = 6;
    if (slot != nullptr) *slot = 2;
    return item == g_extension_item;
}
static bool extension_consume(void*) {
    ++g_extension_action_calls;
    return g_extension_consume_result;
}
static bool extension_remove(bool) { return true; }
static bool remove_item_extension(void*) { ++g_extension_action_calls; return true; }
static void backup_consume(void*) { ++g_backup_action_calls; }
static int backup_remove(void*) { ++g_backup_action_calls; return 7; }

static void* item_at(int32_t, int32_t) { return g_extension_item; }
static void* g_fallback_view_active = nullptr;
static int g_fallback_calls = 0;
static void* view_item_at(int32_t, int32_t) {
    ++g_fallback_calls;
    return g_fallback_view_active;
}
static int g_equip_extension_calls = 0;
static int g_equip_backup_calls = 0;
static void* g_equip_character_seen = nullptr;
static bool extension_equip(void* character, void*, int32_t bag, int32_t slot, int32_t equip_slot) {
    ++g_equip_extension_calls;
    g_equip_character_seen = character;
    return bag == 6 && slot == 2 && equip_slot == 3;
}
static int backup_equip(void*, int32_t, int32_t, int32_t) { ++g_equip_backup_calls; return 9; }

static int g_jewel_backup_result = 0;
static int g_jewel_backup_calls = 0;
static int jewel_backup(void*, void*) { ++g_jewel_backup_calls; return g_jewel_backup_result; }
static int g_jewel_extension_calls = 0;
static int jewel_extension(void* equip_item, void* jewel_item, Stage4JewelBackup backup) {
    ++g_jewel_extension_calls;
    return backup == nullptr ? 3 : backup(equip_item, jewel_item);
}

static int g_unequip_backup_result = 0;
static int g_unequip_backup_calls = 0;
static int unequip_backup(void*, int32_t) { ++g_unequip_backup_calls; return g_unequip_backup_result; }
static int g_unequip_adopt_calls = 0;
static bool g_unequip_adopt_result = true;
static bool unequip_adopt(void*, int32_t) { ++g_unequip_adopt_calls; return g_unequip_adopt_result; }

static int g_install_calls = 0;
static int g_uninstall_calls = 0;
static int g_install_fail_at = -1;
static int fake_install(void*, void*, void** backup) {
    const int index = g_install_calls++;
    if (index == g_install_fail_at) return 1;
    if (backup != nullptr) *backup = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + index));
    return 0;
}
static int fake_uninstall(void*) { ++g_uninstall_calls; return 0; }

static void test_queries() {
    bool guard = false;
    g_original_value = 1; g_extension_value = 5; g_original_calls = 0; g_extension_calls = 0;
    CHECK(inventory_stage::have_item(1, original_count, extension_count, guard) == 1);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);

    g_original_value = 0; g_extension_value = 5; g_original_calls = 0; g_extension_calls = 0;
    CHECK(inventory_stage::have_item(1, original_count, extension_count, guard) == 1);
    CHECK(g_original_calls == 1 && g_extension_calls == 1 && !guard);

    g_original_value = 4; g_extension_value = 6; g_original_calls = 0; g_extension_calls = 0;
    CHECK(inventory_stage::get_item_count(1, original_count, extension_count, guard) == 10);
    CHECK(g_original_calls == 1 && g_extension_calls == 1 && !guard);

    g_original_calls = 0; g_extension_calls = 0; g_recursive_guard = &guard;
    CHECK(inventory_stage::have_item(1, recursive_original, extension_count, guard) == 1);
    CHECK(g_original_calls == 2 && g_extension_calls == 1 && !guard);
    g_recursive_guard = nullptr;
}

static void test_object_operations() {
    bool guard = false;
    g_identify_calls = 0; g_extension_action_calls = 0; g_backup_action_calls = 0;
    g_extension_consume_result = true;
    CHECK(inventory_stage::consume_item(g_extension_item, identify_item, extension_consume, backup_consume,
                              guard));
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);
    CHECK(inventory_stage::consume_item(reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999)),
                              identify_item, extension_consume, backup_consume, guard));
    CHECK(g_backup_action_calls == 1 && !guard);

    g_extension_consume_result = false;
    g_extension_action_calls = 0; g_backup_action_calls = 0;
    CHECK(!inventory_stage::consume_item(g_extension_item, identify_item, extension_consume, backup_consume,
                               guard));
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);

    g_extension_action_calls = 0; g_backup_action_calls = 0;
    CHECK(inventory_stage::remove_item(g_extension_item, identify_item, remove_item_extension,
                             backup_remove, guard) == 1);
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);
    CHECK(inventory_stage::remove_item(reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999)),
                             identify_item, remove_item_extension, backup_remove, guard) == 7);
    CHECK(g_backup_action_calls == 1 && !guard);

    void* equip_character = reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678));
    g_equip_extension_calls = 0; g_equip_backup_calls = 0; g_equip_character_seen = nullptr;
    CHECK(stage4_equip_item(equip_character, 0, 0, 3, item_at, identify_item,
                            extension_equip, backup_equip) == 1);
    CHECK(g_equip_extension_calls == 1 && g_equip_backup_calls == 0 &&
          g_equip_character_seen == equip_character);
    CHECK(stage4_equip_item(equip_character, 0, 0, 4, nullptr, identify_item,
                            extension_equip, backup_equip) == 9);
    CHECK(g_equip_backup_calls == 1);

    // 源槽读空（INVEN 全程真实，扩展物品只在控件投影）时由带视图门禁的
    // extension_item_at 物化兜底：命中扩展物品走扩展装备，门禁返回空回落原版。
    g_fallback_view_active = g_extension_item; g_fallback_calls = 0;
    g_equip_extension_calls = 0; g_equip_backup_calls = 0; g_equip_character_seen = nullptr;
    CHECK(stage4_equip_item(equip_character, 0, 0, 3, nullptr, identify_item,
                            extension_equip, backup_equip, view_item_at) == 1);
    CHECK(g_fallback_calls == 1 && g_equip_extension_calls == 1 && g_equip_backup_calls == 0 &&
          g_equip_character_seen == equip_character);
    g_fallback_view_active = nullptr; g_fallback_calls = 0;
    g_equip_extension_calls = 0; g_equip_backup_calls = 0;
    CHECK(stage4_equip_item(equip_character, 0, 0, 3, nullptr, identify_item,
                            extension_equip, backup_equip, view_item_at) == 9);
    CHECK(g_fallback_calls == 1 && g_equip_extension_calls == 0 && g_equip_backup_calls == 1);

    g_jewel_backup_result = 0; g_jewel_backup_calls = 0; g_extension_action_calls = 0;
    CHECK(stage4_put_jewel(nullptr, g_extension_item, identify_item, jewel_backup,
                            nullptr) == 0);
    CHECK(g_jewel_backup_calls == 1 && g_extension_action_calls == 0);
    g_jewel_backup_result = 3;
    CHECK(stage4_put_jewel(nullptr, g_extension_item, identify_item, jewel_backup,
                            nullptr) == 3);
    CHECK(g_extension_action_calls == 0);

    g_jewel_backup_result = 0; g_jewel_backup_calls = 0; g_jewel_extension_calls = 0;
    CHECK(stage4_put_jewel(g_extension_item, g_extension_item, identify_item, jewel_backup,
                           jewel_extension) == 0);
    CHECK(g_jewel_extension_calls == 1 && g_jewel_backup_calls == 1 &&
          g_extension_action_calls == 0);

    // EquipControlEventProc 的 wrapper 判据：只有 event=0x04、扩展宝石源且目标为
    // 装备槽才进入扩展分支；原版源、扩展非宝石、非装备目标和其它事件均必须 backup。
    CHECK(extension_bag_is_equip_control_source(0x04, true, true, true));
    CHECK(!extension_bag_is_equip_control_source(0x04, true, true, false));
    CHECK(!extension_bag_is_equip_control_source(0x04, true, false, true));
    CHECK(!extension_bag_is_equip_control_source(0x04, false, true, true));
    CHECK(!extension_bag_is_equip_control_source(0x02, true, true, true));
    CHECK(extension_bag_is_equip_control_source(0x04, true, true, true));

    // S-03/R-50 apply 准入判据：宝石/卷轴 + 装备类目标 + IsApplyStuff 判真才进入
    // apply；宝石/卷轴 + 普通物品目标必须非 apply（回退扩展交换/移动）；普通物品
    // 源 + 任意目标一律非 apply；类别数据不可用由调用方映射为 target_is_equip=false，
    // 与普通物品目标同路径 fail-closed。
    CHECK(extension_bag_is_apply_candidate(true, true, true, true));
    CHECK(extension_bag_is_apply_candidate(true, true, true, false) == false);
    CHECK(extension_bag_is_apply_candidate(true, true, false, true) == false);
    CHECK(extension_bag_is_apply_candidate(true, true, false, false) == false);
    CHECK(extension_bag_is_apply_candidate(false, true, true, true) == false);
    CHECK(extension_bag_is_apply_candidate(true, false, true, true) == false);
    CHECK(extension_bag_is_apply_candidate(false, false, false, false) == false);
    CHECK(extension_bag_finish_requires_abort(false));
    CHECK(!extension_bag_finish_requires_abort(true));
}

static void test_unequip_to_inven() {
    bool guard = false;

    g_unequip_backup_result = 5; g_unequip_backup_calls = 0; g_unequip_adopt_calls = 0;
    CHECK(stage4_unequip_item_to_inven(nullptr, 3, unequip_backup, unequip_adopt, guard) == 5);
    CHECK(g_unequip_backup_calls == 1 && g_unequip_adopt_calls == 0 && !guard);

    g_unequip_backup_result = 0; g_unequip_adopt_result = true;
    g_unequip_backup_calls = 0; g_unequip_adopt_calls = 0;
    CHECK(stage4_unequip_item_to_inven(nullptr, 3, unequip_backup, unequip_adopt, guard) == 1);
    CHECK(g_unequip_backup_calls == 1 && g_unequip_adopt_calls == 1 && !guard);

    g_unequip_backup_result = 0; g_unequip_adopt_result = false;
    g_unequip_backup_calls = 0; g_unequip_adopt_calls = 0;
    CHECK(stage4_unequip_item_to_inven(nullptr, 3, unequip_backup, unequip_adopt, guard) == 0);
    CHECK(g_unequip_backup_calls == 1 && g_unequip_adopt_calls == 1 && !guard);

    g_unequip_backup_result = 0; g_unequip_adopt_calls = 0;
    CHECK(stage4_unequip_item_to_inven(nullptr, 3, unequip_backup, nullptr, guard) == 0);
    CHECK(g_unequip_adopt_calls == 0 && !guard);
}

static void test_install_transaction() {
    void* backup_a = nullptr;
    void* backup_b = nullptr;
    const Stage4HookSpec hooks[] = {{reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), &backup_a},
                                    {reinterpret_cast<void*>(3), reinterpret_cast<void*>(4), &backup_b}};
    g_install_calls = 0; g_uninstall_calls = 0; g_install_fail_at = -1;
    CHECK(stage4_install_transaction(hooks, 2, fake_install, fake_uninstall));
    CHECK(g_install_calls == 2 && g_uninstall_calls == 0 && backup_a != nullptr && backup_b != nullptr);

    backup_a = nullptr; backup_b = nullptr; g_install_calls = 0; g_uninstall_calls = 0; g_install_fail_at = 1;
    CHECK(!stage4_install_transaction(hooks, 2, fake_install, fake_uninstall));
    CHECK(g_install_calls == 2 && g_uninstall_calls == 1 && backup_a == nullptr && backup_b == nullptr);
}

static int g_cumulate_backup_result = 0;
static int g_cumulate_backup_calls = 0;
static int cumulate_backup(void*) { ++g_cumulate_backup_calls; return g_cumulate_backup_result; }

enum class EffectiveResultMode : uint8_t { kOk, kNotReady, kUnsupported };
static uint32_t g_effective_count = 0;
static EffectiveResultMode g_effective_result_mode = EffectiveResultMode::kOk;

static QolResult fake_effective(const void*, uint32_t* out_count) {
    if (g_effective_result_mode == EffectiveResultMode::kNotReady) {
        return qol::contracts::error(kQolErrNotReady, "not ready", 9);
    }
    if (g_effective_result_mode == EffectiveResultMode::kUnsupported) {
        return qol::contracts::error(kQolErrUnsupported, "unsupported", 11);
    }
    *out_count = g_effective_count;
    return qol::contracts::ok();
}

static const qol::contracts::QolItemCountApi kStage4ItemCountApi = {
    nullptr, &fake_effective, nullptr, nullptr, nullptr};

// H-17 读侧分流（R-45/R-46/R-47/R-49）：effective facade 成功时直接返回，
// 未驻留/未就绪与 unsupported 走原版 backup。
static void test_get_cumulate_count_facade() {
    void* nonnull_item = reinterpret_cast<void*>(static_cast<uintptr_t>(0x417));
    // facade 成功：返回 facade 值，backup 保持未调用（数量 0/99/100/127/128/199/999/1023）。
    const uint32_t counts[] = {0u, 99u, 100u, 127u, 128u, 199u, 999u, 1023u};
    for (const uint32_t count : counts) {
        g_cumulate_backup_calls = 0;
        g_cumulate_backup_result = -1;
        g_effective_result_mode = EffectiveResultMode::kOk;
        g_effective_count = count;
        CHECK(inventory_stage::get_cumulate_count(nonnull_item, &kStage4ItemCountApi,
                                        cumulate_backup) == static_cast<int>(count));
        CHECK(g_cumulate_backup_calls == 0);
    }
    // kNotReady：provider 未驻留/999 原子组未就绪时 original-first backup。
    g_cumulate_backup_calls = 0; g_cumulate_backup_result = 1;
    g_effective_result_mode = EffectiveResultMode::kNotReady;
    CHECK(inventory_stage::get_cumulate_count(nonnull_item, &kStage4ItemCountApi,
                                    cumulate_backup) == 1);
    CHECK(g_cumulate_backup_calls == 1);

    // kUnsupported：类别判定不可用时 fail-closed 走 backup。
    g_cumulate_backup_calls = 0; g_cumulate_backup_result = 1;
    g_effective_result_mode = EffectiveResultMode::kUnsupported;
    CHECK(inventory_stage::get_cumulate_count(nonnull_item, &kStage4ItemCountApi,
                                    cumulate_backup) == 1);
    CHECK(g_cumulate_backup_calls == 1);

    // 空指针：原版语义返回 0（backup(nullptr)）。
    g_cumulate_backup_calls = 0; g_cumulate_backup_result = 0;
    CHECK(inventory_stage::get_cumulate_count(nullptr, &kStage4ItemCountApi,
                                    cumulate_backup) == 0);
    CHECK(g_cumulate_backup_calls == 1);

    // facade 未驻留与 backup 缺失兜底：返回 0，不崩。
    CHECK(inventory_stage::get_cumulate_count(nonnull_item, nullptr, nullptr) == 0);
    g_effective_result_mode = EffectiveResultMode::kOk;
}

static QolResult fake_conversion_limit(int32_t extended, uint32_t* out_limit) {
    if (out_limit == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    *out_limit = extended != 0 ? 999u : 99u;
    return qol::contracts::ok();
}

static constexpr uint32_t kTestS2ShiftA = 22u;
static constexpr uint32_t kTestS2MaskA = 0x7u << kTestS2ShiftA;
static constexpr uint32_t kTestS2ShiftB = 25u;
static constexpr uint32_t kTestS2MaskB = 0x7fu << kTestS2ShiftB;

static uint32_t s2_test_write_count(uint32_t raw, uint32_t count) {
    return (raw & ~(kTestS2MaskA | kTestS2MaskB)) |
           (((count / 128u) & 0x7u) << kTestS2ShiftA) |
           ((count & 0x7fu) << kTestS2ShiftB);
}

static uint32_t s2_test_read_count(uint32_t raw) {
    return (((raw >> kTestS2ShiftA) & 0x7u) * 128u) +
           ((raw >> kTestS2ShiftB) & 0x7fu);
}

static uint32_t s2_test_raw(uint32_t count) {
    return s2_test_write_count(0x00123456u, count);
}

static uint32_t s2_test_b_only_raw(uint32_t pre_raw, uint32_t remain) {
    return (pre_raw & ~kTestS2MaskB) | ((remain & 0x7fu) << kTestS2ShiftB);
}

static QolResult fake_field_from_canonical(uint32_t raw, uint32_t canonical,
                                           int32_t extended, uint32_t* out_raw) {
    if (out_raw == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    if (extended != 0) {
        if (canonical > 999u) canonical = 999u;
        *out_raw = s2_test_write_count(raw, canonical);
    } else {
        uint32_t view = canonical & 0x7fu;
        if (view > 99u) view = 99u;
        *out_raw = (raw & ~kTestS2MaskB) | (view << kTestS2ShiftB);
    }
    return qol::contracts::ok();
}

static QolResult fake_field_from_canonical_failure(uint32_t, uint32_t, int32_t, uint32_t*) {
    return qol::contracts::error(kQolErrUnsupported, "unsupported", 11);
}

static const qol::contracts::QolItemCountApi kStage4ConversionApi = {
    nullptr, nullptr, nullptr, nullptr, nullptr, &fake_field_from_canonical,
    nullptr, nullptr, &fake_conversion_limit, nullptr, nullptr, nullptr};

static inventory_stage::RemoveDataEntry s2_test_pre(void* item, uint32_t full) {
    return {item, s2_test_raw(full), full};
}

static inventory_stage::RemoveDataPost s2_test_post(void* item, uint32_t raw) {
    const uint32_t decoded = s2_test_read_count(raw);
    // 与 item_count.effective 的启用态读视图一致：超过 999 时 clamp。
    return {item, raw, decoded > 999u ? 999u : decoded};
}

// H-21 INVEN_RemoveItemData 按原版 b-only 写回形态识别唯一部分删堆，覆盖跨 127、
// clamp 边界、整删消失堆、多候选、非候选原始字段变化与 fail-closed 入参。
static void test_remove_item_data_plan() {
    void* a = reinterpret_cast<void*>(static_cast<uintptr_t>(0x501));
    void* b = reinterpret_cast<void*>(static_cast<uintptr_t>(0x502));
    void* c = reinterpret_cast<void*>(static_cast<uintptr_t>(0x503));

    // 150 - 25 = 125；原版保留 a=1，只写 b，视图增大为 253，需修正为 125。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 125u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 25);
        CHECK(post[0].post_view == 253u);
        CHECK(plan.correct && plan.entry_index == 0 && plan.remain == 125u);
    }

    // remain=127 与 9 时，旧 a 分别使原版 b-only 视图成为 255 与 137。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 199u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 127u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 72);
        CHECK(post[0].post_view == 255u);
        CHECK(plan.correct && plan.remain == 127u);
    }
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 199u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 9u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 190);
        CHECK(post[0].post_view == 137u);
        CHECK(plan.correct && plan.remain == 9u);
    }

    // remain=172 与 100 均按 a 段原样保留识别，尽管 b 段可不变或回绕增大。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 300u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 172u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 128);
        CHECK(post[0].post_raw == pre[0].pre_raw);
        CHECK(plan.correct && plan.remain == 172u);
    }
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 300u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 100u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 200);
        CHECK(plan.correct && plan.remain == 100u);
    }

    // 300 - 44 = 256；b-only 写回视图已等于 remain，不需要二次回写。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 300u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 256u))};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 44).correct);
    }

    // expected_view 超过 999 时只按未 clamp 的 S2 视图判断原版 b-only 形态。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 999u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 127u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 872);
        CHECK(post[0].post_view == 999u);
        CHECK(plan.correct && plan.remain == 127u);
    }

    // 整删只累计消失堆；全部删除与 sum_vanished > count 均不回写。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 199u), s2_test_pre(b, 50u)};
        const inventory_stage::RemoveDataPost post[] = {{nullptr, 0u, 0u}, {nullptr, 0u, 0u}};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 2, 249).correct);
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 2, 9999).correct);
    }
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u)};
        const inventory_stage::RemoveDataPost post[] = {{nullptr, 0u, 0u}};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 149).correct);
    }

    // 消失堆累计扣除后，剩余堆仍按其唯一 b-only 形态修正。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u), s2_test_pre(b, 300u)};
        const inventory_stage::RemoveDataPost post[] = {
            {nullptr, 0u, 0u},
            s2_test_post(b, s2_test_b_only_raw(pre[1].pre_raw, 200u))};
        const inventory_stage::RemoveDataPlan plan = inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 2, 250);
        CHECK(plan.correct && plan.entry_index == 1 && plan.remain == 200u);
    }

    // 两个堆都呈现同一 b-only 结果时，多候选 fail-closed。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u), s2_test_pre(b, 150u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 125u)),
            s2_test_post(b, s2_test_b_only_raw(pre[1].pre_raw, 125u))};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 2, 25).correct);
    }

    // 非候选同对象槽 raw 改变时 fail-closed，包括数量段以外的位变化。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u), s2_test_pre(b, 300u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 125u)),
            s2_test_post(b, pre[1].pre_raw ^ 1u)};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 2, 25).correct);
    }
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 124u))};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 25).correct);
    }

    // 没有部分删除、对象替换、count<=0 和退化输入均不回写。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 199u)};
        const inventory_stage::RemoveDataPost unchanged[] = {s2_test_post(a, pre[0].pre_raw)};
        const inventory_stage::RemoveDataPost replaced[] = {s2_test_post(c, s2_test_raw(1u))};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, unchanged, 1, 1).correct);
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, unchanged, 1, 0).correct);
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, unchanged, 1, -1).correct);
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, replaced, 1, 1).correct);
    }
    CHECK(!inventory_stage::remove_item_data_plan(
        &kStage4ConversionApi, nullptr, nullptr, 0, 5).correct);
    CHECK(!inventory_stage::remove_item_data_plan(nullptr, nullptr, nullptr, 0, 5).correct);

    // 转换 API 缺失或失败时不猜测原版写回形态。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 150u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 125u))};
        qol::contracts::QolItemCountApi missing_api = kStage4ConversionApi;
        missing_api.field_from_canonical = nullptr;
        qol::contracts::QolItemCountApi failed_api = kStage4ConversionApi;
        failed_api.field_from_canonical = &fake_field_from_canonical_failure;
        CHECK(!inventory_stage::remove_item_data_plan(&missing_api, pre, post, 1, 25).correct);
        CHECK(!inventory_stage::remove_item_data_plan(&failed_api, pre, post, 1, 25).correct);
    }

    // 原版形态已将 a+b 表达为 remain 时无需回写。
    {
        const inventory_stage::RemoveDataEntry pre[] = {s2_test_pre(a, 50u)};
        const inventory_stage::RemoveDataPost post[] = {
            s2_test_post(a, s2_test_b_only_raw(pre[0].pre_raw, 40u))};
        CHECK(!inventory_stage::remove_item_data_plan(
            &kStage4ConversionApi, pre, post, 1, 10).correct);
    }
}

// H-21 扩展桥接（R-56）补扣量纯函数：物理实扣 = 前后 H-03 总数差，扩展补扣 =
// max(0, requested - physical_removed)。覆盖足额/不足/全扩展/退化入参。
static void test_remove_data_extension_shortfall() {
    // 物理足额（含恰好全扣）→ 不补扣。
    CHECK(inventory_stage::remove_data_extension_shortfall(10, 5, 3) == 0);
    CHECK(inventory_stage::remove_data_extension_shortfall(8, 3, 5) == 0);
    CHECK(inventory_stage::remove_data_extension_shortfall(10, 0, 10) == 0);
    // 物理不足 → 补差额。
    CHECK(inventory_stage::remove_data_extension_shortfall(10, 8, 5) == 3);
    CHECK(inventory_stage::remove_data_extension_shortfall(2, 0, 5) == 3);
    // 材料全在扩展袋（原版一个物理都没扣）→ 全额补扣。
    CHECK(inventory_stage::remove_data_extension_shortfall(5, 5, 5) == 5);
    CHECK(inventory_stage::remove_data_extension_shortfall(0, 0, 1) == 1);
    // requested<=0 不补扣。
    CHECK(inventory_stage::remove_data_extension_shortfall(5, 5, 0) == 0);
    CHECK(inventory_stage::remove_data_extension_shortfall(5, 5, -1) == 0);
    // 数据反向（after>before）按物理零扣处理，返回全额，绝不产生负补扣。
    CHECK(inventory_stage::remove_data_extension_shortfall(5, 6, 4) == 4);
}

// VM-41：原版背包详情出售接管判定 + 金额（R-55）。调用点常量与 game_symbols.h
// 同源；启用/关闭两态、canonical 0/1/99/100/199/999、越界拒绝全覆盖。
static void test_vanilla_sell_routing() {
    // 结算点常量与反汇编证据一致（0xb83d0 回调 / desc_type==2 背包详情）。
    CHECK(F_UIEQUIP_OK_DESTROY_ITEM_VMA == 0xb83d0u);
    CHECK(F_UIEQUIP_BUTTON_DESTROY_EXE_VMA == 0xb6240u);
    CHECK(kUIEquipBagDescType == 2u);
    CHECK(kVanillaSellPreviewFlag == 0x666u);

    // c18 0x1260c4 结算：((unit*65)/100)*count，先截断再乘数量（17→11，×228→2508）。
    CHECK(c18_detail_sell_price(17, 228) == 2508);
    CHECK(c18_detail_sell_price(4, 3) == 6);
    CHECK(c18_detail_sell_price(0, 1) == 0);
    CHECK(c18_detail_sell_price(1, 0) == 0);

    // 原版可表示数量、非 count-encoded 物品与任务袋均透传；S2 编码才接管。
    const uint32_t canonical_counts[]{1u, 99u, 100u, 127u, 128u, 199u, 999u};
    for (size_t index = 0; index < 4; ++index) {
        CHECK(vanilla_sell_route(0, true, canonical_counts[index], true) ==
              VanillaSellRoute::kBackup);
        CHECK(vanilla_sell_route(0, true, canonical_counts[index], false) ==
              VanillaSellRoute::kBackup);
    }
    for (size_t index = 4; index < 7; ++index) {
        CHECK(vanilla_sell_route(0, true, canonical_counts[index], true) ==
              VanillaSellRoute::kPreview);
        CHECK(vanilla_sell_route(0, true, canonical_counts[index], false) ==
              VanillaSellRoute::kTakeover);
    }
    CHECK(vanilla_sell_route(0, false, 199u, true) == VanillaSellRoute::kBackup);
    CHECK(vanilla_sell_route(5, true, 199u, true) == VanillaSellRoute::kBackup);

    qol::contracts::QolItemDetailContext preview{2, 1, 4, {17, 3}};
    auto confirmation = preview;
    CHECK(vanilla_sell_detail_context_matches(preview, confirmation));
    confirmation.bag = 2;
    CHECK(!vanilla_sell_detail_context_matches(preview, confirmation));
    confirmation = preview;
    confirmation.slot = 5;
    CHECK(!vanilla_sell_detail_context_matches(preview, confirmation));
    confirmation = preview;
    confirmation.item.generation = 4;
    CHECK(!vanilla_sell_detail_context_matches(preview, confirmation));
    confirmation = preview;
    confirmation.item.slot = 0;
    CHECK(!vanilla_sell_detail_context_matches(preview, confirmation));
}

int main() {
    test_s2_writeback_applied_gate();
    test_queries();
    test_original_only_queries();
    test_get_cumulate_count_facade();
    test_remove_item_data_plan();
    test_remove_data_extension_shortfall();
    test_vanilla_sell_routing();
    test_object_operations();
    test_unequip_to_inven();
    test_install_transaction();
    std::printf("stage4_hook_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
