#include "feature/patch/inventory_hook_stage4.h"

#include "data/native/game_symbols.h"

#include <cstdint>
#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

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
        return stage4_have_item(category, recursive_original, extension_count, *g_recursive_guard);
    }
    return 0;
}

static int g_empty_original = 0;
static bool g_empty_extension = false;
static int g_empty_original_calls = 0;
static int g_empty_extension_calls = 0;
static int empty_original(int32_t, int32_t) { ++g_empty_original_calls; return g_empty_original; }
static bool empty_extension(int32_t, int32_t) { ++g_empty_extension_calls; return g_empty_extension; }

static void test_original_only_queries() {
    bool guard = false;
    g_original_value = 0;
    g_original_calls = 0;
    g_extension_calls = 0;
    CHECK(stage4_have_item_original_only(1, original_count, guard) == 0);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);

    g_original_value = 4;
    g_original_calls = 0;
    CHECK(stage4_get_item_count_original_only(1, original_count, guard) == 4);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);

    g_empty_original = 1;
    g_empty_original_calls = 0;
    g_empty_extension_calls = 0;
    CHECK(stage4_is_having_empty_slot_original_only(1, 1, empty_original, guard) == 1);
    CHECK(g_empty_original_calls == 1 && g_empty_extension_calls == 0 && !guard);
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
    CHECK(stage4_have_item(1, original_count, extension_count, guard) == 1);
    CHECK(g_original_calls == 1 && g_extension_calls == 0 && !guard);

    g_original_value = 0; g_extension_value = 5; g_original_calls = 0; g_extension_calls = 0;
    CHECK(stage4_have_item(1, original_count, extension_count, guard) == 1);
    CHECK(g_original_calls == 1 && g_extension_calls == 1 && !guard);

    g_original_value = 4; g_extension_value = 6; g_original_calls = 0; g_extension_calls = 0;
    CHECK(stage4_get_item_count(1, original_count, extension_count, guard) == 10);
    CHECK(g_original_calls == 1 && g_extension_calls == 1 && !guard);

    g_empty_original = 1; g_empty_extension = false; g_empty_original_calls = 0; g_empty_extension_calls = 0;
    CHECK(stage4_is_having_empty_slot(1, 0, empty_original, empty_extension, guard) == 1);
    CHECK(g_empty_extension_calls == 0 && !guard);

    g_empty_original = 0; g_empty_extension = true; g_empty_original_calls = 0; g_empty_extension_calls = 0;
    CHECK(stage4_is_having_empty_slot(2, 1, empty_original, empty_extension, guard) == 1);
    CHECK(g_empty_original_calls == 1 && g_empty_extension_calls == 1 && !guard);
    // needed<=0 = 无需新槽（物品全可叠进现有堆）：原版返回 1（可放），
    // 不得误报满（0x103460 反汇编 0x10347c b.le → 0x1035d8 return 1）。
    CHECK(stage4_is_having_empty_slot(0, 1, empty_original, empty_extension, guard) == 1);
    CHECK(g_empty_extension_calls == 1 && !guard);

    g_original_calls = 0; g_extension_calls = 0; g_recursive_guard = &guard;
    CHECK(stage4_have_item(1, recursive_original, extension_count, guard) == 1);
    CHECK(g_original_calls == 2 && g_extension_calls == 1 && !guard);
    g_recursive_guard = nullptr;
}

