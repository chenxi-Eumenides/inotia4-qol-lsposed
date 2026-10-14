// custom_recipe 单 owner 结果层：production feature + typed handles/write port 的 handle-chain 覆盖。
//
// 覆盖 CR01（分阶段失败/已扣状态/无双重释放）、CR02（quality 不可用与非宝石区分）、
// CR03（闭区间随机与错误传播）、CR08（SaveEnter 重建/停用作废 pending、identity 校验）、
// CR10（need 写入顺序与 jewel 最后提交）。
//
// 本目标链接真实 plugins/custom_recipe/feature.cpp；表注入与游戏表由测试替身提供。

#include "plugins/custom_recipe/feature.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include "contracts/attribute_quality.h"
#include "contracts/craft_commands.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/plugin.h"
#include "plugins/custom_recipe/catalog.h"
#include "plugins/custom_recipe/result.h"
#include "plugins/custom_recipe/table.h"

namespace {

using namespace qol::contracts;

// ---- typed identities ----
constexpr QolItemHandle kMaterialA{10, 2};   // 类别 35（可堆叠）
constexpr QolItemHandle kMaterialB{11, 3};   // 类别 41（可堆叠）
constexpr QolItemHandle kJewel{12, 4};       // 类别 28（可堆叠）
constexpr QolItemHandle kScrollWeapon{13, 5};  // 类别 20（可堆叠）
constexpr QolItemHandle kScrollArmor{14, 6};   // 类别 25（可堆叠）
constexpr QolItemHandle kEquipAlpha{15, 7};    // 类别 485（不可堆叠）
constexpr QolItemHandle kEquipBeta{16, 8};     // 类别 485（不可堆叠）
constexpr QolItemHandle kPotion{17, 9};        // 类别 14（可堆叠）
constexpr QolItemHandle kProduct{50, 8};

QolResult ok() { return qol::contracts::ok(); }
QolResult fail(int32_t code) { return qol::contracts::error(code, "test failure", 12); }
bool same(QolItemHandle a, QolItemHandle b) {
    return a.slot == b.slot && a.generation == b.generation;
}

// ---- 类别 / 堆叠属性表 ----
struct ItemFact {
    QolItemHandle handle;
    int32_t category;
    bool stackable;
};
constexpr ItemFact kItemFacts[] = {
    {kMaterialA, 35, true},  {kMaterialB, 41, true},   {kJewel, 28, true},
    {kScrollWeapon, 20, true}, {kScrollArmor, 25, true}, {kEquipAlpha, 485, false},
    {kEquipBeta, 485, false}, {kPotion, 14, true},
};

const ItemFact* find_fact(QolItemHandle item) {
    for (const ItemFact& fact : kItemFacts) {
        if (same(fact.handle, item)) return &fact;
    }
    return nullptr;
}

// ---- 世界状态 ----
struct HeldCount {
    int32_t category;
    int32_t count;
};
HeldCount g_held[8]{};
int g_held_size = 0;
int32_t held_of(int32_t category) {
    for (int i = 0; i < g_held_size; ++i) {
        if (g_held[i].category == category) return g_held[i].count;
    }
    return 0;
}
void add_held(int32_t category, int32_t count) {
    for (int i = 0; i < g_held_size; ++i) {
        if (g_held[i].category == category) {
            g_held[i].count += count;
            return;
        }
    }
    if (g_held_size < 8) {
        g_held[g_held_size].category = category;
        g_held[g_held_size].count = count;
        ++g_held_size;
    }
}

QolItemHandle g_slots[3] = {kInvalidItemHandle, kInvalidItemHandle, kInvalidItemHandle};
bool g_stuff_slot_available = true;
int32_t g_read_stuff_fail_index = -1;
int32_t g_character_level = 50;

// ---- 调用记录 / 失败注入 ----
int g_remove_item_calls = 0;
QolItemHandle g_removed[8]{};
int g_remove_item_data_calls = 0;
int32_t g_remove_item_data_category[8]{};
int32_t g_remove_item_fail_slot = -1;   // 命中该 slot 的 remove_item 返回失败
int g_remove_item_data_fail_call = -1;  // 第 N 次 remove_item_data 失败（0-based）
bool g_observation_skip_decrement = false;  // 扣料成功但不减计数（模拟计数不符）
bool g_observation_read_fail = false;
int g_held_read_calls = 0;
int g_held_read_fail_after = -1;  // >0：前 N 次成功，之后读取失败（观察阶段失败注入）
int g_create_calls = 0;
int32_t g_create_result = kQolOk;
bool g_create_invalid_handle = false;
int g_free_calls = 0;
QolItemHandle g_freed{};
int32_t g_free_result = kQolOk;
int g_save_calls = 0;
int32_t g_save_result = kQolOk;
int32_t g_save_saved = 1;
int g_jewel_write_calls = 0;
uint32_t g_jewel_word_written = 0;
int32_t g_jewel_write_result = kQolOk;
int g_read_jewel_word_calls = 0;
uint32_t g_jewel_word = 20u | (3u << 18) | (5u << 11);
int g_set_max_socket_calls = 0;
int32_t g_set_max_socket_result = kQolOk;
int g_stuff_need_calls = 0;
int32_t g_stuff_need_fail_at = -1;
int g_random_calls = 0;
int32_t g_random_result_code = kQolOk;
int32_t g_random_out_override = std::numeric_limits<int32_t>::min();

int g_sequence = 0;
int g_jewel_write_sequence = 0;
int g_stuff_need_sequence[8]{};
int g_create_sequence = 0;
int g_save_sequence = 0;
int g_product_scale_sequence = 0;
int g_remove_sequence[3]{};

void reset_env() {
    g_held_size = 0;
    add_held(4, 5);
    add_held(35, 5);
    add_held(41, 5);
    add_held(28, 5);
    add_held(20, 5);
    add_held(25, 5);
    add_held(485, 4);
    add_held(14, 5);
    g_slots[0] = g_slots[1] = g_slots[2] = kInvalidItemHandle;
    g_stuff_slot_available = true;
    g_read_stuff_fail_index = -1;
    g_character_level = 50;

    g_remove_item_calls = 0;
    g_removed[0] = g_removed[1] = g_removed[2] = kInvalidItemHandle;
    g_remove_item_data_calls = 0;
    g_remove_item_fail_slot = -1;
    g_remove_item_data_fail_call = -1;
    g_observation_skip_decrement = false;
    g_observation_read_fail = false;
    g_held_read_calls = 0;
    g_held_read_fail_after = -1;
    g_create_calls = 0;
    g_create_result = kQolOk;
    g_create_invalid_handle = false;
    g_free_calls = 0;
    g_freed = kInvalidItemHandle;
    g_free_result = kQolOk;
    g_save_calls = 0;
    g_save_result = kQolOk;
    g_save_saved = 1;
    g_jewel_write_calls = 0;
    g_jewel_word_written = 0;
    g_jewel_write_result = kQolOk;
    g_read_jewel_word_calls = 0;
    g_jewel_word = 20u | (3u << 18) | (5u << 11);
    g_set_max_socket_calls = 0;
    g_set_max_socket_result = kQolOk;
    g_stuff_need_calls = 0;
    g_stuff_need_fail_at = -1;
    g_random_calls = 0;
    g_random_result_code = kQolOk;
    g_random_out_override = std::numeric_limits<int32_t>::min();
    g_sequence = 0;
    g_jewel_write_sequence = 0;
    g_create_sequence = 0;
    g_save_sequence = 0;
    g_product_scale_sequence = 0;
    for (int& value : g_remove_sequence) value = 0;
    for (int& value : g_stuff_need_sequence) value = 0;
}

// ---- capability 替身 ----
QolResult item_category(QolItemHandle item, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    const ItemFact* fact = find_fact(item);
    if (fact == nullptr) return fail(kQolErrNotFound);
    *out = fact->category;
    return ok();
}

QolResult item_is_real_equip(QolItemHandle item, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    const ItemFact* fact = find_fact(item);
    if (fact == nullptr) return fail(kQolErrNotFound);
    *out = fact->stackable ? 0 : 1;
    return ok();
}

QolResult is_jewel_category(int32_t category, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = category >= 28 && category <= 32 ? 1 : 0;
    return ok();
}

QolResult held_count(int32_t category, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    ++g_held_read_calls;
    if (g_observation_read_fail) return fail(kQolErrNotReady);
    if (g_held_read_fail_after >= 0 && g_held_read_calls > g_held_read_fail_after) {
        return fail(kQolErrNotReady);
    }
    *out = held_of(category);
    return ok();
}

QolResult character_level(int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = g_character_level;
    return ok();
}

QolResult stuff_item(uint32_t index, QolItemHandle* out) {
    if (out == nullptr || index >= 3) return fail(kQolErrInvalidArgument);
    if (static_cast<int32_t>(index) == g_read_stuff_fail_index) return fail(kQolErrNotReady);
    *out = g_slots[index];
    return ok();
}

QolResult slot_available(uint32_t index, int32_t* out) {
    if (index >= 3 || out == nullptr) return fail(kQolErrInvalidArgument);
    *out = g_stuff_slot_available ? 1 : 0;
    return ok();
}

QolResult remove_item(QolItemHandle item) {
    const ItemFact* fact = find_fact(item);
    for (int i = 0; i < 3; ++i) {
        if (same(g_slots[i], item)) {
            if (g_remove_item_fail_slot == i) return fail(kQolErrBlocked);
            break;
        }
    }
    if (g_remove_item_calls < 8) g_removed[g_remove_item_calls] = item;
    if (g_remove_item_calls < 3) g_remove_sequence[g_remove_item_calls] = ++g_sequence;
    ++g_remove_item_calls;
    if (!g_observation_skip_decrement && fact != nullptr) add_held(fact->category, -1);
    return ok();
}

QolResult remove_item_data(int32_t category, int32_t) {
    const int call = g_remove_item_data_calls;
    if (call < 3) g_remove_sequence[call] = ++g_sequence;
    ++g_remove_item_data_calls;
    if (g_remove_item_data_fail_call == call) return fail(kQolErrBlocked);
    if (!g_observation_skip_decrement) add_held(category, -1);
    return ok();
}

QolResult create_perfect_item(int32_t, QolItemHandle* out) {
    ++g_create_calls;
    g_create_sequence = ++g_sequence;
    if (g_create_result != kQolOk) return fail(g_create_result);
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = g_create_invalid_handle ? kInvalidItemHandle : kProduct;
    return ok();
}

QolResult create_item(int32_t, int32_t, int32_t, int32_t, QolItemHandle* out) {
    return create_perfect_item(0, out);
}

QolResult save_item(QolItemHandle item, int32_t* out_saved) {
    ++g_save_calls;
    g_save_sequence = ++g_sequence;
    if (out_saved == nullptr) return fail(kQolErrInvalidArgument);
    if (g_save_result != kQolOk) return fail(g_save_result);
    *out_saved = g_save_saved;
    (void)item;
    return ok();
}

QolResult free_item(QolItemHandle item) {
    ++g_free_calls;
    g_freed = item;
    if (g_free_result != kQolOk) return fail(g_free_result);
    return ok();
}

QolResult read_jewel_word(QolItemHandle item, uint32_t* out) {
    ++g_read_jewel_word_calls;
    if (out == nullptr || !same(item, kJewel)) return fail(kQolErrNotFound);
    *out = g_jewel_word;
    return ok();
}

QolResult write_jewel_word(QolItemHandle item, uint32_t word) {
    ++g_jewel_write_calls;
    if (same(item, kProduct)) {
        g_product_scale_sequence = ++g_sequence;
    } else {
        g_jewel_write_sequence = ++g_sequence;
    }
    if (g_jewel_write_result != kQolOk) return fail(g_jewel_write_result);
    g_jewel_word_written = word;
    if (!same(item, kProduct)) g_jewel_word = word;  // 仅源宝石回读生效
    return ok();
}

QolResult set_max_socket_enchant(QolItemHandle) {
    ++g_set_max_socket_calls;
    if (g_set_max_socket_result != kQolOk) return fail(g_set_max_socket_result);
    return ok();
}

QolResult write_stuff_need(uint32_t index, int32_t, int32_t) {
    const int call = g_stuff_need_calls;
    if (call < 8) g_stuff_need_sequence[call] = ++g_sequence;
    ++g_stuff_need_calls;
    if (g_stuff_need_fail_at == call) return fail(kQolErrBlocked);
    return ok();
}

QolResult port_random(int32_t lo, int32_t hi, int32_t* out) {
    ++g_random_calls;
    if (g_random_result_code != kQolOk) return fail(g_random_result_code);
    if (out == nullptr || lo > hi) return fail(kQolErrInvalidArgument);
    *out = g_random_out_override != std::numeric_limits<int32_t>::min()
               ? g_random_out_override
               : lo;
    return ok();
}

// 轮转随机：动态配方生成需要「互不相同」的下标（避免去重失败）。
int g_cycle_random = 0;
QolResult cycling_random(int32_t lo, int32_t hi, int32_t* out) {
    if (out == nullptr || lo > hi) return fail(kQolErrInvalidArgument);
    const int32_t span = hi - lo + 1;
    *out = lo + (g_cycle_random++ % span);
    return ok();
}

int g_set_stuff_item_calls = 0;
QolResult set_stuff_item(uint32_t, QolItemHandle) {
    ++g_set_stuff_item_calls;
    return ok();
}
QolResult description_set_off() { return ok(); }
QolResult set_target_slot_item(QolItemHandle) { return ok(); }

// ---- attribute.quality v2 替身 ----
bool g_quality_capability_available = true;
AttributeQualityApi g_quality_api{};

int32_t quality_level(int32_t value, int32_t minimum, int32_t maximum) {
    if (maximum <= minimum || value >= maximum) return kAttributeQualityHighestLevel;
    const int64_t percentile =
        (static_cast<int64_t>(value) - minimum) * 100 /
        (static_cast<int64_t>(maximum) - minimum);
    return percentile >= 90 ? 4
         : percentile >= 75 ? 3
         : percentile >= 60 ? 2
         : percentile >= 30 ? 1
                            : 0;
}

QolResult quality_query_item(QolItemHandle item, const AttributeQualityOptions*,
                             AttributeQualityItemResult* out) {
    if (out == nullptr || !same(item, kJewel)) return fail(kQolErrNotReady);
    const int32_t value = static_cast<int32_t>(g_jewel_word & 0x7ffu);
    const int32_t type = static_cast<int32_t>((g_jewel_word >> 18) & 0x3fu);
    out->complete = 1;
    out->count = 1;
    out->attrs[0] = AttributeQualityItem{AttributeKind::Jewel, 0, type, 0, value, {0, 100},
                                         {value, quality_level(value, 0, 100)}, kQolOk};
    return ok();
}

QolResult quality_query_option(QolItemHandle, int32_t, int32_t,
                               const AttributeQualityOptions*, AttributeQualityItem*) {
    return fail(kQolErrUnsupported);
}

QolResult quality_evaluate(int32_t value, AttributeRange range,
                           const AttributeQualityOptions*, AttributeQuality* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    out->percentile = range.max <= range.min || value >= range.max ? 100
                     : value < range.min ? 0
                     : static_cast<int32_t>((static_cast<int64_t>(value) - range.min) * 100 /
                                            (static_cast<int64_t>(range.max) - range.min));
    out->level = quality_level(value, range.min, range.max);
    return ok();
}

QolResult capability_require(const char* id, QolVersionRange, const void** out) {
    if (id == nullptr || out == nullptr) return fail(kQolErrInvalidArgument);
    *out = nullptr;
    if (std::strcmp(id, kAttributeQualityCapabilityId) == 0) {
        if (!g_quality_capability_available) return fail(kQolErrNotReady);
        *out = &g_quality_api;
        return ok();
    }
    return fail(kQolErrNotFound);
}

// ---- 表注入替身（host 上 table.cpp 只编译纯构造器）----
bool g_table_ensure_result = true;
int g_table_ensure_calls = 0;
int g_table_deactivate_calls = 0;

// ---- 全局 fixture ----
ItemFactsApi g_item_facts{};
CraftFactsApi g_craft_facts{};
QolGameHost g_game{};
QolHostSdk g_sdk{};
QolCustomRecipeWritePort g_write_port{};

using qol::features::custom_recipe::result::CraftExecuteResult;
using qol::features::custom_recipe::result::CraftGate;
using qol::features::custom_recipe::result::CraftMakeItemInput;
using qol::features::custom_recipe::result::CraftPlaceInput;
using qol::features::custom_recipe::result::CraftPrepareInput;
using qol::features::custom_recipe::result::CraftExecuteInput;
using qol::features::custom_recipe::result::PlaceOutcome;
using qol::features::custom_recipe::result::PrepareOutcome;

const qol::features::custom_recipe::result::CraftResultApi& api() {
    return qol::features::custom_recipe::craft_result_api();
}

// 布置三格（类别/identity 由 g_slots 派生），并断言 prepare 进入待确认。
bool prepare_three_slots(const QolItemHandle (&items)[3], bool* out_confirm = nullptr) {
    g_slots[0] = items[0];
    g_slots[1] = items[1];
    g_slots[2] = items[2];
    uint16_t categories[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const ItemFact* fact = find_fact(items[i]);
        categories[i] = fact != nullptr ? static_cast<uint16_t>(fact->category) : 0;
    }
    CraftPrepareInput in{};
    std::memcpy(in.slot_categories, categories, sizeof(categories));
    const PrepareOutcome outcome = api().prepare(&in);
    if (out_confirm != nullptr) *out_confirm = outcome == PrepareOutcome::kConfirmPending;
    return outcome == PrepareOutcome::kConfirmPending;
}

CraftExecuteResult execute_three_slots(const QolItemHandle (&items)[3]) {
    CraftExecuteInput in{};
    for (int i = 0; i < 3; ++i) {
        in.items[i] = items[i];
        const ItemFact* fact = find_fact(items[i]);
        in.slot_categories[i] = fact != nullptr ? static_cast<uint16_t>(fact->category) : 0;
        if (fact != nullptr) in.held_counts[i] = held_of(fact->category);
    }
    return api().execute(&in);
}

// 目录第 2 条（kJewelTierUp）的 mixType = base_record_count(69) + 1。
constexpr uint32_t kTierUpMixType = 69 + 1;

// 缩放配方：宝石 + 混沌武器卷轴(20) + 混沌防具卷轴(25) → 宝石自身（scale 1200）。
const QolItemHandle kScaleRecipe[3] = {kJewel, kScrollWeapon, kScrollArmor};
// 固定类别配方：皮革(35) + 空 + 魔法衣料(41) → 背包装（类别 4）。
const QolItemHandle kFixedRecipe[3] = {kMaterialA, kInvalidItemHandle, kMaterialB};
// 特殊装备拉满配方：两件同类特殊装备(485) + 元气恢复药水(14)。
const QolItemHandle kSpecialRecipe[3] = {kEquipAlpha, kPotion, kEquipBeta};

void init_fixture() {
    reset_env();

    g_item_facts = ItemFactsApi{};
    g_item_facts.struct_size = sizeof(g_item_facts);
    g_item_facts.category = &item_category;
    g_item_facts.is_real_equip = &item_is_real_equip;
    g_item_facts.is_jewel_category = &is_jewel_category;

    g_craft_facts = CraftFactsApi{};
    g_craft_facts.read.category_held_count = &held_count;
    g_craft_facts.read.character_level = &character_level;
    g_craft_facts.read.stuff_item = &stuff_item;
    g_craft_facts.read_only.stuff_slot_available = &slot_available;

    g_game = QolGameHost{};
    g_game.inventory_write.remove_item = &remove_item;
    g_game.inventory_write.remove_item_data = &remove_item_data;

    g_sdk = QolHostSdk{};
    g_sdk.abi = {sizeof(g_sdk), kHostSdkMajor, kHostSdkMinor};
    g_sdk.capabilities.require = &capability_require;

    g_quality_api = AttributeQualityApi{};
    g_quality_api.struct_size = static_cast<uint32_t>(sizeof(AttributeQualityApi));
    g_quality_api.query_item = &quality_query_item;
    g_quality_api.query_option_context = &quality_query_option;
    g_quality_api.evaluate = &quality_evaluate;
    g_quality_capability_available = true;

    g_write_port = QolCustomRecipeWritePort{};
    g_write_port.abi_version = kQolPortCustomRecipeWriteVersion;
    g_write_port.write_stuff_need = &write_stuff_need;
    g_write_port.set_stuff_item = &set_stuff_item;
    g_write_port.description_set_off = &description_set_off;
    g_write_port.create_perfect_item = &create_perfect_item;
    g_write_port.create_item = &create_item;
    g_write_port.save_item = &save_item;
    g_write_port.free_item = &free_item;
    g_write_port.read_jewel_word = &read_jewel_word;
    g_write_port.write_jewel_word = &write_jewel_word;
    g_write_port.set_max_socket_enchant = &set_max_socket_enchant;
    g_write_port.random_inclusive = &port_random;

    PluginContext context{};
    context.game = &g_game;
    context.sdk = &g_sdk;
    qol::features::custom_recipe::set_context(context);
    qol::features::custom_recipe::set_item_facts(&g_item_facts);
    qol::features::custom_recipe::set_craft_facts(&g_craft_facts);
    qol::features::custom_recipe::set_write_port(&g_write_port);
    qol::features::custom_recipe::set_enabled(true);
    custom_recipe::bind_base_record_count(69);
}

// ---------------------------------------------------------------------------
// place / prepare / 成功执行
// ---------------------------------------------------------------------------
void test_place_and_execute_success() {
    init_fixture();
    const CraftPlaceInput place{0, kMaterialA, false, 35, 0, 0, 0};
    assert(api().place(&place) == PlaceOutcome::kPlaced);
    g_stuff_slot_available = false;
    assert(api().place(&place) == PlaceOutcome::kPlaced);  // 格满 → 无事发生
    g_stuff_slot_available = true;

    assert(prepare_three_slots(kFixedRecipe));
    const int held_before = held_of(35);
    assert(execute_three_slots(kFixedRecipe) == CraftExecuteResult::kOk);
    assert(g_create_calls == 1 && g_save_calls == 1 && g_free_calls == 0);
    assert(g_remove_item_calls + g_remove_item_data_calls == 2);
    assert(held_of(35) == held_before - 1);
}

// ---------------------------------------------------------------------------
// CR01：前置 identity/category/count/function ready 失败 → 无扣料、无产物
// ---------------------------------------------------------------------------
void test_precheck_failures_do_not_consume() {
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));

    // 入参 identity 与 pending 不一致（同类别不同 identity）。
    {
        QolItemHandle tampered[3] = {kJewel, kScrollWeapon, kScrollArmor};
        tampered[1] = QolItemHandle{kScrollWeapon.slot, kScrollWeapon.generation + 1};
        CraftExecuteInput in{};
        for (int i = 0; i < 3; ++i) {
            in.items[i] = tampered[i];
            in.slot_categories[i] = i == 0 ? 28 : (i == 1 ? 20 : 25);
        }
        assert(api().execute(&in) == CraftExecuteResult::kStaleSlots);
        assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0);
        assert(g_create_calls == 0 && g_save_calls == 0);
    }

    // pending 已失效：重新 prepare 后逐项注入失败。
    assert(prepare_three_slots(kScaleRecipe));
    // live 槽位 identity 与 pending 不一致。
    g_slots[1] = QolItemHandle{kScrollWeapon.slot, kScrollWeapon.generation + 1};
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kStaleSlots);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_create_calls == 0);

    // live category 与入参 category 不一致。
    assert(prepare_three_slots(kScaleRecipe));
    g_slots[2] = kPotion;  // 类别 14 ≠ 25
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kStaleSlots);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_create_calls == 0);

    // 读取 live 身份失败 → 明确前置失败（非 stale）。
    assert(prepare_three_slots(kScaleRecipe));
    g_read_stuff_fail_index = 1;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kPrecheckFailed);
    g_read_stuff_fail_index = -1;
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_create_calls == 0);

    // 所需函数未就绪（写端口缺 create/save）→ 前置失败，无副作用。
    assert(prepare_three_slots(kScaleRecipe));
    g_write_port.create_perfect_item = nullptr;
    g_write_port.create_item = nullptr;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kPrecheckFailed);
    g_write_port.create_perfect_item = &create_perfect_item;
    g_write_port.create_item = &create_item;
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_create_calls == 0);

    // 库存不足（类别计数 < 需扣单位数）→ kNotEnoughHeld，无副作用。
    add_held(20, -held_of(20));
    assert(prepare_three_slots(kScaleRecipe));
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kNotEnoughHeld);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_create_calls == 0);
}

