#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "contracts/attribute_quality.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"
#include "plugins/attribute_range_display/feature.h"
#include "plugins/attribute_range_display/plugin.h"

namespace {

enum class QueryMode { Success, Error, ItemFailure, InvalidLevel };
enum class ViewMode {
    Native,
    Jewel,
    NativeAndJewel,
    DuplicateNative,
    TwoNative,
    Empty,
    Error,
    Truncated,
    MissingCompleteness,
};

int g_add_calls = 0;
int g_make_calls = 0;
int g_register_calls = 0;
int g_lookup_calls = 0;
int g_query_calls = 0;
int g_query_item_calls = 0;
int g_item_view_calls = 0;
int g_insert_calls = 0;
int g_fail_cursor_call = 0;
bool g_emit_add_from_make = false;
bool g_emit_second_add = false;
bool g_quality_available = true;
bool g_item_facts_available = true;
bool g_use_explicit_quality_level = false;
int32_t g_explicit_quality_level = 0;
int32_t g_register_status = qol::contracts::kQolItemHandleStatusOk;
int32_t g_lookup_status = qol::contracts::kQolItemHandleStatusNotFound;
uint32_t g_lookup_owner = qol::contracts::kQolItemHandleOwnerGame;
qol::contracts::QolItemHandle g_lookup_handle{};
void* g_lookup_item = nullptr;
qol::contracts::QolItemHandle g_last_query_handle{};
QueryMode g_query_mode = QueryMode::Success;
ViewMode g_view_mode = ViewMode::Native;
const qol::contracts::AttributeQualityApi* g_quality_api = nullptr;
qol::contracts::ItemFactsApi g_item_facts_api{};
int g_option_type = 0;
int g_option_id = 0;
int g_display_value = 0;
int g_second_option_id = 0;
int g_second_display_value = 0;
void* g_nested_outer_item = nullptr;
void* g_nested_inner_item = nullptr;
int32_t g_nested_outer_id = 0;
int32_t g_nested_outer_value = 0;
int32_t g_nested_inner_id = 0;
int32_t g_nested_inner_value = 0;
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
    // 本插件不应再调用 provider 的整件 query_item（会重复探测范围）。
    ++g_query_item_calls;
    return qol::contracts::error(kQolErrUnsupported, "query_item must not be used", 26);
}

void append_view_option(qol::contracts::QolItemInventoryView* view, bool native, int32_t id,
                        int32_t value) {
    if (native) {
        view->native_options[view->native_option_count++] = {
            value, id, static_cast<int32_t>(qol::contracts::QolItemInventoryOptionType::kNative)};
    } else {
        view->jewel_options[view->jewel_option_count++] = {
            value, id, static_cast<int32_t>(qol::contracts::QolItemInventoryOptionType::kJewel)};
    }
}

QolResult fake_item_view(qol::contracts::QolItemHandle handle,
                         qol::contracts::QolItemInventoryView* out) {
    ++g_item_view_calls;
    assert(handle.slot != 0 && handle.generation != 0);
    if (g_view_mode == ViewMode::Error)
        return qol::contracts::error(kQolErrNotReady, "item view error", 15);
    if (out == nullptr)
        return qol::contracts::error(kQolErrInvalidArgument, "missing item view", 17);

    *out = {};
    out->equip = 1;
    out->fields = qol::contracts::kQolItemInventoryHasOptionCompleteness |
                  qol::contracts::kQolItemInventoryHasNativeOptions |
                  qol::contracts::kQolItemInventoryHasJewelOptions;
    if (g_view_mode == ViewMode::MissingCompleteness) {
        out->fields &= ~qol::contracts::kQolItemInventoryHasOptionCompleteness;
    }
    if (g_view_mode == ViewMode::Truncated) {
        out->fields |= qol::contracts::kQolItemInventoryOptionsTruncated;
    }
    switch (g_view_mode) {
        case ViewMode::Native:
        case ViewMode::Truncated:
        case ViewMode::MissingCompleteness:
            append_view_option(out, true, g_option_id, g_display_value);
            break;
        case ViewMode::Jewel:
            append_view_option(out, false, g_option_id, g_display_value);
            break;
        case ViewMode::NativeAndJewel:
            append_view_option(out, true, g_option_id, g_display_value);
            append_view_option(out, false, g_option_id, g_display_value);
            break;
        case ViewMode::DuplicateNative:
            append_view_option(out, true, g_option_id, g_display_value);
            append_view_option(out, true, g_option_id, g_display_value);
            break;
        case ViewMode::TwoNative:
            append_view_option(out, true, g_option_id, g_display_value);
            append_view_option(out, true, g_second_option_id, g_second_display_value);
            break;
        case ViewMode::Empty:
        case ViewMode::Error:
            break;
    }
    return qol::contracts::ok();
}