static void test_object_operations() {
    bool guard = false;
    g_identify_calls = 0; g_extension_action_calls = 0; g_backup_action_calls = 0;
    g_extension_consume_result = true;
    CHECK(stage4_consume_item(g_extension_item, identify_item, extension_consume, backup_consume,
                              guard));
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);
    CHECK(stage4_consume_item(reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999)),
                              identify_item, extension_consume, backup_consume, guard));
    CHECK(g_backup_action_calls == 1 && !guard);

    g_extension_consume_result = false;
    g_extension_action_calls = 0; g_backup_action_calls = 0;
    CHECK(!stage4_consume_item(g_extension_item, identify_item, extension_consume, backup_consume,
                               guard));
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);

    g_extension_action_calls = 0; g_backup_action_calls = 0;
    CHECK(stage4_remove_item(g_extension_item, identify_item, remove_item_extension,
                             backup_remove, guard) == 1);
    CHECK(g_extension_action_calls == 1 && g_backup_action_calls == 0 && !guard);
    CHECK(stage4_remove_item(reinterpret_cast<void*>(static_cast<uintptr_t>(0x9999)),
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
    CHECK(stage4_is_extension_equip_control_source(0x04, true, true, true));
    CHECK(!stage4_is_extension_equip_control_source(0x04, true, true, false));
    CHECK(!stage4_is_extension_equip_control_source(0x04, true, false, true));
    CHECK(!stage4_is_extension_equip_control_source(0x04, false, true, true));
    CHECK(!stage4_is_extension_equip_control_source(0x02, true, true, true));
    CHECK(stage4_is_extension_equip_control_source(0x04, true, true, true));

    // S-03/R-50 apply 准入判据：宝石/卷轴 + 装备类目标 + IsApplyStuff 判真才进入
    // apply；宝石/卷轴 + 普通物品目标必须非 apply（回退扩展交换/移动）；普通物品
    // 源 + 任意目标一律非 apply；类别数据不可用由调用方映射为 target_is_equip=false，
    // 与普通物品目标同路径 fail-closed。
    CHECK(stage4_is_extension_apply_candidate(true, true, true, true));
    CHECK(stage4_is_extension_apply_candidate(true, true, true, false) == false);
    CHECK(stage4_is_extension_apply_candidate(true, true, false, true) == false);
    CHECK(stage4_is_extension_apply_candidate(true, true, false, false) == false);
    CHECK(stage4_is_extension_apply_candidate(false, true, true, true) == false);
    CHECK(stage4_is_extension_apply_candidate(true, false, true, true) == false);
    CHECK(stage4_is_extension_apply_candidate(false, false, false, false) == false);
    CHECK(stage4_finish_requires_abort(false));
    CHECK(!stage4_finish_requires_abort(true));
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

// H-17 读侧分流（R-45/R-46/R-47/R-49）：仅 count-encoded 类别按模式视图解码
//（启用态 128a+b、关闭态只读 b），非可堆叠与 kUnknown fail-closed 走 backup；
// kEncoded 不先调 backup（§3.2 1.1 例外）。
static void test_get_cumulate_count_s2() {
    void* nonnull_item = reinterpret_cast<void*>(static_cast<uintptr_t>(0x417));
    using stack_codec::CountEncoding;
    using stack_codec::s2_write_count;

    // kEncoded 启用态：S2 全量解码直达，backup 保持未调用（数量 0/99/100/127/128/199/999/1023）。
    const uint32_t counts[] = {0u, 99u, 100u, 127u, 128u, 199u, 999u, 1023u};
    for (const uint32_t count : counts) {
        g_cumulate_backup_calls = 0;
        g_cumulate_backup_result = -1;
        CHECK(stage4_get_cumulate_count(nonnull_item, s2_write_count(0u, count),
                                        CountEncoding::kEncoded,
                                        cumulate_backup, true) == static_cast<int>(count));
        CHECK(g_cumulate_backup_calls == 0);
    }
    // kEncoded 关闭态（R-47 决策 b）：只读 b 段（count mod 128），a 不读不参与。
    for (const uint32_t count : counts) {
        g_cumulate_backup_calls = 0;
        g_cumulate_backup_result = -1;
        CHECK(stage4_get_cumulate_count(nonnull_item, s2_write_count(0u, count),
                                        CountEncoding::kEncoded,
                                        cumulate_backup, false) ==
              static_cast<int>(count & 0x7Fu));
        CHECK(g_cumulate_backup_calls == 0);
    }
    // 模式切换不丢值：canonical 199 关闭态读 71，重开读 199。
    CHECK(stage4_get_cumulate_count(nonnull_item, s2_write_count(0u, 199u),
                                    CountEncoding::kEncoded, cumulate_backup, false) == 71);
    CHECK(stage4_get_cumulate_count(nonnull_item, s2_write_count(0u, 199u),
                                    CountEncoding::kEncoded, cumulate_backup, true) == 199);
    // a 段进位边界：127 = b 满（b=127<<25）、128 = a=1（1<<22）、999 = b=103、a=7。
    CHECK(s2_write_count(0u, 127u) == (127u << 25));
    CHECK(s2_write_count(0u, 128u) == (1u << 22));
    CHECK(s2_write_count(0u, 999u) == ((103u << 25) | (7u << 22)));

    // 非可堆叠：original-first backup（装备 marker/宝石选项/袋容量不解释 bits22–31）。
    g_cumulate_backup_calls = 0; g_cumulate_backup_result = 1;
    CHECK(stage4_get_cumulate_count(nonnull_item, 0xFFFFFFFFu, CountEncoding::kNotEncoded,
                                    cumulate_backup, true) == 1);
    CHECK(g_cumulate_backup_calls == 1);

    // fail-closed：类别表不可用（kUnknown）必须走 backup，不得按可堆叠解码。
    g_cumulate_backup_calls = 0; g_cumulate_backup_result = 1;
    CHECK(stage4_get_cumulate_count(nonnull_item, 0xFFFFFFFFu, CountEncoding::kUnknown,
                                    cumulate_backup, false) == 1);
    CHECK(g_cumulate_backup_calls == 1);

    // 空指针：原版语义返回 0（backup(nullptr)）。
    g_cumulate_backup_calls = 0; g_cumulate_backup_result = 0;
    CHECK(stage4_get_cumulate_count(nullptr, 0u, CountEncoding::kUnknown,
                                    cumulate_backup, true) == 0);
    CHECK(g_cumulate_backup_calls == 1);

    // backup 缺失兜底：非可堆叠/未知返回 0，不崩。
    CHECK(stage4_get_cumulate_count(nonnull_item, 0u, CountEncoding::kNotEncoded, nullptr, true) == 0);
    CHECK(stage4_get_cumulate_count(nonnull_item, 0u, CountEncoding::kUnknown, nullptr, false) == 0);
}

// H-21 INVEN_RemoveItemData 修正计划（R-45/R-49）：部分删堆 remain 修正、
// 整删消失堆求和、count<=0 / 无缩减 / 多缩减 / 越域 / 已正确各分支 fail-closed。
static void test_remove_item_data_plan() {
    void* a = reinterpret_cast<void*>(static_cast<uintptr_t>(0x501));
    void* b = reinterpret_cast<void*>(static_cast<uintptr_t>(0x502));
    void* c = reinterpret_cast<void*>(static_cast<uintptr_t>(0x503));
    using stack_codec::s2_write_count;

    // 场景 1（启用态跨 127 借位，单一部分删堆）：堆 199 + 堆 50，删 60。
    // 原版顺序（0x1040a8）：60 < 199 → 首堆即部分删，remain=199+0-60=139>127，
    // b 写截断（post_view = a 残留 0 + b=11），堆 2 不动。计划应修正 entry 0 → 139。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 50u}};
        Stage4RemoveDataPost post[] = {{a, 11u}, {b, 50u}};
        const Stage4RemoveDataPlan plan = stage4_remove_item_data_plan(pre, post, 2, 60);
        CHECK(plan.correct && plan.entry_index == 0 && plan.remain == 139u);
    }
    // 场景 1b（整删后跨堆部分删）：堆 199 + 堆 50，删 220：199 整删（w22=199），
    // 220 ≥ 199 → 继续遍历；220 < 249 → 堆 2 部分删 remain=199+50-220=29，
    // 原版 b 写 29（a=0 保留）→ 已正确，不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 50u}};
        Stage4RemoveDataPost post[] = {{nullptr, 0u}, {b, 29u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 220).correct);
    }
    // 场景 2（remain ≤127 但旧 a 残留）：堆 199 部分删 190 → remain=9，
    // 原版 b 写后 post_view = 旧 a(1)*128 + 9 = 137 ≠ 9 → 修正为 9。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}};
        Stage4RemoveDataPost post[] = {{a, 137u}};
        const Stage4RemoveDataPlan plan = stage4_remove_item_data_plan(pre, post, 1, 190);
        CHECK(plan.correct && plan.entry_index == 0 && plan.remain == 9u);
    }
    // 场景 3（remain ≤127 且 a 原本为 0）：堆 50 删 10 → remain=40，原版已写对
    //（post_view=40）→ 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 50u}};
        Stage4RemoveDataPost post[] = {{a, 40u}};
        const Stage4RemoveDataPlan plan = stage4_remove_item_data_plan(pre, post, 1, 10);
        CHECK(!plan.correct);
    }
    // 场景 4（全部整删，无部分删堆）：count ≥ 总量 → 无修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 50u}};
        Stage4RemoveDataPost post[] = {{nullptr, 0u}, {nullptr, 0u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 249).correct);
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 9999).correct);
    }
    // 场景 5（count<=0：语义未冻结/空操作）→ 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}};
        Stage4RemoveDataPost post[] = {{a, 137u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 1, 0).correct);
        CHECK(!stage4_remove_item_data_plan(pre, post, 1, -1).correct);
    }
    // 场景 6（fail-closed：缩减堆不唯一）→ 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 199u}};
        Stage4RemoveDataPost post[] = {{a, 100u}, {b, 100u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 198).correct);
    }
    // 场景 7（fail-closed：未触堆数量被改/增长）→ 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 50u}};
        Stage4RemoveDataPost post[] = {{nullptr, 0u}, {b, 70u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 60).correct);
    }
    // 场景 8（fail-closed：count 小于整删累计，数据矛盾）→ 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 50u}};
        Stage4RemoveDataPost post[] = {{nullptr, 0u}, {b, 61u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 10).correct);
    }
    // 场景 9（fail-closed：deleted_here 恰等于部分删堆全量——原版该走整删分支，
    // remain=0 矛盾）→ 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 199u}, {b, 50u}};
        Stage4RemoveDataPost post[] = {{nullptr, 0u}, {b, 0u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 2, 249).correct);
    }
    // 场景 10（堆未被触及时原样通过）：无缩减 → 不修正。
    {
        Stage4RemoveDataEntry pre[] = {{a, 999u}};
        Stage4RemoveDataPost post[] = {{a, 999u}};
        CHECK(!stage4_remove_item_data_plan(pre, post, 1, 1).correct);
    }
    // 场景 11（S2 编码交叉验证：139 = a1 + b11、9 = a0 + b9）。
    CHECK(s2_write_count(0u, 139u) == ((1u << 22) | (11u << 25)));
    CHECK(s2_write_count(0u, 9u) == (9u << 25));
    // 场景 12（空/退化入参）→ 不修正。
    CHECK(!stage4_remove_item_data_plan(nullptr, nullptr, 0, 5).correct);
}