// ---------------------------------------------------------------------------
// CR01：产物预创建/配置失败 → 扣料前清理临时品，无扣料
// ---------------------------------------------------------------------------
void test_product_prepare_failure_no_consumption() {
    init_fixture();
    // 创建失败。
    assert(prepare_three_slots(kScaleRecipe));
    g_create_result = kQolErrNotReady;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductPrepareFailed);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_save_calls == 0);
    assert(g_free_calls == 0);  // 无有效 handle → 不 free

    // 创建成功但 handle 非法 → 清理（free 失败也记录，不 double free）。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_create_result = kQolOk;
    g_create_invalid_handle = true;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductPrepareFailed);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_save_calls == 0);
    assert(g_free_calls == 0);

    // 数值缩放（product 上的 jewel word 写）失败 → 清理 + 无扣料。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_jewel_write_result = kQolErrBlocked;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductPrepareFailed);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_save_calls == 0);
    assert(g_free_calls == 1 && same(g_freed, kProduct));

    // 清理失败（free 报错）也不 double free：只调用一次。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_jewel_write_result = kQolErrBlocked;
    g_free_result = kQolErrBlocked;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductPrepareFailed);
    assert(g_free_calls == 1);

    // 特殊装备配方：max socket/enchant 配置失败 → 清理 + 无扣料。
    init_fixture();
    assert(prepare_three_slots(kSpecialRecipe));
    g_set_max_socket_result = kQolErrNotReady;
    assert(execute_three_slots(kSpecialRecipe) == CraftExecuteResult::kProductPrepareFailed);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_save_calls == 0);
    assert(g_free_calls == 1);
}