QolResult query_option_context(
    qol::contracts::QolItemHandle handle, int32_t option_id, int32_t display_value,
    const qol::contracts::AttributeQualityOptions* options,
    qol::contracts::AttributeQualityItem* out) {
    ++g_query_calls;
    assert(handle.slot != 0 && handle.generation != 0);
    g_last_query_handle = handle;
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

QolResult require_capability(const char* id, QolVersionRange range, const void** out_iface) {
    if (id == nullptr || out_iface == nullptr)
        return qol::contracts::error(kQolErrInvalidArgument, "invalid capability request", 27);
    if (std::strcmp(id, qol::contracts::kAttributeQualityCapabilityId) == 0) {
        assert(range.major == qol::contracts::kAttributeQualityVersionMajor);
        assert(range.min_minor == qol::contracts::kAttributeQualityVersionMinor);
        if (!g_quality_available || g_quality_api == nullptr)
            return qol::contracts::error(kQolErrNotReady, "quality unavailable", 19);
        *out_iface = g_quality_api;
        return qol::contracts::ok();
    }
    if (std::strcmp(id, qol::contracts::kItemFactsCapabilityId) == 0) {
        assert(range.major == qol::contracts::kItemFactsVersionMajor);
        assert(range.min_minor == qol::contracts::kItemFactsVersionMinor);
        if (!g_item_facts_available)
            return qol::contracts::error(kQolErrNotReady, "item facts unavailable", 22);
        *out_iface = &g_item_facts_api;
        return qol::contracts::ok();
    }
    return qol::contracts::error(kQolErrUnsupported, "unknown capability", 18);
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

int32_t lookup_item(void* item, qol::contracts::QolItemHandle* out, uint32_t* out_owner) {
    ++g_lookup_calls;
    assert(item != nullptr && out != nullptr && out_owner != nullptr);
    if (g_lookup_status != qol::contracts::kQolItemHandleStatusOk) return g_lookup_status;
    assert(item == g_lookup_item);
    *out = g_lookup_handle;
    *out_owner = g_lookup_owner;
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
    FakeBuilder builder{20};
    if (g_nested_outer_item != nullptr && item == g_nested_outer_item) {
        g_view_mode = ViewMode::Native;
        g_option_id = g_nested_outer_id;
        g_display_value = g_nested_outer_value;
        qol::features::attribute_range_display::uid_desc_add_option_wrapper(
            &builder, g_option_type, g_option_id, g_display_value);

        g_view_mode = ViewMode::Jewel;
        g_option_id = g_nested_inner_id;
        g_display_value = g_nested_inner_value;
        qol::features::attribute_range_display::uid_desc_make_item_wrapper(
            g_nested_inner_item, nullptr, nullptr);

        g_view_mode = ViewMode::Native;
        g_option_id = g_nested_outer_id;
        g_display_value = g_nested_outer_value;
        qol::features::attribute_range_display::uid_desc_add_option_wrapper(
            &builder, g_option_type, g_option_id, g_display_value);
        return reinterpret_cast<uintptr_t>(item) + 1;
    }
    if (g_nested_inner_item != nullptr && item == g_nested_inner_item) {
        g_view_mode = ViewMode::Jewel;
        g_option_id = g_nested_inner_id;
        g_display_value = g_nested_inner_value;
        qol::features::attribute_range_display::uid_desc_add_option_wrapper(
            &builder, g_option_type, g_option_id, g_display_value);
        return reinterpret_cast<uintptr_t>(item) + 1;
    }
    if (g_emit_add_from_make) {
        qol::features::attribute_range_display::uid_desc_add_option_wrapper(
            &builder, g_option_type, g_option_id, g_display_value);
        if (g_emit_second_add) {
            qol::features::attribute_range_display::uid_desc_add_option_wrapper(
                &builder, g_option_type, g_second_option_id, g_second_display_value);
        }
    }
    return reinterpret_cast<uintptr_t>(item) + 1;
}

void reset_quality_state() {
    g_quality_available = true;
    g_item_facts_available = true;
    g_use_explicit_quality_level = false;
    g_explicit_quality_level = 0;
    g_quality_api = &g_default_quality_api;
    g_register_status = qol::contracts::kQolItemHandleStatusOk;
    g_lookup_status = qol::contracts::kQolItemHandleStatusNotFound;
    g_lookup_owner = qol::contracts::kQolItemHandleOwnerGame;
    g_lookup_handle = {};
    g_lookup_item = nullptr;
    g_query_mode = QueryMode::Success;
    g_view_mode = ViewMode::Native;
    g_emit_add_from_make = false;
    g_query_calls = 0;
    g_item_view_calls = 0;
    g_second_option_id = 0;
    g_second_display_value = 0;
    g_emit_second_add = false;
    g_nested_outer_item = nullptr;
    g_nested_inner_item = nullptr;
    g_nested_outer_id = 0;
    g_nested_outer_value = 0;
    g_nested_inner_id = 0;
    g_nested_inner_value = 0;
    g_register_calls = 0;
    g_lookup_calls = 0;
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
    assert(g_lookup_calls == 1 && g_register_calls == 1 && g_query_calls == 1 &&
           g_item_view_calls == 1 && g_insert_calls == 1);
    assert(g_cursor_calls == 2);
    assert(g_last_query_option_id == 12 && g_last_query_value == 41);
    assert(g_last_color == static_cast<uint8_t>('C'));
    assert(g_last_before_offset == 20 && g_last_after_offset == 28);
    assert(g_last_builder != nullptr);
}

void test_provider_owned_item_reuses_existing_handle() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    void* item = reinterpret_cast<void*>(0x1800);
    g_lookup_status = qol::contracts::kQolItemHandleStatusOk;
    g_lookup_item = item;
    g_lookup_owner = qol::contracts::kQolItemHandleOwnerProvider;
    g_lookup_handle = {91, 17};

    add_with_context(item, 13, 42);
    assert(g_lookup_calls == 1 && g_register_calls == 0);
    assert(g_query_calls == 1 && g_insert_calls == 1);
    assert(g_last_query_handle.slot == 91 && g_last_query_handle.generation == 17);
}

void test_native_option_is_colored_after_source_check() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    add_with_context(reinterpret_cast<void*>(0x2200), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 1 && g_insert_calls == 1);
}