// 原版装备页出售/销毁结算数量语义（0x1261c4 直读 b 段 + clamp）：把 canonical
// 编码成原生 +0x10 字段后，函数必须复现实测表（199→71→546 的价格基数）。
static void test_native_equip_sell_count() {
    using stack_codec::s2_write_count;
    const auto field = [](uint32_t canonical) { return s2_write_count(0u, canonical); };
    // 实测：100..129→1（b∈{0,1} 或 b=100..127），130→2，199→71，227→99，228→1。
    CHECK(native_equip_sell_count(field(100u)) == 1u);
    CHECK(native_equip_sell_count(field(128u)) == 1u);
    CHECK(native_equip_sell_count(field(129u)) == 1u);
    CHECK(native_equip_sell_count(field(130u)) == 2u);
    CHECK(native_equip_sell_count(field(199u)) == 71u);
    CHECK(native_equip_sell_count(field(227u)) == 99u);
    CHECK(native_equip_sell_count(field(228u)) == 1u);
    CHECK(native_equip_sell_count(field(999u)) == 1u);
    // 裸字段边界：b=0（128 的 a 段单独）与 b=100（228）都必须回退 1。
    CHECK(native_equip_sell_count(0u) == 1u);
    CHECK(native_equip_sell_count(1u << 22) == 1u);
    CHECK(native_equip_sell_count(100u << 25) == 1u);
    CHECK(native_equip_sell_count(99u << 25) == 99u);
}