// ---------------------------------------------------------------------------
// CR01：扣料失败/计数不符/读取失败 → 立即停止，不 save，已扣状态准确
// ---------------------------------------------------------------------------
void test_material_remove_failures() {
    // 第 1/2/3 次 remove 失败：停止后续扣料与 save。
    for (int failing_call = 0; failing_call < 3; ++failing_call) {
        init_fixture();
        assert(prepare_three_slots(kScaleRecipe));
        g_remove_item_data_fail_call = failing_call;
        assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kMaterialRemoveFailed);
        assert(g_save_calls == 0);
        assert(g_remove_item_data_calls == failing_call + 1);  // 后续槽位不再扣
        assert(g_free_calls == 1);                             // 未入包产物被清理
    }

    // 不可堆叠槽位的 remove_item 失败（特殊装备配方第 0 格）。
    init_fixture();
    assert(prepare_three_slots(kSpecialRecipe));
    g_remove_item_fail_slot = 0;
    assert(execute_three_slots(kSpecialRecipe) == CraftExecuteResult::kMaterialRemoveFailed);
    assert(g_save_calls == 0 && g_remove_item_calls == 0 && g_remove_item_data_calls == 0);

    // 计数不变（观察不符）→ kMaterialObservationFailed，不 save。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_observation_skip_decrement = true;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kMaterialObservationFailed);
    assert(g_save_calls == 0 && g_remove_item_data_calls == 1);

    // 前置计数读取失败 → kPrecheckFailed（扣料前即停止，无副作用）。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_observation_read_fail = true;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kPrecheckFailed);
    assert(g_save_calls == 0 && g_remove_item_calls == 0 && g_remove_item_data_calls == 0 &&
           g_create_calls == 0);

    // 扣料后的计数读取失败 → kMaterialObservationFailed，不 save（前置 3 次读成功）。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_held_read_fail_after = 3;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kMaterialObservationFailed);
    assert(g_save_calls == 0 && g_remove_item_data_calls == 1);

    // 过扣（一次扣 2）同样视为观察不符。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_observation_skip_decrement = true;
    add_held(28, -1);  // 额外少 1 → after == before - 2
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kMaterialObservationFailed);
    assert(g_save_calls == 0);
}