void test_jewel_option_is_not_colored() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::Jewel;
    add_with_context(reinterpret_cast<void*>(0x2400), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);
}

void test_ambiguous_native_and_jewel_id_value_is_not_colored() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::NativeAndJewel;
    add_with_context(reinterpret_cast<void*>(0x2600), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);
}

void test_duplicate_native_options_are_colored() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::DuplicateNative;
    add_with_context(reinterpret_cast<void*>(0x2700), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 1 && g_insert_calls == 1);
}

void test_view_failure_truncation_or_missing_completeness_is_not_colored() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::Error;
    add_with_context(reinterpret_cast<void*>(0x2800), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);

    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::Truncated;
    add_with_context(reinterpret_cast<void*>(0x2900), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);

    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::MissingCompleteness;
    add_with_context(reinterpret_cast<void*>(0x2a00), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);

    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::Empty;
    add_with_context(reinterpret_cast<void*>(0x2b00), 12, 41);
    assert(g_item_view_calls == 1 && g_query_calls == 0 && g_insert_calls == 0);
}

void test_item_facts_unavailable_or_malformed_is_not_colored() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_item_facts_available = false;
    add_with_context(reinterpret_cast<void*>(0x2c00), 12, 41);
    assert(g_item_view_calls == 0 && g_query_calls == 0 && g_insert_calls == 0);

    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_item_facts_api.struct_size = 1;
    add_with_context(reinterpret_cast<void*>(0x2d00), 12, 41);
    assert(g_item_view_calls == 0 && g_query_calls == 0 && g_insert_calls == 0);
    g_item_facts_api.struct_size = sizeof(g_item_facts_api);

    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    const auto view_fn = g_item_facts_api.view;
    g_item_facts_api.view = nullptr;
    add_with_context(reinterpret_cast<void*>(0x2e00), 12, 41);
    assert(g_item_view_calls == 0 && g_query_calls == 0 && g_insert_calls == 0);
    g_item_facts_api.view = view_fn;
}

