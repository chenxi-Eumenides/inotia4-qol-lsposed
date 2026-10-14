#include <cassert>
#include <cstddef>
#include <cstdint>

#include "contracts/attribute_quality.h"
#include "contracts/game_host.h"
#include "contracts/ui.h"
#include "plugins/attribute_range_display/feature.h"

namespace {

enum class QueryMode { Success, Error, ItemFailure, InvalidLevel };

int g_add_calls = 0;
int g_make_calls = 0;
int g_register_calls = 0;
int g_query_calls = 0;
int g_insert_calls = 0;
bool g_emit_add_from_make = false;
bool g_quality_available = true;
int32_t g_register_status = qol::contracts::kQolItemHandleStatusOk;
QueryMode g_query_mode = QueryMode::Success;
const qol::contracts::AttributeQualityApi* g_quality_api = nullptr;
int g_option_type = 0;
int g_option_id = 0;
int g_display_value = 0;
int g_last_query_option_id = -1;
int g_last_query_value = -1;
uint8_t g_last_color = 0;
uint32_t g_last_max_text_bytes = 0;
void* g_last_builder = nullptr;

qol::contracts::QolHostSdk g_sdk{};
qol::contracts::QolGameHost g_game{};
qol::contracts::QolUiAdapter g_ui{};

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

QolResult insert_inline_color(void* builder, uint32_t max_text_bytes, uint8_t color) {
    ++g_insert_calls;
    g_last_builder = builder;
    g_last_max_text_bytes = max_text_bytes;
    g_last_color = color;
    return qol::contracts::ok();
}

void original_add(void*, int, int, int) { ++g_add_calls; }

uintptr_t original_make(void* item, void*, void*) {
    ++g_make_calls;
    if (g_emit_add_from_make) {
        char builder{};
        qol::features::attribute_range_display::uid_desc_add_option_wrapper(
            &builder, g_option_type, g_option_id, g_display_value);
    }
    return reinterpret_cast<uintptr_t>(item) + 1;
}

void reset_quality_state() {
    g_quality_available = true;
    g_quality_api = &g_default_quality_api;
    g_register_status = qol::contracts::kQolItemHandleStatusOk;
    g_query_mode = QueryMode::Success;
    g_query_calls = 0;
    g_register_calls = 0;
    g_insert_calls = 0;
    g_last_query_option_id = -1;
    g_last_query_value = -1;
    g_last_color = 0;
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

    char builder{};
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
    assert(g_last_query_option_id == 12 && g_last_query_value == 41);
    assert(g_last_color == static_cast<uint8_t>('C'));
    assert(g_last_max_text_bytes == 4096);
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

}  // namespace

int main() {
    g_sdk = {};
    g_sdk.capabilities.require = require_quality;
    g_sdk.item_handle.register_item = register_item;
    g_ui = {};
    g_ui.insert_inline_color = insert_inline_color;
    g_sdk.ui = &g_ui;

    qol::contracts::PluginContext context{};
    context.sdk = &g_sdk;
    context.game = &g_game;
    qol::features::attribute_range_display::set_context(context);
    qol::features::attribute_range_display::on_resource_ready(
        context, qol::features::attribute_range_display::kResourceIdAddOption,
        reinterpret_cast<void*>(&original_add));
    qol::features::attribute_range_display::on_resource_ready(
        context, qol::features::attribute_range_display::kResourceIdMakeItem,
        reinterpret_cast<void*>(&original_make));

    test_disabled_wrappers_pass_through();
    test_active_context_uses_provider_level_color();
    test_current_value_disambiguates_repeated_option_id();
    test_unavailable_or_invalid_quality_fails_closed();
    test_query_and_game_handle_errors_fail_closed();
    return 0;
}