// ---------------------------------------------------------------------------
// CR01：save 失败 → 不 free/不删除产物（ownership 可能已转移），不 double free
// ---------------------------------------------------------------------------
void test_save_failure_no_double_free() {
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_save_result = kQolErrNotReady;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductSaveFailed);
    assert(g_save_calls == 1);
    assert(g_free_calls == 0);  // save 之后绝不按 handle 释放

    // saved 计数为 0 → 仍算保存失败，且不 free。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_save_saved = 0;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductSaveFailed);
    assert(g_free_calls == 0);

    // 成功后也不 free。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kOk);
    assert(g_free_calls == 0 && g_save_calls == 1);
}

// ---------------------------------------------------------------------------
// CR08：SaveEnter 重建 / 停用作废 pending；identity 校验不靠类别
// ---------------------------------------------------------------------------
void test_pending_invalidation() {
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    // 动态表重建（SaveEnter 路径）后 pending 失效。
    custom_recipe::ThreeSlotRecipe dynamic_out[4];
    size_t dynamic_count = 0;
    const QolResult built = custom_recipe::build_dynamic_recipes(
        custom_recipe::kSpecialEquipCategories, 1, custom_recipe::kMaterialPool,
        custom_recipe::kMaterialPoolSize, &cycling_random, dynamic_out, 4, &dynamic_count);
    assert(built.ok == kQolOk && dynamic_count == 1);
    custom_recipe::set_dynamic_three_slot_recipes(dynamic_out, dynamic_count);
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kStaleSlots);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0 && g_save_calls == 0);
    custom_recipe::set_dynamic_three_slot_recipes(nullptr, 0);

    // 停用 → pending 失效，execute 直接 kAborted。
    assert(prepare_three_slots(kScaleRecipe));
    qol::features::custom_recipe::set_enabled(false);
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kAborted);
    qol::features::custom_recipe::set_enabled(true);

    // 同类别不同 identity：不可堆叠物品同 identity 占两格 → kStaleSlots。
    init_fixture();
    const QolItemHandle duplicate[3] = {kEquipAlpha, kPotion, kEquipAlpha};
    assert(prepare_three_slots(duplicate));
    assert(execute_three_slots(duplicate) == CraftExecuteResult::kStaleSlots);
    assert(g_remove_item_calls == 0 && g_create_calls == 0);

    // 两件同类但不同 identity 的特殊装备 → 正常进入执行。
    assert(prepare_three_slots(kSpecialRecipe));
    assert(execute_three_slots(kSpecialRecipe) == CraftExecuteResult::kOk);

    // 掩格类别不同（两件必须同类）→ kStaleSlots。
    init_fixture();
    const QolItemHandle mixed[3] = {kEquipAlpha, kPotion, kEquipBeta};  // 两件同为类别 485
    assert(prepare_three_slots(mixed));
    assert(execute_three_slots(mixed) == CraftExecuteResult::kOk);
}

