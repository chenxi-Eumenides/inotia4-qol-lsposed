#include "plugins/attribute_quality/feature.h"
#include "plugins/attribute_quality/plugin.h"
#include "plugins/attribute_quality/rules.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace contracts = qol::contracts;
namespace feature = qol::features::attribute_quality;

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (condition) {                                                                    \
            ++g_pass;                                                                       \
        } else {                                                                            \
            ++g_fail;                                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
        }                                                                                   \
    } while (false)

constexpr char kOpaqueHostMessage[] = "opaque host failure";
const QolResult kOpaqueHostError{71, 731, kOpaqueHostMessage,
                                  static_cast<uint32_t>(sizeof(kOpaqueHostMessage) - 1)};
constexpr char kWrongThreadMessage[] = "wrong thread";
const QolResult kWrongThreadError{kQolErrBlocked, kQolErrBlocked, kWrongThreadMessage,
                                  static_cast<uint32_t>(sizeof(kWrongThreadMessage) - 1)};

struct FakeItem {
    bool present = false;
    uint32_t generation = 1;
    int32_t category = 0;
    int32_t real_equip = 0;
    contracts::QolItemInventoryView view{};
};

FakeItem g_items[4];
contracts::ItemFactsApi g_item_facts{};
contracts::QolGameHost g_game{};
const contracts::ItemFactsApi* g_required_facts = nullptr;
QolVersionRange g_requested_facts_range{};
int g_require_calls = 0;
int g_category_calls = 0;
int g_jewel_category_calls = 0;
int g_real_equip_calls = 0;
int g_view_calls = 0;
bool g_category_host_error = false;
QolResult g_view_host_result{};

int32_t g_range_min = 0;
int32_t g_range_max = 100;
int32_t g_last_option_id = -1;
int32_t g_last_option_level = -1;
int32_t g_last_option_flag = -1;
int32_t g_last_option_value = -1;
int32_t g_second_wrapper_value = -1;
bool g_repeat_wrapper_call = false;
int32_t g_host_error_option_id = -1;
int32_t g_wrong_thread_option_id = -1;
int32_t g_no_capture_option_id = -1;
bool g_jewel_host_error = false;
int g_backup_calls = 0;

enum class NestedMode { kNone, kBeforeCapture, kAfterCapture };
NestedMode g_nested_mode = NestedMode::kNone;
bool g_nested_called = false;
QolResult g_nested_result{};
contracts::AttributeQualityItem g_nested_output{};

int32_t g_uses_table = 0;
bool g_uses_host_error = false;
bool g_pair_host_error = false;
bool g_pair_not_found = false;
bool g_pair_not_ready = false;
int32_t g_pair_p1 = -50;
int32_t g_pair_p2 = 50;
int g_uses_calls = 0;
int g_pair_calls = 0;
int32_t g_last_pair_option_id = -1;

FakeItem* find_item(contracts::QolItemHandle handle) {
    if (handle.slot == 0 || handle.slot > 4) return nullptr;
    FakeItem& item = g_items[handle.slot - 1];
    if (!item.present || item.generation != handle.generation) return nullptr;
    return &item;
}

