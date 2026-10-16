#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "contracts/attribute_quality.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"
#include "plugins/attribute_range_display/feature.h"
#include "plugins/attribute_range_display/plugin.h"

namespace {

enum class QueryMode { Success, Error, ItemFailure, InvalidLevel };

int g_add_calls = 0;
int g_make_calls = 0;
int g_register_calls = 0;
int g_query_calls = 0;
int g_insert_calls = 0;
int g_fail_cursor_call = 0;
bool g_emit_add_from_make = false;
bool g_quality_available = true;
bool g_use_explicit_quality_level = false;
int32_t g_explicit_quality_level = 0;
int32_t g_register_status = qol::contracts::kQolItemHandleStatusOk;
QueryMode g_query_mode = QueryMode::Success;
const qol::contracts::AttributeQualityApi* g_quality_api = nullptr;
int g_option_type = 0;
int g_option_id = 0;
int g_display_value = 0;
int g_last_query_option_id = -1;
int g_last_query_value = -1;
uint8_t g_last_color = 0;
uint32_t g_last_before_offset = 0;
uint32_t g_last_after_offset = 0;
int g_cursor_calls = 0;
void* g_last_builder = nullptr;

struct FakeBuilder {
    uint32_t cursor;
};

qol::contracts::QolHostSdk g_sdk{};
qol::contracts::QolUiAdapter g_ui{};
qol::contracts::PluginContext g_context{};

QolResult query_item(qol::contracts::QolItemHandle,
                     const qol::contracts::AttributeQualityOptions*,
                     qol::contracts::AttributeQualityItemResult*) {
    return qol::contracts::ok();
}

QolResult query_option_context(
    qol::contracts::QolItemHandle handle, int32_t option_id, int32_t display_value,
    const qol::contracts::AttributeQualityOptions* options,
    qol::contracts::AttributeQualityItem* out) {
    ++g_query_calls;
    assert(handle.slot != 0 && handle.generation != 0);
    assert(options != nullptr && options->allow_outside_range == 0);
    g_last_query_option_id = option_id;
    g_last_query_value = display_value;
    if (g_query_mode == QueryMode::Error)
        return qol::contracts::error(kQolErrNotReady, "query error", 11);
    if (out == nullptr)
        return qol::contracts::error(kQolErrInvalidArgument, "missing output", 14);

    *out = {};
    out->kind = qol::contracts::AttributeKind::OptionContext;
    out->status = g_query_mode == QueryMode::ItemFailure
                      ? qol::contracts::kQolItemHandleStatusNotFound
                      : qol::contracts::kQolItemHandleStatusOk;
    if (g_query_mode == QueryMode::InvalidLevel) {
        out->quality.level = qol::contracts::kAttributeQualityHighestLevel + 1;
    } else if (g_use_explicit_quality_level) {
        out->quality.level = g_explicit_quality_level;
    } else {
        // Current display value determines the result; no cached option-list lookup is used.
        out->quality.level = display_value == 88 ? 5 : 2;
    }
    return qol::contracts::ok();
}

QolResult evaluate(int32_t, qol::contracts::AttributeRange,
                   const qol::contracts::AttributeQualityOptions*,
                   qol::contracts::AttributeQuality*) {
    return qol::contracts::ok();
}

qol::contracts::AttributeQualityApi g_default_quality_api{
    sizeof(qol::contracts::AttributeQualityApi), query_item, query_option_context, evaluate};

QolResult require_quality(const char* id, QolVersionRange range, const void** out_iface) {
    assert(id != nullptr);
    assert(range.major == qol::contracts::kAttributeQualityVersionMajor);
    if (!g_quality_available || out_iface == nullptr || g_quality_api == nullptr)
        return qol::contracts::error(kQolErrNotReady, "quality unavailable", 19);
    *out_iface = g_quality_api;
    return qol::contracts::ok();
}

int32_t register_item(void* item, uint32_t owner, qol::contracts::QolItemHandle* out) {
    ++g_register_calls;
    assert(item != nullptr);
    assert(owner == qol::contracts::kQolItemHandleOwnerGame);
    if (g_register_status != qol::contracts::kQolItemHandleStatusOk) return g_register_status;
    if (out == nullptr) return qol::contracts::kQolItemHandleStatusInvalidArgument;
    *out = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(item)), 3};
    return qol::contracts::kQolItemHandleStatusOk;
}