// ---------------------------------------------------------------------------
// CR02：quality 不可用 → kUnavailable（非 kNotJewel）；三格配方不受影响
// ---------------------------------------------------------------------------
void test_craft_gate_unavailable_vs_not_jewel() {
    init_fixture();
    // 真非宝石（类别 35）→ kNotJewel。
    assert(api().craft_gate(kMaterialA) == CraftGate::kNotJewel);
    // 宝石 + quality 可用 → kPass（未到金档）。
    assert(api().craft_gate(kJewel) == CraftGate::kPass);
    // quality 能力撤销 → kUnavailable，而不是 kNotJewel。
    g_quality_capability_available = false;
    assert(api().craft_gate(kJewel) == CraftGate::kUnavailable);
    g_quality_capability_available = true;
    // 金档 → kAlreadyGold。
    g_jewel_word = 2047u | (3u << 18) | (5u << 11);
    assert(api().craft_gate(kJewel) == CraftGate::kAlreadyGold);
    g_jewel_word = 20u | (3u << 18) | (5u << 11);
    // 质量事实与当前 word 一致时仍可放行（不因一致性校验误报）。
    g_jewel_word = 30u | (3u << 18) | (5u << 11);
    assert(api().craft_gate(kJewel) == CraftGate::kPass);
    // 三格配方在 quality 撤销时仍可用（只阻断 tier-up）。
    init_fixture();
    g_quality_capability_available = false;
    assert(prepare_three_slots(kFixedRecipe));
    assert(execute_three_slots(kFixedRecipe) == CraftExecuteResult::kOk);
    assert(api().is_jewel_item(kJewel));
    assert(!api().is_jewel_item(kMaterialA));
}

