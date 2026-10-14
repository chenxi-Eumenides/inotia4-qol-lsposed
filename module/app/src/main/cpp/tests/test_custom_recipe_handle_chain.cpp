#include "plugins/custom_recipe/feature.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include "contracts/attribute_quality.h"
#include "contracts/plugin.h"
#include "plugins/custom_recipe/catalog.h"
#include "plugins/custom_recipe/table.h"

namespace {

using namespace qol::contracts;

constexpr QolItemHandle kMaterialA{10, 2};
constexpr QolItemHandle kMaterialB{11, 3};
constexpr QolItemHandle kJewel{12, 4};
constexpr QolItemHandle kProduct{50, 8};

int g_remove_item_calls = 0;
QolItemHandle g_removed[3]{};
int g_create_calls = 0;
int g_save_calls = 0;
int g_free_calls = 0;
QolItemHandle g_saved_item{};
QolItemHandle g_freed_item{};
uint32_t g_jewel_word = 20u | (3u << 18) | (5u << 11);
QolItemHandle g_word_read_item{};
QolItemHandle g_word_write_item{};
uint32_t g_word_written = 0;
int g_stuff_need_calls = 0;
QolItemHandle g_placed_item{};
bool g_stuff_available = true;
int32_t g_stuff_write_result = kQolOk;
int g_stuff_write_calls = 0;
int g_random_calls = 0;
int g_jewel_write_calls = 0;
int g_operation_sequence = 0;
int g_jewel_write_sequence = 0;
int g_stuff_need_sequence = 0;
int32_t g_jewel_write_result = kQolOk;
bool g_quality_capability_available = true;
bool g_quality_query_ok = true;
bool g_quality_query_complete = true;
uint32_t g_quality_query_count = 1;
AttributeKind g_quality_kind = AttributeKind::Jewel;
int32_t g_quality_status = kQolOk;
int32_t g_quality_level_override = -1;
int32_t g_quality_eval_fail_value = std::numeric_limits<int32_t>::min();
int g_quality_require_calls = 0;
int g_quality_query_calls = 0;
int g_quality_evaluate_calls = 0;
bool g_quality_options_were_null = true;

QolResult ok() { return qol::contracts::ok(); }
QolResult fail(int32_t code) { return qol::contracts::error(code, "test failure", 12); }

QolResult item_category(QolItemHandle item, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    if (item.slot == kMaterialA.slot && item.generation == kMaterialA.generation) *out = 35;
    else if (item.slot == kMaterialB.slot && item.generation == kMaterialB.generation) *out = 41;
    else if (item.slot == kJewel.slot && item.generation == kJewel.generation) *out = 28;
    else return fail(kQolErrNotFound);
    return ok();
}

QolResult item_is_real_equip(QolItemHandle, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = 1;
    return ok();
}

QolResult is_jewel_category(int32_t category, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = category >= 28 && category <= 32 ? 1 : 0;
    return ok();
}

QolResult held_count(int32_t, int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = 3;
    return ok();
}

QolResult character_level(int32_t* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = 50;
    return ok();
}

QolResult slot_available(uint32_t index, int32_t* out) {
    if (index >= 3 || out == nullptr) return fail(kQolErrInvalidArgument);
    *out = g_stuff_available ? 1 : 0;
    return ok();
}

QolResult remove_item(QolItemHandle item) {
    if (g_remove_item_calls >= 3) return fail(kQolErrBlocked);
    g_removed[g_remove_item_calls++] = item;
    return ok();
}

QolResult remove_item_data(int32_t, int32_t) { return ok(); }

QolResult write_stuff_need(uint32_t, int32_t, int32_t) {
    ++g_stuff_need_calls;
    g_stuff_need_sequence = ++g_operation_sequence;
    return ok();
}

QolResult set_stuff_item(uint32_t, QolItemHandle item) {
    ++g_stuff_write_calls;
    if (g_stuff_write_result != kQolOk) return fail(g_stuff_write_result);
    g_placed_item = item;
    return ok();
}

QolResult create_perfect_item(int32_t, QolItemHandle* out) {
    ++g_create_calls;
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    *out = kProduct;
    return ok();
}

QolResult create_item(int32_t, int32_t, int32_t, int32_t, QolItemHandle* out) {
    return create_perfect_item(0, out);
}

QolResult save_item(QolItemHandle item, int32_t* out_saved) {
    ++g_save_calls;
    if (out_saved == nullptr) return fail(kQolErrInvalidArgument);
    g_saved_item = item;
    *out_saved = 1;
    return ok();
}

QolResult free_item(QolItemHandle item) {
    ++g_free_calls;
    g_freed_item = item;
    return ok();
}

QolResult read_jewel_word(QolItemHandle item, uint32_t* out) {
    if (out == nullptr || item.slot != kJewel.slot || item.generation != kJewel.generation)
        return fail(kQolErrNotFound);
    g_word_read_item = item;
    *out = g_jewel_word;
    return ok();
}

QolResult write_jewel_word(QolItemHandle item, uint32_t word) {
    if (item.slot != kJewel.slot || item.generation != kJewel.generation)
        return fail(kQolErrNotFound);
    ++g_jewel_write_calls;
    g_jewel_write_sequence = ++g_operation_sequence;
    if (g_jewel_write_result != kQolOk) return fail(g_jewel_write_result);
    g_word_write_item = item;
    g_word_written = word;
    g_jewel_word = word;
    return ok();
}

QolResult set_max_socket_enchant(QolItemHandle) { return ok(); }
QolResult description_set_off() { return ok(); }
int64_t random_inclusive(int lo, int) {
    ++g_random_calls;
    return lo;
}
int math_random(int lo, int) { return lo; }

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

QolResult query_item(QolItemHandle item, const AttributeQualityOptions* options,
                     AttributeQualityItemResult* out) {
    ++g_quality_query_calls;
    g_quality_options_were_null = g_quality_options_were_null && options == nullptr;
    if (!g_quality_query_ok || item.slot != kJewel.slot ||
        item.generation != kJewel.generation || out == nullptr) {
        return fail(kQolErrNotReady);
    }
    const int32_t value = static_cast<int32_t>(g_jewel_word & 0x7ffu);
    const int32_t type = static_cast<int32_t>((g_jewel_word >> 18) & 0x3fu);
    out->complete = g_quality_query_complete ? 1 : 0;
    out->count = g_quality_query_count;
    out->attrs[0] = AttributeQualityItem{
        g_quality_kind, 0, type, 0, value, {0, 100},
        {value, g_quality_level_override >= 0
                    ? g_quality_level_override
                    : quality_level(value, 0, 100)},
        g_quality_status};
    return ok();
}

QolResult query_option_context(QolItemHandle, int32_t, int32_t,
                               const AttributeQualityOptions*, AttributeQualityItem*) {
    return fail(kQolErrUnsupported);
}

QolResult evaluate_quality(int32_t value, AttributeRange range,
                           const AttributeQualityOptions* options, AttributeQuality* out) {
    ++g_quality_evaluate_calls;
    g_quality_options_were_null = g_quality_options_were_null && options == nullptr;
    if (value == g_quality_eval_fail_value) return fail(kQolErrBlocked);
    if (out == nullptr) return fail(kQolErrInvalidArgument);
    out->percentile = range.max <= range.min || value >= range.max
                          ? 100
                          : value < range.min
                                ? 0
                                : static_cast<int32_t>(
                                      (static_cast<int64_t>(value) - range.min) * 100 /
                                      (static_cast<int64_t>(range.max) - range.min));
    out->level = g_quality_level_override >= 0
                     ? g_quality_level_override
                     : quality_level(value, range.min, range.max);
    return ok();
}

AttributeQualityApi g_attribute_quality_api{
    static_cast<uint32_t>(sizeof(AttributeQualityApi)), &query_item,
    &query_option_context, &evaluate_quality};

QolResult capability_require(const char* id, QolVersionRange range, const void** out) {
    ++g_quality_require_calls;
    if (id == nullptr || out == nullptr) return fail(kQolErrInvalidArgument);
    *out = nullptr;
    if (std::strcmp(id, kAttributeQualityCapabilityId) == 0) {
        if (range.major != kAttributeQualityVersionMajor ||
            range.min_minor != kAttributeQualityVersionMinor) {
            return fail(kQolErrUnsupported);
        }
        if (!g_quality_capability_available) return fail(kQolErrNotReady);
        *out = &g_attribute_quality_api;
        return ok();
    }
    return fail(kQolErrNotFound);
}

QolResult set_target_slot_item(QolItemHandle) { return ok(); }

ItemFactsApi g_item_facts{};
CraftFactsApi g_craft_facts{};
QolGameHost g_game{};
QolHostSdk g_sdk{};
QolCustomRecipeWritePort g_write_port{};

}  // namespace