QolResult ui_desc_text_cursor(void* builder, uint32_t* out_offset) {
    ++g_cursor_calls;
    if (g_cursor_calls == g_fail_cursor_call) {
        return qol::contracts::error(kQolErrNotReady, "cursor unavailable", 18);
    }
    if (builder == nullptr || out_offset == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid cursor", 14);
    }
    *out_offset = static_cast<FakeBuilder*>(builder)->cursor;
    return qol::contracts::ok();
}

QolResult insert_inline_color_span(void* builder, uint32_t before, uint32_t after,
                                   uint8_t color) {
    ++g_insert_calls;
    g_last_builder = builder;
    g_last_before_offset = before;
    g_last_after_offset = after;
    g_last_color = color;
    static_cast<FakeBuilder*>(builder)->cursor += 4;
    return qol::contracts::ok();
}

void original_add(void* builder, int, int, int) {
    ++g_add_calls;
    static_cast<FakeBuilder*>(builder)->cursor += 8;
}

uintptr_t original_make(void* item, void*, void*) {
    ++g_make_calls;
    if (g_emit_add_from_make) {
        FakeBuilder builder{20};
        qol::features::attribute_range_display::uid_desc_add_option_wrapper(
            &builder, g_option_type, g_option_id, g_display_value);
    }
    return reinterpret_cast<uintptr_t>(item) + 1;
}

void reset_quality_state() {
    g_quality_available = true;
    g_use_explicit_quality_level = false;
    g_explicit_quality_level = 0;
    g_quality_api = &g_default_quality_api;
    g_register_status = qol::contracts::kQolItemHandleStatusOk;
    g_query_mode = QueryMode::Success;
    g_query_calls = 0;
    g_register_calls = 0;
    g_insert_calls = 0;
    g_cursor_calls = 0;
    g_fail_cursor_call = 0;
    g_last_query_option_id = -1;
    g_last_query_value = -1;
    g_last_color = 0;
    g_last_before_offset = 0;
    g_last_after_offset = 0;
}

void add_with_context(void* item, int option_id, int display_value) {
    g_emit_add_from_make = true;
    g_option_type = 7;
    g_option_id = option_id;
    g_display_value = display_value;
    assert(qol::features::attribute_range_display::uid_desc_make_item_wrapper(
               item, nullptr, nullptr) == reinterpret_cast<uintptr_t>(item) + 1);
    g_emit_add_from_make = false;
}

void test_disabled_wrappers_pass_through() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(false);

    FakeBuilder builder{20};
    qol::features::attribute_range_display::uid_desc_add_option_wrapper(&builder, 0, 1, 3);
    assert(g_add_calls == 1);
    assert(g_register_calls == 0 && g_query_calls == 0 && g_insert_calls == 0);

    void* item = reinterpret_cast<void*>(0x1000);
    assert(qol::features::attribute_range_display::uid_desc_make_item_wrapper(
               item, nullptr, nullptr) == 0x1001);
    assert(g_make_calls == 1);
}

void test_active_context_uses_provider_level_color() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    void* item = reinterpret_cast<void*>(0x1000);

    add_with_context(item, 12, 41);
    assert(g_add_calls == 2);
    assert(g_make_calls == 2);
    assert(g_register_calls == 1 && g_query_calls == 1 && g_insert_calls == 1);
    assert(g_cursor_calls == 2);
    assert(g_last_query_option_id == 12 && g_last_query_value == 41);
    assert(g_last_color == static_cast<uint8_t>('C'));
    assert(g_last_before_offset == 20 && g_last_after_offset == 28);
    assert(g_last_builder != nullptr);
}

void test_current_value_disambiguates_repeated_option_id() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    void* item = reinterpret_cast<void*>(0x2000);

    add_with_context(item, 12, 41);
    add_with_context(item, 12, 88);
    assert(g_query_calls == 2 && g_insert_calls == 2);
    assert(g_last_query_option_id == 12 && g_last_query_value == 88);
    assert(g_last_color == static_cast<uint8_t>('Y'));
    assert(g_last_before_offset == 20 && g_last_after_offset == 28);
}