// ---------------------------------------------------------------------------
// CR03：闭区间随机（真实端口）与错误传播
// ---------------------------------------------------------------------------
void test_random_port_and_tier_up_ordering() {
    init_fixture();
    // 端口随机：闭区间两端 + 错误传播 + 越界拒绝 + 版本门禁。
    int32_t value = -1;
    assert(qol::features::custom_recipe::random_inclusive(5, 9, &value).ok == kQolOk);
    assert(value == 5);  // 桩返回 lo，仍落在闭区间内
    g_random_out_override = 9;
    assert(qol::features::custom_recipe::random_inclusive(5, 9, &value).ok == kQolOk);
    assert(value == 9);
    g_random_result_code = kQolErrBlocked;
    assert(qol::features::custom_recipe::random_inclusive(5, 9, &value).code == kQolErrBlocked);
    g_random_result_code = kQolOk;
    g_random_out_override = 100;  // 越界 → 显式错误
    assert(qol::features::custom_recipe::random_inclusive(5, 9, &value).ok != kQolOk);
    g_random_out_override = std::numeric_limits<int32_t>::min();
    const QolVersion saved_version = g_write_port.abi_version;
    g_write_port.abi_version = QolVersion{1, 1};  // 旧 minor 无 typed random
    assert(qol::features::custom_recipe::random_inclusive(5, 9, &value).ok != kQolOk);
    g_write_port.abi_version = saved_version;
    assert(qol::features::custom_recipe::random_inclusive(9, 5, &value).ok != kQolOk);

    // CR10：need 先写、jewel 后写；质量/评估失败不写 need/不写 jewel。
    init_fixture();
    g_character_level = 50;
    const CraftMakeItemInput make{kTierUpMixType, kJewel, &port_random};
    assert(api().make_item(&make) == 0);
    assert(g_stuff_need_calls > 0);
    assert(g_jewel_write_calls == 1);
    assert(g_stuff_need_sequence[0] > 0 && g_jewel_write_sequence > g_stuff_need_sequence[0]);
    assert(g_random_calls == 1);  // tier-up 候选随机经同一 typed 端口

    // quality 撤销 → 不写 need、不写 jewel、不随机。
    init_fixture();
    g_quality_capability_available = false;
    const int needs_before = g_stuff_need_calls;
    assert(api().make_item(&make) != 0);
    assert(g_stuff_need_calls == needs_before && g_jewel_write_calls == 0 && g_random_calls == 0);

    // 材料需求校验失败（无法识别宝石档位）→ 不写 need/jewel。
    init_fixture();
    g_write_port.read_jewel_word = &read_jewel_word;
    const int needs_before2 = g_stuff_need_calls;
    assert(api().make_item(&make) == 0);  // 正常
    assert(g_stuff_need_calls > needs_before2);

    // need 写入失败 → 返回 1 且不写 jewel。
    init_fixture();
    g_stuff_need_fail_at = 0;
    assert(api().make_item(&make) != 0);
    assert(g_jewel_write_calls == 0);

    // tier-up 随机错误 → 返回 1，且不写 need/jewel。
    init_fixture();
    g_random_result_code = kQolErrBlocked;
    const int needs_before3 = g_stuff_need_calls;
    assert(api().make_item(&make) != 0);
    assert(g_stuff_need_calls == needs_before3 && g_jewel_write_calls == 0);

    // 金档 → 返回 1（不写 need/jewel）。
    init_fixture();
    g_jewel_word = 2047u | (3u << 18) | (5u << 11);
    assert(api().make_item(&make) != 0);
    assert(g_jewel_write_calls == 0);

    // 非本层 mixType → -1（界面转调原版）；停用 → 1（fail-closed）。
    init_fixture();
    const CraftMakeItemInput unknown{999, kJewel, &port_random};
    assert(api().make_item(&unknown) == -1);
    qol::features::custom_recipe::set_enabled(false);
    assert(api().make_item(&make) == 1);
    qol::features::custom_recipe::set_enabled(true);
}