int (*fn_math_get_random)(int, int) = &math_random;

namespace custom_recipe {

void set_context(const qol::contracts::QolGameHost*, const qol::contracts::QolHostSdk*) {}
bool custom_recipe_table_ensure() { return true; }
void custom_recipe_table_deactivate() {}

}  // namespace custom_recipe

int main() {
    g_item_facts.struct_size = sizeof(g_item_facts);
    g_item_facts.category = &item_category;
    g_item_facts.is_real_equip = &item_is_real_equip;
    g_item_facts.is_jewel_category = &is_jewel_category;
    g_craft_facts.read.category_held_count = &held_count;
    g_craft_facts.read.character_level = &character_level;
    g_craft_facts.read_only.stuff_slot_available = &slot_available;

    g_game.inventory_write.remove_item = &remove_item;
    g_game.inventory_write.remove_item_data = &remove_item_data;
    g_sdk.capabilities.require = &capability_require;

    g_write_port.set_stuff_item = &set_stuff_item;
    g_write_port.write_stuff_need = &write_stuff_need;
    g_write_port.description_set_off = &description_set_off;
    g_write_port.create_perfect_item = &create_perfect_item;
    g_write_port.create_item = &create_item;
    g_write_port.save_item = &save_item;
    g_write_port.free_item = &free_item;
    g_write_port.read_jewel_word = &read_jewel_word;
    g_write_port.write_jewel_word = &write_jewel_word;
    g_write_port.set_max_socket_enchant = &set_max_socket_enchant;

    qol::contracts::PluginContext context{};
    context.game = &g_game;
    context.sdk = &g_sdk;
    qol::features::custom_recipe::set_context(context);
    qol::features::custom_recipe::set_item_facts(&g_item_facts);
    qol::features::custom_recipe::set_craft_facts(&g_craft_facts);
    qol::features::custom_recipe::set_write_port(&g_write_port);
    qol::features::custom_recipe::set_enabled(true);
    custom_recipe::bind_base_record_count(69);

    const auto& api = qol::features::custom_recipe::craft_result_api();
    const CraftPlaceInput place{0, kMaterialA, false, 35, 0, 0, 0};
    assert(api.place(&place) == PlaceOutcome::kPlaced);
    assert(g_placed_item.slot == kMaterialA.slot && g_placed_item.generation == kMaterialA.generation);
    g_stuff_write_result = kQolErrNotReady;
    const int writes_before_failure = g_stuff_write_calls;
    assert(api.place(&place) == PlaceOutcome::kWriteFailed);
    assert(g_stuff_write_calls == writes_before_failure + 1);
    assert(g_placed_item.slot == kMaterialA.slot && g_placed_item.generation == kMaterialA.generation);
    g_stuff_write_result = kQolErrBlocked;
    assert(api.place(&place) == PlaceOutcome::kWriteFailed);
    assert(g_placed_item.slot == kMaterialA.slot && g_placed_item.generation == kMaterialA.generation);
    g_stuff_write_result = kQolOk;

    const CraftPrepareInput prepare{{35, 0, 41}};
    assert(api.prepare(&prepare) == PrepareOutcome::kConfirmPending);
    const CraftExecuteInput execute{{35, 0, 41}, {kMaterialA, kInvalidItemHandle, kMaterialB},
                                    {0, 0, 0}};
    assert(api.execute(&execute) == CraftExecuteResult::kOk);
    assert(g_remove_item_calls == 2);
    assert(g_removed[0].slot == kMaterialA.slot && g_removed[0].generation == kMaterialA.generation);
    assert(g_removed[1].slot == kMaterialB.slot && g_removed[1].generation == kMaterialB.generation);
    assert(g_create_calls == 1 && g_save_calls == 1 && g_free_calls == 0);
    assert(g_saved_item.slot == kProduct.slot && g_saved_item.generation == kProduct.generation);

    const uint32_t value_before = g_jewel_word & 0x7FFu;
    const CraftMakeItemInput make_item{70, kJewel, &random_inclusive};
    assert(api.make_item(&make_item) == 0);
    assert(g_word_read_item.slot == kJewel.slot && g_word_read_item.generation == kJewel.generation);
    assert(g_word_write_item.slot == kJewel.slot && g_word_write_item.generation == kJewel.generation);
    assert((g_word_written & 0x7FFu) > value_before);
    assert(g_stuff_need_calls == 1);

    assert(g_quality_require_calls == 1 && g_quality_query_calls == 1);
    assert(g_quality_evaluate_calls > 0 && g_random_calls == 1);
    assert(g_quality_options_were_null);
    assert(g_jewel_write_sequence > 0 && g_jewel_write_sequence < g_stuff_need_sequence);

    const auto assert_quality_failure_has_no_writes = [&]() {
        const int random_before = g_random_calls;
        const int writes_before = g_jewel_write_calls;
        const int needs_before = g_stuff_need_calls;
        const uint32_t word_before = g_jewel_word;
        assert(api.make_item(&make_item) != 0);
        assert(g_random_calls == random_before);
        assert(g_jewel_write_calls == writes_before);
        assert(g_stuff_need_calls == needs_before);
        assert(g_jewel_word == word_before);
    };

    // 每次制作重新 require；能力撤销时不复用上一调用的 API 指针。
    g_quality_capability_available = false;
    const int requires_before_revoke = g_quality_require_calls;
    assert_quality_failure_has_no_writes();
    assert(g_quality_require_calls == requires_before_revoke + 1);
    g_quality_capability_available = true;

    // v2 API 的完整尺寸和三个非空函数指针都是消费前置条件。
    g_attribute_quality_api.struct_size = static_cast<uint32_t>(sizeof(AttributeQualityApi) - 1);
    assert_quality_failure_has_no_writes();
    g_attribute_quality_api.struct_size = static_cast<uint32_t>(sizeof(AttributeQualityApi));

    // 当前物品查询必须完整且唯一，kind/status/level 不可伪装成 Grey。
    g_quality_query_complete = false;
    assert_quality_failure_has_no_writes();
    g_quality_query_complete = true;
    g_quality_query_count = 2;
    assert_quality_failure_has_no_writes();
    g_quality_query_count = 1;
    g_quality_kind = AttributeKind::StoredOption;
    assert_quality_failure_has_no_writes();
    g_quality_kind = AttributeKind::Jewel;
    g_quality_status = kQolErrNotReady;
    assert_quality_failure_has_no_writes();
    g_quality_status = kQolOk;
    g_quality_level_override = kAttributeQualityHighestLevel + 1;
    assert_quality_failure_has_no_writes();
    g_quality_level_override = -1;
    g_quality_query_ok = false;
    assert_quality_failure_has_no_writes();
    g_quality_query_ok = true;

    // evaluate 的候选失败透传，且发生在随机、宝石写入和材料数组更新之前。
    g_quality_eval_fail_value = 50;
    assert_quality_failure_has_no_writes();
    g_quality_eval_fail_value = std::numeric_limits<int32_t>::min();

    // 成功计算后宝石写失败仍不得先改 stuffList。
    const int random_before_write_failure = g_random_calls;
    const int needs_before_write_failure = g_stuff_need_calls;
    const uint32_t word_before_write_failure = g_jewel_word;
    g_jewel_write_result = kQolErrBlocked;
    assert(api.make_item(&make_item) != 0);
    assert(g_random_calls == random_before_write_failure + 1);
    assert(g_stuff_need_calls == needs_before_write_failure);
    assert(g_jewel_word == word_before_write_failure);
    g_jewel_write_result = kQolOk;

    const CraftPlaceInput stale_place{0, {999, 999}, false, 35, 0, 0, 0};
    assert(api.place(&stale_place) == PlaceOutcome::kNotMine);
    assert(g_placed_item.slot == kMaterialA.slot);
    assert(api.prepare(&prepare) == PrepareOutcome::kConfirmPending);
    const CraftExecuteInput stale_execute{{35, 0, 41}, {{999, 999}, kInvalidItemHandle,
                                                         kMaterialB}, {0, 0, 0}};
    assert(api.execute(&stale_execute) == CraftExecuteResult::kStaleSlots);
    assert(g_remove_item_calls == 2);
    assert(g_jewel_write_calls == 2);
    return 0;
}