void test_cursor_snapshots_bracket_backup_and_unsupported_adapter_fails_closed() {
    void* item = reinterpret_cast<void*>(0x2800);
    qol::features::attribute_range_display::set_active(true);

    reset_quality_state();
    const int add_calls_before = g_add_calls;
    g_fail_cursor_call = 1;
    add_with_context(item, 12, 41);
    assert(g_add_calls == add_calls_before + 1);
    assert(g_cursor_calls == 1 && g_register_calls == 0 && g_query_calls == 0 &&
           g_insert_calls == 0);

    reset_quality_state();
    const int add_calls_before_after_failure = g_add_calls;
    g_fail_cursor_call = 2;
    add_with_context(item, 12, 41);
    assert(g_add_calls == add_calls_before_after_failure + 1);
    assert(g_cursor_calls == 2 && g_register_calls == 0 && g_query_calls == 0 &&
           g_insert_calls == 0);

    reset_quality_state();
    const uint16_t current_minor = g_ui.minor;
    g_ui.minor = qol::contracts::kUiAdapterMinor - 1;
    const int add_calls_before_old_adapter = g_add_calls;
    add_with_context(item, 12, 41);
    assert(g_add_calls == add_calls_before_old_adapter + 1);
    assert(g_cursor_calls == 0 && g_register_calls == 0 && g_query_calls == 0 &&
           g_insert_calls == 0);
    g_ui.minor = current_minor;

    reset_quality_state();
    g_ui.insert_inline_color_span = nullptr;
    const int add_calls_before_missing_slot = g_add_calls;
    add_with_context(item, 12, 41);
    assert(g_add_calls == add_calls_before_missing_slot + 1);
    assert(g_cursor_calls == 0 && g_register_calls == 0 && g_query_calls == 0 &&
           g_insert_calls == 0);
    g_ui.insert_inline_color_span = insert_inline_color_span;
}

void test_unavailable_or_invalid_quality_fails_closed() {
    void* item = reinterpret_cast<void*>(0x3000);
    qol::features::attribute_range_display::set_active(true);

    reset_quality_state();
    g_quality_available = false;
    add_with_context(item, 4, 41);
    assert(g_register_calls == 0 && g_query_calls == 0 && g_insert_calls == 0);

    reset_quality_state();
    auto malformed_api = g_default_quality_api;
    malformed_api.struct_size = offsetof(qol::contracts::AttributeQualityApi, evaluate);
    g_quality_api = &malformed_api;
    add_with_context(item, 4, 41);
    assert(g_register_calls == 0 && g_query_calls == 0 && g_insert_calls == 0);
}

void test_query_and_game_handle_errors_fail_closed() {
    void* item = reinterpret_cast<void*>(0x4000);
    qol::features::attribute_range_display::set_active(true);

    reset_quality_state();
    g_query_mode = QueryMode::Error;
    add_with_context(item, 5, 41);
    assert(g_register_calls == 1 && g_query_calls == 1 && g_insert_calls == 0);

    reset_quality_state();
    g_register_status = qol::contracts::kQolItemHandleStatusNotFound;
    add_with_context(item, 5, 41);
    assert(g_register_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);

    reset_quality_state();
    g_query_mode = QueryMode::ItemFailure;
    add_with_context(item, 5, 41);
    assert(g_query_calls == 1 && g_insert_calls == 0);
}

void test_descriptor_contract() {
    const auto& descriptor = qol::features::attribute_range_display::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "attribute_range_display") == 0);
    assert(std::strcmp(descriptor.version, "0.1.0") == 0);
    assert(descriptor.requires_count == 1 && descriptor.requires != nullptr);
    assert(std::strcmp(descriptor.requires[0].id, "attribute.quality") == 0);
    assert(descriptor.requires[0].range.major ==
           qol::contracts::kAttributeQualityVersionMajor);
    assert(descriptor.requires[0].range.min_minor ==
           qol::contracts::kAttributeQualityVersionMinor);
    assert(descriptor.optional_count == 0 && descriptor.optional_requires == nullptr);
    assert(descriptor.port_count == 0 && descriptor.ports == nullptr);
    assert(descriptor.provides_count == 0 && descriptor.provides == nullptr);
    assert(descriptor.resources_count == 2 && descriptor.resources != nullptr);
    assert(descriptor.feature_count == 1 && descriptor.features != nullptr);
    assert(std::strcmp(descriptor.features[0].id, "attribute_range_display.ranges") == 0);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.features[0].category == qol::contracts::FeatureCategory::kBasicOptimization);
    assert(descriptor.features[0].type == qol::contracts::FeatureType::kToggle);
}