// ---------------------------------------------------------------------------
// wrapper 等价成功路径（覆盖边界说明）：
// UI 子层 on_confirm_three_slot 的输入构造 = 逐格读 stuff_item + 类别 + 持有数，
// 然后调 api_execute。本目标用同一构造路径验证真实产出与材料状态；
// owner fixture 只管到 UI 提示/取消/失败（其写入端口为空），不作为「真机成功合成」证据。
// ---------------------------------------------------------------------------
void test_wrapper_equivalent_execute_success() {
    init_fixture();
    // 缩放配方（宝石 + 两张混沌卷轴）→ 产物 = 首格宝石自身。
    assert(prepare_three_slots(kScaleRecipe));

    CraftExecuteInput in{};
    for (size_t i = 0; i < 3; ++i) {
        in.items[i] = g_slots[i];                               // 同 wrapper：读槽位身份
        const ItemFact* fact = find_fact(in.items[i]);
        in.slot_categories[i] = fact != nullptr ? static_cast<uint16_t>(fact->category) : 0;
        if (fact != nullptr) in.held_counts[i] = held_of(fact->category);
    }
    const int32_t held_28 = held_of(28);
    assert(api().execute(&in) == CraftExecuteResult::kOk);
    assert(g_remove_item_data_calls == 3);   // 三格各扣 1（真实扣料）
    assert(g_create_calls == 1 && g_save_calls == 1);
    assert(g_free_calls == 0);               // 成功后不释放产物
    assert(g_random_calls == 0);             // 三格配方不需要随机
    assert(held_of(28) == held_28 - 1);
}

// ---------------------------------------------------------------------------
// CR 修复：PrepareOutcome 区分「真未命中」与「上下文不可用」
//   · match_three_slot 返回 nullptr            → kNoMatch（提示：配方不匹配）
//   · 已匹配但槽位身份/类别读取或一致性失败    → kUnavailable（提示：合成暂不可用，材料未扣）
// ---------------------------------------------------------------------------
void test_prepare_outcome_distinction() {
    // 枚举值稳定性（尾追加不改变既有数值）。
    assert(static_cast<uint8_t>(PrepareOutcome::kNotMine) == 0);
    assert(static_cast<uint8_t>(PrepareOutcome::kNoMatch) == 1);
    assert(static_cast<uint8_t>(PrepareOutcome::kConfirmPending) == 2);
    assert(static_cast<uint8_t>(PrepareOutcome::kUnavailable) == 3);

    init_fixture();
    // 真未命中：{35, 41, 28} 不属于任何配方 → kNoMatch。
    {
        g_slots[0] = kMaterialA;
        g_slots[1] = kMaterialB;
        g_slots[2] = kJewel;
        CraftPrepareInput in{{35, 41, 28}};
        assert(api().prepare(&in) == PrepareOutcome::kNoMatch);
    }

    // 已匹配但 live 身份读取失败 → kUnavailable（不是 NoMatch）。
    init_fixture();
    {
        g_slots[0] = kMaterialA;
        g_slots[1] = kInvalidItemHandle;
        g_slots[2] = kMaterialB;
        g_read_stuff_fail_index = 2;
        CraftPrepareInput in{{35, 0, 41}};
        assert(api().prepare(&in) == PrepareOutcome::kUnavailable);
        g_read_stuff_fail_index = -1;
    }

    // 已匹配（调用方类别 {35,0,41}）但槽内实际类别不一致 → kUnavailable。
    init_fixture();
    {
        g_slots[0] = kMaterialA;      // 实际 35（与调用方一致）
        g_slots[1] = kInvalidItemHandle;
        g_slots[2] = kJewel;          // 实际 28，但调用方声称 41
        CraftPrepareInput in{{35, 0, 41}};
        assert(api().prepare(&in) == PrepareOutcome::kUnavailable);
    }

    // 已匹配但空格声称有对象 → kUnavailable（上下文不一致，不是配方错）。
    init_fixture();
    {
        g_slots[0] = kMaterialA;
        g_slots[1] = kMaterialB;  // 空格却有对象
        g_slots[2] = kMaterialB;
        CraftPrepareInput in{{35, 0, 41}};
        assert(api().prepare(&in) == PrepareOutcome::kUnavailable);
    }

    // 真命中仍为 kConfirmPending。
    init_fixture();
    {
        g_slots[0] = kMaterialA;
        g_slots[1] = kInvalidItemHandle;
        g_slots[2] = kMaterialB;
        CraftPrepareInput in{{35, 0, 41}};
        assert(api().prepare(&in) == PrepareOutcome::kConfirmPending);
    }

    // 放料：同类可堆叠「2 个物品不能添加 3 次」→ 真实 kNotEnoughHeld（保留 94 语义）。
    init_fixture();
    {
        add_held(5, -held_of(5));
        add_held(5, 2);  // 只有 2 个恢复药水
        g_slots[0] = kPotion;
        g_slots[1] = kPotion;
        g_slots[2] = kInvalidItemHandle;
        CraftPlaceInput place{};
        place.slot_index = 2;
        place.item = kPotion;
        place.item_stackable = true;
        place.item_category = 14;  // find_fact(kPotion).category = 14
        place.placed_same_object = 1;
        place.same_category_placed = 2;
        place.category_held_count = 2;
        assert(api().place(&place) == PlaceOutcome::kNotEnoughHeld);
    }
}