QolResult fake_category(contracts::QolItemHandle handle, int32_t* out_category) {
    ++g_category_calls;
    if (g_category_host_error) return kOpaqueHostError;
    if (out_category == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    FakeItem* item = find_item(handle);
    if (item == nullptr) return contracts::error(kQolErrNotFound, "stale fake item", 15);
    *out_category = item->category;
    return contracts::ok();
}

QolResult fake_is_jewel_category(int32_t category, int32_t* out_is_jewel) {
    ++g_jewel_category_calls;
    if (out_is_jewel == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    *out_is_jewel = category == 100 ? 1 : 0;
    return contracts::ok();
}

QolResult fake_is_real_equip(contracts::QolItemHandle handle, int32_t* out_real_equip) {
    ++g_real_equip_calls;
    if (out_real_equip == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    FakeItem* item = find_item(handle);
    if (item == nullptr) return contracts::error(kQolErrNotFound, "stale fake item", 15);
    *out_real_equip = item->real_equip;
    return contracts::ok();
}

QolResult fake_item_view(contracts::QolItemHandle handle,
                         contracts::QolItemInventoryView* out_view) {
    ++g_view_calls;
    if (!contracts::succeeded(g_view_host_result)) return g_view_host_result;
    if (out_view == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    FakeItem* item = find_item(handle);
    if (item == nullptr) return contracts::error(kQolErrNotFound, "stale fake item", 15);
    *out_view = item->view;
    return contracts::ok();
}

QolResult fake_require_facts(const char* id, QolVersionRange range, const void** out_iface) {
    ++g_require_calls;
    g_requested_facts_range = range;
    if (id == nullptr || std::strcmp(id, contracts::kItemFactsCapabilityId) != 0 ||
        out_iface == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "bad capability request", 22);
    }
    *out_iface = g_required_facts;
    return g_required_facts == nullptr
               ? contracts::error(kQolErrNotFound, "item.facts missing", 18)
               : contracts::ok();
}

int fake_backup_math_get_random(int min, int max) {
    ++g_backup_calls;
    if (min > max) return min;
    return static_cast<int>(static_cast<int64_t>(min) +
                            (static_cast<int64_t>(max) - min) / 2);
}

void run_nested_option_context(contracts::QolItemHandle item) {
    if (g_nested_called) return;
    g_nested_called = true;
    g_nested_result = feature::api_query_option_context(item, 999, 123, nullptr,
                                                        &g_nested_output);
}

QolResult fake_option_value(int32_t option_id, int32_t ability_level, int32_t flag,
                            contracts::QolItemHandle item, int32_t* out_value) {
    g_last_option_id = option_id;
    g_last_option_level = ability_level;
    g_last_option_flag = flag;
    if (out_value == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    if (option_id == g_host_error_option_id) return kOpaqueHostError;
    if (option_id == g_wrong_thread_option_id) return kWrongThreadError;
    if (g_nested_mode == NestedMode::kBeforeCapture) run_nested_option_context(item);
    if (flag <= 0) {
        // The game option getter does not call MATH_GetRandom for non-positive flags.
        *out_value = 123;
        g_last_option_value = *out_value;
        return contracts::ok();
    }
    if (option_id == g_no_capture_option_id) {
        *out_value = 123;
        g_last_option_value = *out_value;
        return contracts::ok();
    }
    *out_value = feature::math_get_random_wrapper(g_range_min, g_range_max);
    g_last_option_value = *out_value;
    if (g_repeat_wrapper_call) {
        g_second_wrapper_value = feature::math_get_random_wrapper(g_range_min, g_range_max);
    }
    if (g_nested_mode == NestedMode::kAfterCapture) run_nested_option_context(item);
    return contracts::ok();
}

QolResult fake_jewel_value(int32_t, contracts::QolItemHandle, int32_t* out_value) {
    if (out_value == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    if (g_jewel_host_error) return kOpaqueHostError;
    *out_value = feature::math_get_random_wrapper(g_range_min, g_range_max);
    return contracts::ok();
}

QolResult fake_uses_fixed_option_table(int32_t, int32_t* out_uses) {
    ++g_uses_calls;
    if (out_uses == nullptr) return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    if (g_uses_host_error) return kOpaqueHostError;
    *out_uses = g_uses_table;
    return contracts::ok();
}

QolResult fake_fixed_option_pair(int32_t, int32_t option_id, int32_t* out_p1, int32_t* out_p2) {
    ++g_pair_calls;
    g_last_pair_option_id = option_id;
    if (out_p1 == nullptr || out_p2 == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "bad output", 10);
    }
    if (g_pair_host_error) return kOpaqueHostError;
    if (g_pair_not_ready) return contracts::error(kQolErrNotReady, "pair not ready", 14);
    if (g_pair_not_found) {
        return contracts::error(kQolErrNotFound, "fixed option not found",
                                sizeof("fixed option not found") - 1);
    }
    *out_p1 = g_pair_p1;
    *out_p2 = g_pair_p2;
    return contracts::ok();
}

contracts::QolItemHandle handle(uint32_t slot) { return {slot, 1}; }

void set_item(uint32_t slot, int32_t category, bool is_equip, uint32_t fields,
              int32_t need_level = 0) {
    FakeItem& item = g_items[slot - 1];
    item = {};
    item.present = true;
    item.category = category;
    item.real_equip = is_equip ? 1 : 0;
    item.view.fields = fields;
    item.view.need_level = need_level;
}

void append_option(uint32_t slot, int32_t option_id, int32_t type, int32_t value) {
    FakeItem& item = g_items[slot - 1];
    if (type == static_cast<int32_t>(contracts::QolItemInventoryOptionType::kNative)) {
        const uint32_t ordinal = item.view.native_option_count++;
        item.view.native_options[ordinal] = {value, option_id, type};
    } else if (type == static_cast<int32_t>(contracts::QolItemInventoryOptionType::kJewel)) {
        const uint32_t ordinal = item.view.jewel_option_count++;
        item.view.jewel_options[ordinal] = {value, option_id, type};
    }
}

void reset_fixture() {
    feature::clear_runtime();
    for (FakeItem& item : g_items) item = {};
    g_item_facts = {};
    g_item_facts.struct_size = sizeof(g_item_facts);
    g_item_facts.category = &fake_category;
    g_item_facts.is_real_equip = &fake_is_real_equip;
    g_item_facts.is_jewel_category = &fake_is_jewel_category;
    g_item_facts.view = &fake_item_view;

    g_game = {};
    g_game.abi = {sizeof(g_game), contracts::kGameHostMajor, contracts::kGameHostMinor};
    g_game.read.attr_range = {&fake_option_value, &fake_jewel_value,
                              &fake_uses_fixed_option_table, &fake_fixed_option_pair};

    g_required_facts = &g_item_facts;
    g_requested_facts_range = {};
    g_require_calls = 0;
    g_category_calls = 0;
    g_jewel_category_calls = 0;
    g_real_equip_calls = 0;
    g_view_calls = 0;
    g_category_host_error = false;
    g_view_host_result = contracts::ok();
    g_range_min = 0;
    g_range_max = 100;
    g_last_option_id = -1;
    g_last_option_level = -1;
    g_last_option_flag = -1;
    g_last_option_value = -1;
    g_second_wrapper_value = -1;
    g_repeat_wrapper_call = false;
    g_host_error_option_id = -1;
    g_wrong_thread_option_id = -1;
    g_no_capture_option_id = -1;
    g_jewel_host_error = false;
    g_backup_calls = 0;
    g_nested_mode = NestedMode::kNone;
    g_nested_called = false;
    g_nested_result = {};
    g_nested_output = {};
    g_uses_table = 0;
    g_uses_host_error = false;
    g_pair_host_error = false;
    g_pair_not_found = false;
    g_pair_not_ready = false;
    g_pair_p1 = -50;
    g_pair_p2 = 50;
    g_uses_calls = 0;
    g_pair_calls = 0;
    g_last_pair_option_id = -1;
}

void ready_provider() {
    contracts::PluginContext ctx{};
    ctx.game = &g_game;
    feature::set_context(ctx);
    feature::set_item_facts(&g_item_facts);
    const int32_t ready = feature::on_resource_ready(
        ctx, feature::kResourceIdMathGetRandom,
        reinterpret_cast<void*>(&fake_backup_math_get_random));
    CHECK(ready == 0);
    feature::set_active(true);
}

void test_descriptor_and_install_gates() {
    reset_fixture();
    const contracts::PluginDescriptor& descriptor = feature::plugin_descriptor();
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(descriptor.provides[0].version.major == 2 && descriptor.provides[0].version.minor == 0);
    const auto* api = static_cast<const contracts::AttributeQualityApi*>(descriptor.provides[0].iface);
    CHECK(contracts::attribute_quality_api_size_valid(api));
    CHECK(!contracts::attribute_quality_api_size_valid(nullptr));
    contracts::AttributeQualityApi incomplete_api = *api;
    incomplete_api.struct_size = static_cast<uint32_t>(offsetof(
        contracts::AttributeQualityApi, evaluate));
    CHECK(!contracts::attribute_quality_api_size_valid(&incomplete_api));
    incomplete_api = *api;
    incomplete_api.evaluate = nullptr;
    CHECK(!contracts::attribute_quality_api_size_valid(&incomplete_api));
    CHECK(descriptor.requires_count == 1 && descriptor.requires != nullptr);
    CHECK(descriptor.requires[0].range.major == 3 && descriptor.requires[0].range.min_minor == 6);

    contracts::QolHostSdk sdk{};
    sdk.capabilities.require = &fake_require_facts;
    contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(ctx);
    ctx.sdk = &sdk;
    ctx.game = &g_game;
    CHECK(descriptor.install(ctx) == 0);
    CHECK(g_require_calls == 1);
    CHECK(g_requested_facts_range.major == 3 && g_requested_facts_range.min_minor == 6);
    CHECK(feature::active());
    CHECK(descriptor.on_resource_ready(ctx, feature::kResourceIdMathGetRandom,
                                       reinterpret_cast<void*>(&fake_backup_math_get_random)) == 0);
    descriptor.deactivate(ctx);
    CHECK(!feature::active());

    reset_fixture();
    g_game.abi.minor = 2;
    sdk = {};
    sdk.capabilities.require = &fake_require_facts;
    ctx.sdk = &sdk;
    ctx.game = &g_game;
    CHECK(descriptor.install(ctx) != 0);
    CHECK(g_require_calls == 0);

    reset_fixture();
    g_game.read.attr_range.fixed_option_percent_pair = nullptr;
    ctx.game = &g_game;
    CHECK(descriptor.install(ctx) != 0);

    reset_fixture();
    contracts::ItemFactsApi invalid_facts = g_item_facts;
    invalid_facts.view = nullptr;
    g_required_facts = &invalid_facts;
    ctx.game = &g_game;
    CHECK(descriptor.install(ctx) != 0);
}

void test_backup_fail_closed_and_disabled_evaluation() {
    reset_fixture();
    contracts::PluginContext ctx{};
    ctx.game = &g_game;
    CHECK(feature::on_resource_ready(ctx, feature::kResourceIdMathGetRandom, nullptr) != 0);
    CHECK(feature::math_get_random_wrapper(10, 20) == INT_MIN);

    contracts::AttributeQuality quality{99, 99};
    CHECK(feature::api_evaluate(30, contracts::AttributeRange{0, 100}, nullptr, &quality).ok ==
          kQolOk);
    CHECK(quality.percentile == 30 && quality.level == 1);
    set_item(1, 200, true,
             contracts::kQolItemInventoryHasOptionCompleteness |
                 contracts::kQolItemInventoryHasNeedLevel,
             12);
    ready_provider();
    CHECK(feature::math_get_random_wrapper(10, 20) == 15);
    CHECK(g_backup_calls == 1);
    feature::set_active(false);
    contracts::AttributeQualityItem context{};
    const QolResult disabled = feature::api_query_option_context(handle(1), 4, 25, nullptr,
                                                                 &context);
    CHECK(disabled.code == kQolErrNotReady);
    CHECK(context.status == kQolErrNotReady);
}

void test_query_item_jewel_and_context_errors() {
    reset_fixture();
    constexpr uint32_t view_fields = contracts::kQolItemInventoryHasOptionCompleteness |
                                     contracts::kQolItemInventoryHasJewelValue |
                                     contracts::kQolItemInventoryHasJewelAttributeType;
    set_item(1, 100, false, view_fields);
    g_items[0].view.jewel_attribute_type = 7;
    g_items[0].view.jewel_value = 50;
    ready_provider();

    contracts::AttributeQualityItemResult batch{};
    CHECK(feature::api_query_item(handle(1), nullptr, &batch).ok == kQolOk);
    CHECK(g_category_calls == 1 && g_jewel_category_calls == 1 && g_real_equip_calls == 1 &&
          g_view_calls == 1);
    CHECK(batch.count == 1 && batch.complete == 1);
    CHECK(batch.attrs[0].kind == contracts::AttributeKind::Jewel);
    CHECK(batch.attrs[0].ordinal == 0 && batch.attrs[0].attribute_id == 7 &&
          batch.attrs[0].option_type == -1 && batch.attrs[0].value == 50);
    CHECK(batch.attrs[0].range.min == 0 && batch.attrs[0].range.max == 100);
    CHECK(batch.attrs[0].quality.percentile == 50 && batch.attrs[0].quality.level == 1);
    CHECK(batch.attrs[0].status == kQolOk);

    contracts::AttributeQualityItem jewel_context{};
    CHECK(feature::api_query_option_context(handle(1), 7, 50, nullptr, &jewel_context).ok ==
          kQolOk);
    CHECK(jewel_context.kind == contracts::AttributeKind::OptionContext &&
          jewel_context.ordinal == UINT32_MAX && jewel_context.attribute_id == 7 &&
          jewel_context.option_type == -1 && jewel_context.value == 50);
    CHECK(jewel_context.range.min == 0 && jewel_context.range.max == 100);
    CHECK(jewel_context.quality.percentile == 50 &&
          jewel_context.quality.level == 1 && jewel_context.status == kQolOk);

    reset_fixture();
    set_item(1, 100, false, contracts::kQolItemInventoryHasOptionCompleteness);
    ready_provider();
    jewel_context = {};
    const QolResult missing_jewel =
        feature::api_query_option_context(handle(1), 7, 50, nullptr, &jewel_context);
    CHECK(missing_jewel.code == kQolErrNotReady && jewel_context.status == kQolErrNotReady);

    reset_fixture();
    set_item(1, 100, false, view_fields);
    g_items[0].view.jewel_attribute_type = 7;
    g_items[0].view.jewel_value = 50;
    ready_provider();
    jewel_context = {};
    const QolResult mismatched_jewel =
        feature::api_query_option_context(handle(1), 8, 50, nullptr, &jewel_context);
    CHECK(mismatched_jewel.code == kQolErrUnsupported &&
          jewel_context.status == kQolErrUnsupported);

    reset_fixture();
    set_item(1, 300, false, 0);
    ready_provider();
    batch = {};
    CHECK(feature::api_query_item(handle(1), nullptr, &batch).code == kQolErrUnsupported);
    CHECK(batch.count == 0 && batch.complete == 0);

    reset_fixture();
    set_item(1, 200, true, contracts::kQolItemInventoryHasNeedLevel, 12);
    ready_provider();
    batch = {};
    CHECK(feature::api_query_item(handle(1), nullptr, &batch).code == kQolErrNotReady);
    CHECK(batch.count == 0);

    reset_fixture();
    set_item(1, 200, true, contracts::kQolItemInventoryHasOptionCompleteness, 12);
    ready_provider();
    batch = {};
    CHECK(feature::api_query_item(handle(1), nullptr, &batch).code == kQolErrNotReady);
    CHECK(batch.count == 0);

    reset_fixture();
    set_item(1, 200, true, contracts::kQolItemInventoryHasOptionCompleteness, 12);
    g_category_host_error = true;
    ready_provider();
    batch = {};
    const QolResult facts_error = feature::api_query_item(handle(1), nullptr, &batch);
    CHECK(facts_error.ok == kOpaqueHostError.ok && facts_error.code == kOpaqueHostError.code &&
          facts_error.message == kOpaqueHostError.message &&
          facts_error.message_len == kOpaqueHostError.message_len);
    CHECK(batch.count == 0 && batch.complete == 0);

    contracts::AttributeQualityItem context{};
    const QolResult context_error = feature::api_query_option_context(
        handle(1), 2, 9, nullptr, &context);
    CHECK(context_error.ok == kOpaqueHostError.ok && context_error.code == kOpaqueHostError.code &&
          context_error.message == kOpaqueHostError.message);
    CHECK(context.status == kOpaqueHostError.code);

    g_category_host_error = false;
    batch = {};
    const QolResult stale = feature::api_query_item(handle(4), nullptr, &batch);
    CHECK(stale.code == kQolErrNotFound && batch.count == 0);
    context = {};
    const QolResult stale_context =
        feature::api_query_option_context(handle(4), 2, 9, nullptr, &context);
    CHECK(stale_context.code == kQolErrNotFound && context.status == kQolErrNotFound);
}

void test_query_item_order_status_and_completeness() {
    reset_fixture();
    constexpr uint32_t flags = contracts::kQolItemInventoryHasOptionCompleteness |
                               contracts::kQolItemInventoryHasNeedLevel;
    set_item(1, 200, true, flags, 17);
    append_option(1, 7, 0, 10);
    append_option(1, 7, 1, 11);
    append_option(1, 13, 0, 12);
    g_host_error_option_id = 13;
    ready_provider();

    contracts::AttributeQualityItemResult batch{};
    CHECK(feature::api_query_item(handle(1), nullptr, &batch).ok == kQolOk);
    CHECK(batch.count == 3 && batch.complete == 1);
    CHECK(batch.attrs[0].ordinal == 0 && batch.attrs[0].attribute_id == 7 &&
          batch.attrs[0].value == 10 && batch.attrs[0].status == kQolOk);
    CHECK(batch.attrs[0].range.min == 0 && batch.attrs[0].range.max == 100);
    CHECK(batch.attrs[1].ordinal == 1 && batch.attrs[1].attribute_id == 13 &&
          batch.attrs[1].status == kOpaqueHostError.code);
    CHECK(batch.attrs[1].range.min == 0 && batch.attrs[1].range.max == 0);
    CHECK(batch.attrs[2].ordinal == 2 && batch.attrs[2].attribute_id == 7 &&
          batch.attrs[2].option_type == 1 && batch.attrs[2].status == kQolErrUnsupported);
    CHECK(g_last_option_level == 17 && g_last_option_flag == 1);

    reset_fixture();
    set_item(1, 200, true, flags, 20);
    ready_provider();
    contracts::AttributeQualityItemResult empty{};
    CHECK(feature::api_query_item(handle(1), nullptr, &empty).ok == kQolOk);
    CHECK(empty.count == 0 && empty.complete == 1);

    for (uint32_t count : {31u, 32u}) {
        reset_fixture();
        set_item(1, 200, true, flags, 20);
        for (uint32_t i = 0; i < count; ++i) append_option(1, 9, 0, static_cast<int32_t>(i));
        ready_provider();
        contracts::AttributeQualityItemResult result{};
        CHECK(feature::api_query_item(handle(1), nullptr, &result).ok == kQolOk);
        CHECK(result.count == count && result.complete == 1);
        CHECK(result.attrs[count - 1].ordinal == count - 1);
    }

    reset_fixture();
    set_item(1, 200, true, flags | contracts::kQolItemInventoryOptionsTruncated, 20);
    for (uint32_t i = 0; i < 32; ++i) append_option(1, 9, 0, static_cast<int32_t>(i));
    ready_provider();
    contracts::AttributeQualityItemResult truncated{};
    CHECK(feature::api_query_item(handle(1), nullptr, &truncated).ok == kQolOk);
    CHECK(truncated.count == 32 && truncated.complete == 0);
}

void test_option_range_capture_fixed_table_and_failures() {
    reset_fixture();
    constexpr uint32_t flags = contracts::kQolItemInventoryHasOptionCompleteness |
                               contracts::kQolItemInventoryHasNeedLevel;
    set_item(1, 200, true, flags, 23);
    ready_provider();

    contracts::AttributeQualityItem context{};
    CHECK(feature::api_query_option_context(handle(1), 99, 100, nullptr, &context).ok == kQolOk);
    CHECK(g_last_option_id == 99 && g_last_option_level == 23 && g_last_option_flag == 1);
    CHECK(context.kind == contracts::AttributeKind::OptionContext &&
          context.ordinal == UINT32_MAX && context.attribute_id == 99 &&
          context.option_type == -1 && context.value == 100);
    CHECK(context.status == kQolOk && context.range.min == 0 && context.range.max == 100);

    g_range_min = 10;
    g_range_max = 20;
    g_repeat_wrapper_call = true;
    context = {};
    CHECK(feature::api_query_option_context(handle(1), 99, 16, nullptr, &context).ok == kQolOk);
    CHECK(context.range.min == 10 && context.range.max == 20);
    CHECK(g_last_option_value == 20 && g_second_wrapper_value == 15 && g_backup_calls == 1);

    reset_fixture();
    set_item(1, 200, true, flags, 23);
    g_uses_table = 1;
    g_range_min = 0;
    g_range_max = 100;
    ready_provider();
    context = {};
    CHECK(feature::api_query_option_context(handle(1), 12, 100, nullptr, &context).ok == kQolOk);
    CHECK(context.range.min == 50 && context.range.max == 150);
    CHECK(g_pair_calls == 1 && g_last_pair_option_id == 12);

    reset_fixture();
    set_item(1, 200, true, flags, 23);
    g_uses_table = 1;
    g_pair_not_found = true;
    ready_provider();
    context = {};
    const QolResult not_found =
        feature::api_query_option_context(handle(1), 12, 100, nullptr, &context);
    CHECK(not_found.code == kQolErrNotFound && context.status == not_found.code);
    CHECK(not_found.message != nullptr && std::strcmp(not_found.message, "fixed option not found") == 0);
    CHECK(context.quality.percentile == 0);

    reset_fixture();
    set_item(1, 200, true, flags, 23);
    g_uses_host_error = true;
    ready_provider();
    context = {};
    const QolResult uses_error =
        feature::api_query_option_context(handle(1), 12, 100, nullptr, &context);
    CHECK(uses_error.ok == kOpaqueHostError.ok && uses_error.code == kOpaqueHostError.code &&
          uses_error.message == kOpaqueHostError.message && context.status == uses_error.code);

    reset_fixture();
    set_item(1, 200, true,
             contracts::kQolItemInventoryHasOptionCompleteness |
                 contracts::kQolItemInventoryHasNeedLevel,
             23);
    append_option(1, 12, 0, 100);
    g_uses_table = 1;
    g_uses_host_error = true;
    ready_provider();
    contracts::AttributeQualityItemResult table_error_batch{};
    CHECK(feature::api_query_item(handle(1), nullptr, &table_error_batch).ok == kQolOk);
    CHECK(table_error_batch.count == 1 &&
          table_error_batch.attrs[0].status == kOpaqueHostError.code);
    CHECK(table_error_batch.attrs[0].quality.percentile == 0);

    reset_fixture();
    set_item(1, 200, true, flags, 23);
    g_uses_table = 1;
    g_pair_host_error = true;
    ready_provider();
    context = {};
    const QolResult pair_error =
        feature::api_query_option_context(handle(1), 12, 100, nullptr, &context);
    CHECK(pair_error.ok == kOpaqueHostError.ok && pair_error.code == kOpaqueHostError.code &&
          pair_error.message == kOpaqueHostError.message && context.status == pair_error.code);

    reset_fixture();
    set_item(1, 200, true,
             contracts::kQolItemInventoryHasOptionCompleteness |
                 contracts::kQolItemInventoryHasNeedLevel,
             23);
    append_option(1, 12, 0, 100);
    g_uses_table = 1;
    g_pair_not_found = true;
    ready_provider();
    contracts::AttributeQualityItemResult pair_not_found_batch{};
    CHECK(feature::api_query_item(handle(1), nullptr, &pair_not_found_batch).ok == kQolOk);
    CHECK(pair_not_found_batch.count == 1 &&
          pair_not_found_batch.attrs[0].status == kQolErrNotFound);
    CHECK(pair_not_found_batch.attrs[0].quality.percentile == 0);

    reset_fixture();
    set_item(1, 200, true,
             contracts::kQolItemInventoryHasOptionCompleteness |
                 contracts::kQolItemInventoryHasNeedLevel,
             23);
    append_option(1, 12, 0, 100);
    g_uses_table = 1;
    g_pair_not_ready = true;
    ready_provider();
    contracts::AttributeQualityItemResult pair_not_ready_batch{};
    CHECK(feature::api_query_item(handle(1), nullptr, &pair_not_ready_batch).ok == kQolOk);
    CHECK(pair_not_ready_batch.count == 1 &&
          pair_not_ready_batch.attrs[0].status == kQolErrNotReady);
    CHECK(pair_not_ready_batch.attrs[0].quality.percentile == 0);

    reset_fixture();
    set_item(1, 200, true, flags, 23);
    g_no_capture_option_id = 12;
    ready_provider();
    context = {};
    const QolResult no_capture =
        feature::api_query_option_context(handle(1), 12, 100, nullptr, &context);
    CHECK(no_capture.code == kQolErrNotReady && context.status == no_capture.code);
    CHECK(g_backup_calls == 0);
    g_no_capture_option_id = -1;
    context = {};
    CHECK(feature::api_query_option_context(handle(1), 12, 100, nullptr, &context).ok == kQolOk);

    reset_fixture();
    set_item(1, 200, true, flags, 23);
    g_host_error_option_id = 13;
    ready_provider();
    context = {};
    const QolResult option_error =
        feature::api_query_option_context(handle(1), 13, 100, nullptr, &context);
    CHECK(option_error.ok == kOpaqueHostError.ok && option_error.code == kOpaqueHostError.code &&
          option_error.message == kOpaqueHostError.message && context.status == option_error.code);
    g_host_error_option_id = -1;
    context = {};
    CHECK(feature::api_query_option_context(handle(1), 13, 100, nullptr, &context).ok == kQolOk);

    g_wrong_thread_option_id = 14;
    context = {};
    const QolResult wrong_thread =
        feature::api_query_option_context(handle(1), 14, 100, nullptr, &context);
    CHECK(wrong_thread.ok == kWrongThreadError.ok && wrong_thread.code == kWrongThreadError.code &&
          wrong_thread.message == kWrongThreadError.message && context.status == wrong_thread.code);
}

void test_capture_scope_nested_guard() {
    for (NestedMode mode : {NestedMode::kBeforeCapture, NestedMode::kAfterCapture}) {
        reset_fixture();
        set_item(1, 200, true,
                 contracts::kQolItemInventoryHasOptionCompleteness |
                     contracts::kQolItemInventoryHasNeedLevel,
                 14);
        ready_provider();
        g_nested_mode = mode;
        contracts::AttributeQualityItem context{};
        CHECK(feature::api_query_option_context(handle(1), 17, 40, nullptr, &context).ok == kQolOk);
        CHECK(g_nested_called);
        CHECK(g_nested_result.code == kQolErrBusy);
        CHECK(g_nested_output.status == kQolErrBusy);
        CHECK(context.status == kQolOk);
        CHECK(context.range.min == g_range_min && context.range.max == g_range_max);
    }
}

void test_item_error_status_and_option_validation() {
    reset_fixture();
    set_item(1, 200, true,
             contracts::kQolItemInventoryHasOptionCompleteness |
                 contracts::kQolItemInventoryHasNeedLevel,
             10);
    append_option(1, 21, 0, 30);
    g_host_error_option_id = 21;
    ready_provider();
    contracts::AttributeQualityItemResult batch{};
    CHECK(feature::api_query_item(handle(1), nullptr, &batch).ok == kQolOk);
    CHECK(batch.count == 1 && batch.attrs[0].status == kOpaqueHostError.code);

    contracts::AttributeQualityOptions invalid{0, {0, 1, 0}};
    batch = {};
    const QolResult invalid_result = feature::api_query_item(handle(1), &invalid, &batch);
    CHECK(invalid_result.code == kQolErrInvalidArgument && batch.count == 0);
    contracts::AttributeQualityItem context{};
    const QolResult invalid_context =
        feature::api_query_option_context(handle(1), -1, 30, nullptr, &context);
    CHECK(invalid_context.code == kQolErrInvalidArgument &&
          context.status == kQolErrInvalidArgument);
    CHECK(feature::api_query_item(handle(1), nullptr, nullptr).code == kQolErrInvalidArgument);
}

}  // namespace

int main() {
    test_descriptor_and_install_gates();
    test_backup_fail_closed_and_disabled_evaluation();
    test_query_item_jewel_and_context_errors();
    test_query_item_order_status_and_completeness();
    test_option_range_capture_fixed_table_and_failures();
    test_capture_scope_nested_guard();
    test_item_error_status_and_option_validation();
    std::printf("attribute_quality_provider_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