void test_negative_level_does_not_insert_color() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_use_explicit_quality_level = true;
    g_explicit_quality_level = -1;

    add_with_context(reinterpret_cast<void*>(0x5000), 6, 51);
    assert(g_query_calls == 1 && g_insert_calls == 0);
}

void test_all_quality_tiers_map_to_inline_colors() {
    constexpr int32_t kLevels[] = {0, 1, 2, 3, 4, 5};
    constexpr uint8_t kColors[] = {'G', 'W', 'C', 'L', 'V', 'Y'};
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_use_explicit_quality_level = true;

    for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); ++i) {
        g_explicit_quality_level = kLevels[i];
        add_with_context(reinterpret_cast<void*>(0x6000), static_cast<int>(i), 61);
        assert(g_last_color == kColors[i]);
        assert(g_insert_calls == static_cast<int>(i + 1));
    }
}

void test_unmatched_resource_id_keeps_plugin_active() {
    qol::features::attribute_range_display::set_active(true);
    assert(qol::features::attribute_range_display::on_resource_ready(
               g_context, "hook:Unrecognized", nullptr) == 0);
    assert(qol::features::attribute_range_display::active());
}

void test_empty_backups_fail_closed_and_clear_callbacks() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    assert(qol::features::attribute_range_display::on_resource_ready(
               g_context, qol::features::attribute_range_display::kResourceIdAddOption,
               nullptr) != 0);
    assert(!qol::features::attribute_range_display::active());
    const int add_calls = g_add_calls;
    FakeBuilder builder{20};
    qol::features::attribute_range_display::uid_desc_add_option_wrapper(&builder, 0, 1, 3);
    assert(g_add_calls == add_calls);

    qol::features::attribute_range_display::set_active(true);
    assert(qol::features::attribute_range_display::on_resource_ready(
               g_context, qol::features::attribute_range_display::kResourceIdMakeItem,
               nullptr) != 0);
    assert(!qol::features::attribute_range_display::active());
    const int make_calls = g_make_calls;
    assert(qol::features::attribute_range_display::uid_desc_make_item_wrapper(
               reinterpret_cast<void*>(0x7000), nullptr, nullptr) == 0);
    assert(g_make_calls == make_calls);
}

}  // namespace

int main() {
    g_sdk = {};
    g_sdk.capabilities.require = require_quality;
    g_sdk.item_handle.register_item = register_item;
    g_ui = {};
    g_ui.size = sizeof(g_ui);
    g_ui.major = qol::contracts::kUiAdapterMajor;
    g_ui.minor = qol::contracts::kUiAdapterMinor;
    g_ui.ui_desc_text_cursor = ui_desc_text_cursor;
    g_ui.insert_inline_color_span = insert_inline_color_span;
    g_sdk.ui = &g_ui;

    g_context.sdk = &g_sdk;
    g_context.game = nullptr;
    qol::features::attribute_range_display::set_context(g_context);
    assert(qol::features::attribute_range_display::on_resource_ready(
               g_context, qol::features::attribute_range_display::kResourceIdAddOption,
               reinterpret_cast<void*>(&original_add)) == 0);
    assert(qol::features::attribute_range_display::on_resource_ready(
               g_context, qol::features::attribute_range_display::kResourceIdMakeItem,
               reinterpret_cast<void*>(&original_make)) == 0);

    test_descriptor_contract();
    test_disabled_wrappers_pass_through();
    test_active_context_uses_provider_level_color();
    test_current_value_disambiguates_repeated_option_id();
    test_cursor_snapshots_bracket_backup_and_unsupported_adapter_fails_closed();
    test_unavailable_or_invalid_quality_fails_closed();
    test_query_and_game_handle_errors_fail_closed();
    test_negative_level_does_not_insert_color();
    test_all_quality_tiers_map_to_inline_colors();
    test_unmatched_resource_id_keeps_plugin_active();
    test_empty_backups_fail_closed_and_clear_callbacks();
    return 0;
}