// 装备页详情出售结算点（0x1261c4）重定向指令映射（VM-37 装备页分支）：
// 反汇编原字节 → 重定向表条目逐位固定；替换 arg 必须是 mov x0,x21（物品指针在
// x21），不能套用其它点的 mov x0,x19。重定向表（game_patch_core.inc）与 Host
// 共用 inventory_hook_stage4.h 的同一常量，避免表与证据漂移。
static void test_equip_sell_redirect_mapping() {
    CHECK(kEquipSellRedirectArgOriginal == 0xb94012a0u);   // ldr w0,[x21,#0x10]
    CHECK(kEquipSellRedirectCallOriginal == 0x940068c8u);  // bl UTIL_GetBitValue
    CHECK(kEquipSellRedirectArgReplacement == (0xaa0003e0u | (21u << 16)));  // mov x0,x21
    CHECK(stage4_equip_sell_redirect_matches(kEquipSellRedirectArgOriginal,
                                             kEquipSellRedirectCallOriginal));
    // x19 版本的 arg 原字节（商店/拆堆点）不得被装备页条目误匹配。
    CHECK(!stage4_equip_sell_redirect_matches(0xb9401260u, kEquipSellRedirectCallOriginal));
    CHECK(!stage4_equip_sell_redirect_matches(kEquipSellRedirectArgOriginal, 0x9401bb13u));
    // 重定向语义：canonical 199（a=1,b=71）原版读 71（546 缺陷基线），
    // 重定向后的 getter 读 199。
    const uint32_t field = stack_codec::s2_write_count(0u, 199u);
    CHECK(native_equip_sell_count(field) == 71u);
    CHECK(stack_codec::effective_read_count(field, true) == 199u);
}