// ---------------------------------------------------------------------------
// typed handler 门禁：gate 关闭（停用 / ownership 未就绪 → !enabled()）时，
// 旧队列里残留的写命令一律不得写回；合法 enabled payload 仍写入。
// ---------------------------------------------------------------------------
void test_typed_handler_gate() {
    init_fixture();
    g_set_stuff_item_calls = 0;
    const qol::contracts::CraftWriteStuffNeedPayload need{1, 15, 3};
    const qol::contracts::CraftSetStuffItemPayload set{2, kJewel};
    const auto* need_bytes = reinterpret_cast<const uint8_t*>(&need);
    const auto* set_bytes = reinterpret_cast<const uint8_t*>(&set);

    // 合法 enabled payload：两条 typed handler 都真实写回。
    assert(qol::features::custom_recipe::enabled());
    assert(qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes, sizeof(need)));
    assert(g_stuff_need_calls == 1);
    assert(qol::features::custom_recipe::handle_set_stuff_item_command(set_bytes, sizeof(set)));
    assert(g_set_stuff_item_calls == 1);

    // gate 关闭（停用）：不写、返回 false；旧队列残留命令亦不写回。
    qol::features::custom_recipe::set_enabled(false);
    assert(!qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes, sizeof(need)));
    assert(g_stuff_need_calls == 1);
    assert(!qol::features::custom_recipe::handle_set_stuff_item_command(set_bytes, sizeof(set)));
    assert(g_set_stuff_item_calls == 1);

    // 参数非法（大小不符 / 空指针）：仍返回 false 且不写。
    qol::features::custom_recipe::set_enabled(true);
    assert(!qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes,
                                                                          sizeof(need) - 1));
    assert(!qol::features::custom_recipe::handle_write_stuff_need_command(nullptr, sizeof(need)));
    assert(g_stuff_need_calls == 1 && g_set_stuff_item_calls == 1);

    // 恢复 enabled：写回重新生效（门禁不是单向锁）。
    assert(qol::features::custom_recipe::handle_write_stuff_need_command(need_bytes, sizeof(need)));
    assert(g_stuff_need_calls == 2);
}

// ---------------------------------------------------------------------------
// F2：失败阶段 → 材料扣减状态（UI 提示分流依据）
// - Prepare（前置/产物准备）失败：材料**未**扣 → 提示「材料未扣」；
// - 扣料后（Observation/Save）失败：材料**可能已扣** → 提示「材料可能已扣」。
// 本测试只固定结果层可观测状态；文案映射由 UI 子层消费该状态。
// ---------------------------------------------------------------------------
void test_failure_stage_material_state() {
    // 产物准备失败：三格材料计数完全不变。
    init_fixture();
    const int32_t held_before_prepare[3] = {held_of(28), held_of(20), held_of(25)};
    assert(prepare_three_slots(kScaleRecipe));
    g_jewel_write_result = kQolErrBlocked;  // product scale 失败（仍在扣料前）
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductPrepareFailed);
    assert(held_of(28) == held_before_prepare[0] && held_of(20) == held_before_prepare[1] &&
           held_of(25) == held_before_prepare[2]);
    assert(g_remove_item_calls == 0 && g_remove_item_data_calls == 0);

    // 前置校验失败：同样零扣减。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    add_held(20, -held_of(20));
    const int32_t held_before_precheck = held_of(28);
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kNotEnoughHeld);
    assert(held_of(28) == held_before_precheck && g_remove_item_data_calls == 0);

    // 观察不符：第 1 格已实扣（材料可能已扣）。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_observation_skip_decrement = true;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kMaterialObservationFailed);
    assert(g_remove_item_data_calls == 1 && g_save_calls == 0);

    // 保存失败：三格已全部实扣（材料可能已扣）。
    init_fixture();
    assert(prepare_three_slots(kScaleRecipe));
    g_save_result = kQolErrNotReady;
    assert(execute_three_slots(kScaleRecipe) == CraftExecuteResult::kProductSaveFailed);
    assert(g_remove_item_data_calls == 3 && g_save_calls == 1);
    assert(held_of(28) == 4 && held_of(20) == 4 && held_of(25) == 4);
}

// ---------------------------------------------------------------------------
// ownership readiness（L2 gate 合取输入）
// ---------------------------------------------------------------------------
void test_ownership_ready_flag() {
    init_fixture();
    const auto& ops = qol::features::custom_recipe::ownership_ops();
    assert(!qol::features::custom_recipe::ownership_ready());
    g_table_ensure_result = false;
    assert(ops.acquire("own:RECIPEBASE_MIXTUREBASE") != 0);
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(g_table_deactivate_calls == 1);  // acquire 失败时恢复局部副作用
    g_table_ensure_result = true;
    assert(ops.acquire("own:RECIPEBASE_MIXTUREBASE") == 0);
    assert(qol::features::custom_recipe::ownership_ready());
    ops.release("own:RECIPEBASE_MIXTUREBASE");
    assert(!qol::features::custom_recipe::ownership_ready());
    assert(g_table_deactivate_calls == 2);
}

}  // namespace

// host 上 table.cpp 只编译纯构造器；表注入与 sdk/game 上下文由本测试替身提供。
namespace custom_recipe {

void set_context(const qol::contracts::QolGameHost*, const qol::contracts::QolHostSdk*) {}

bool custom_recipe_table_ensure() {
    ++g_table_ensure_calls;
    return g_table_ensure_result;
}

void custom_recipe_table_deactivate() {
    ++g_table_deactivate_calls;
}

}  // namespace custom_recipe

int main() {
    test_place_and_execute_success();
    test_precheck_failures_do_not_consume();
    test_product_prepare_failure_no_consumption();
    test_material_remove_failures();
    test_save_failure_no_double_free();
    test_pending_invalidation();
    test_craft_gate_unavailable_vs_not_jewel();
    test_random_port_and_tier_up_ordering();
    test_wrapper_equivalent_execute_success();
    test_prepare_outcome_distinction();
    test_typed_handler_gate();
    test_failure_stage_material_state();
    test_ownership_ready_flag();
    return 0;
}