void test_view_is_cached_per_make_item_cycle() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    g_view_mode = ViewMode::TwoNative;
    g_second_option_id = 13;
    g_second_display_value = 42;
    g_emit_second_add = true;
    add_with_context(reinterpret_cast<void*>(0x2f00), 12, 41);
    assert(g_item_view_calls == 1);
    assert(g_query_calls == 2 && g_insert_calls == 2);
}

void test_nested_make_item_restores_outer_view_cache() {
    reset_quality_state();
    qol::features::attribute_range_display::set_active(true);
    void* outer = reinterpret_cast<void*>(0x3100);
    void* inner = reinterpret_cast<void*>(0x3200);
    g_nested_outer_item = outer;
    g_nested_inner_item = inner;
    g_nested_outer_id = 20;
    g_nested_outer_value = 50;
    g_nested_inner_id = 21;
    g_nested_inner_value = 60;

    assert(qol::features::attribute_range_display::uid_desc_make_item_wrapper(
               outer, nullptr, nullptr) == reinterpret_cast<uintptr_t>(outer) + 1);
    // 外层首行读取一次 view；内层重新读取一次；外层第二行复用恢复后的缓存。
    assert(g_item_view_calls == 2);
    assert(g_query_calls == 2 && g_insert_calls == 2);
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
    const uint32_t current_ui_size = g_ui.size;
    g_ui.minor = 21;
    g_ui.size = offsetof(qol::contracts::QolUiAdapter, ui_desc_text_cursor);
    const int add_calls_before_old_adapter = g_add_calls;
    add_with_context(item, 12, 41);
    assert(g_add_calls == add_calls_before_old_adapter + 1);
    assert(g_cursor_calls == 0 && g_register_calls == 0 && g_query_calls == 0 &&
           g_insert_calls == 0);
    g_ui.minor = current_minor;
    g_ui.size = current_ui_size;

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
    assert(descriptor.requires_count == 2 && descriptor.requires != nullptr);
    assert(std::strcmp(descriptor.requires[0].id, "attribute.quality") == 0);
    assert(descriptor.requires[0].range.major ==
           qol::contracts::kAttributeQualityVersionMajor);
    assert(descriptor.requires[0].range.min_minor ==
           qol::contracts::kAttributeQualityVersionMinor);
    assert(std::strcmp(descriptor.requires[1].id, "item.facts") == 0);
    assert(descriptor.requires[1].range.major == qol::contracts::kItemFactsVersionMajor);
    assert(descriptor.requires[1].range.min_minor == qol::contracts::kItemFactsVersionMinor);
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
    g_sdk.capabilities.require = require_capability;
    g_sdk.item_handle.register_item = register_item;
    g_sdk.item_handle.lookup_item = lookup_item;
    g_item_facts_api = {};
    g_item_facts_api.struct_size = sizeof(g_item_facts_api);
    g_item_facts_api.view = fake_item_view;
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
    test_provider_owned_item_reuses_existing_handle();
    test_native_option_is_colored_after_source_check();
    test_jewel_option_is_not_colored();
    test_ambiguous_native_and_jewel_id_value_is_not_colored();
    test_duplicate_native_options_are_colored();
    test_view_failure_truncation_or_missing_completeness_is_not_colored();
    test_item_facts_unavailable_or_malformed_is_not_colored();
    test_view_is_cached_per_make_item_cycle();
    test_nested_make_item_restores_outer_view_cache();
    test_current_value_disambiguates_repeated_option_id();
    test_cursor_snapshots_bracket_backup_and_unsupported_adapter_fails_closed();
    test_unavailable_or_invalid_quality_fails_closed();
    test_query_and_game_handle_errors_fail_closed();
    test_negative_level_does_not_insert_color();
    test_all_quality_tiers_map_to_inline_colors();
    test_unmatched_resource_id_keeps_plugin_active();
    test_empty_backups_fail_closed_and_clear_callbacks();
    assert(g_query_item_calls == 0);
    return 0;
}