// VM-38：ITEMSYSTEM_MakeItem 数量回写计划——arg2 是静态表查找/品质参数
//（CHARSYSTEM_DropItem 传 2..5、DEALSYSTEM_MakeSale 传 5）而非数量，产物量由
// 原版 CAL 公式写点（≤99，b 写即全量）生成。任何入参组合都不得触发回写，
// 否则掉落物一落地数量即被污染成 arg2（真机实证：药水 2/卷轴 3/材料 4）。
static void test_make_item_writeback_count() {
    // 掉落品质分支实参组合（category 2/3/4 与 arg2 2..5 的交叉）。
    for (int32_t category = 2; category <= 4; ++category) {
        for (int32_t arg2 = 2; arg2 <= 5; ++arg2) {
            CHECK(stage4_make_item_writeback_count(category, arg2, 0) == 0u);
            CHECK(stage4_make_item_writeback_count(category, arg2, 1) == 0u);
        }
    }
    // 商店货架（MakeSale arg2=5）与退化/边界入参。
    CHECK(stage4_make_item_writeback_count(1, 5, 0) == 0u);
    CHECK(stage4_make_item_writeback_count(0, 0, 0) == 0u);
    CHECK(stage4_make_item_writeback_count(-1, 99, 127) == 0u);
    CHECK(stage4_make_item_writeback_count(401, 999, -1) == 0u);
}

// VM-41：原版背包详情出售接管判定 + 金额（R-55）。调用点常量与 game_symbols.h
// 同源；启用/关闭两态、canonical 0/1/99/100/199/999、越界拒绝全覆盖。
static void test_vanilla_sell_takeover() {
    // 结算点常量与反汇编证据一致（0xb83d0 回调 / desc_type==2 背包详情）。
    CHECK(F_UIEQUIP_OK_DESTROY_ITEM_VMA == 0xb83d0u);
    CHECK(F_UIEQUIP_BUTTON_DESTROY_EXE_VMA == 0xb6240u);
    CHECK(kUIEquipBagDescType == 2u);

    // 关闭态：预演与真实结算都 backup（原版逐指令不变）。
    CHECK(vanilla_sell_route(false, true) == VanillaSellRoute::kBackup);
    CHECK(vanilla_sell_route(false, false) == VanillaSellRoute::kBackup);

    // 启用态：按钮预演只回填展示金额；弹窗 OK 真实接管。
    CHECK(vanilla_sell_route(true, true) == VanillaSellRoute::kPreview);
    CHECK(vanilla_sell_route(true, false) == VanillaSellRoute::kTakeover);

    // 金额：unit × canonical × 7 / 10。
    int64_t price = -1;
    CHECK(vanilla_sell_money(10, 1u, &price) && price == 7);
    CHECK(vanilla_sell_money(10, 99u, &price) && price == 693);
    CHECK(vanilla_sell_money(10, 100u, &price) && price == 700);
    CHECK(vanilla_sell_money(10, 199u, &price) && price == 1393);
    CHECK(vanilla_sell_money(10, 999u, &price) && price == 6993);
    // canonical 0 拒绝；越界 canonical 按启用态上限 999 收敛。
    CHECK(!vanilla_sell_money(10, 0u, &price));
    CHECK(vanilla_sell_money(10, 1023u, &price) && price == 6993);
    CHECK(vanilla_sell_money(10, 1000u, &price) && price == 6993);
    // 越界拒绝：负单价 / 结果超出 int32 / 空出参。
    CHECK(!vanilla_sell_money(-1, 10u, &price));
    CHECK(!vanilla_sell_money(INT32_MAX, 999u, &price));
    CHECK(!vanilla_sell_money(10, 10u, nullptr));
    // 单价 0 合法（金额 0），不得因结果 0 被误拒。
    CHECK(vanilla_sell_money(0, 10u, &price) && price == 0);
}

int main() {
    test_queries();
    test_original_only_queries();
    test_get_cumulate_count_s2();
    test_remove_item_data_plan();
    test_native_equip_sell_count();
    test_equip_sell_redirect_mapping();
    test_make_item_writeback_count();
    test_vanilla_sell_takeover();
    test_object_operations();
    test_unequip_to_inven();
    test_install_transaction();
    std::printf("stage4_hook_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
