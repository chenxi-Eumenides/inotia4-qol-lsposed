#include "plugins/autosell/view.h"

#include <cassert>
#include <cstdint>

#include "contracts/attribute_quality.h"
#include "contracts/game_host.h"
#include "contracts/item_facts.h"
#include "plugins/autosell/plugin.h"

namespace {

qol::contracts::QolItemInventoryView g_facts{};
bool g_facts_available = true;
bool g_is_jewel = false;
bool g_quality_capability_available = true;
const qol::contracts::AttributeQualityApi* g_quality_api = nullptr;
int g_quality_query_calls = 0;
QolResult g_quality_query_result{kQolOk, 0, nullptr, 0};
qol::contracts::AttributeQualityItemResult g_quality_result{};
qol::contracts::QolHostSdk g_sdk{};

QolResult query_item(qol::contracts::QolItemHandle item,
                     const qol::contracts::AttributeQualityOptions* options,
                     qol::contracts::AttributeQualityItemResult* out) {
    ++g_quality_query_calls;
    assert(item.slot == 11 && item.generation == 19);
    assert(options != nullptr && options->allow_outside_range == 0);
    if (g_quality_query_result.ok != kQolOk) return g_quality_query_result;
    if (out == nullptr) return {kQolErrInvalidArgument, kQolErrInvalidArgument, nullptr, 0};
    *out = g_quality_result;
    return qol::contracts::ok();
}

QolResult query_option_context(qol::contracts::QolItemHandle, int32_t, int32_t,
                               const qol::contracts::AttributeQualityOptions*,
                               qol::contracts::AttributeQualityItem*) {
    return qol::contracts::error(kQolErrUnsupported, "unused", 6);
}

QolResult evaluate(int32_t, qol::contracts::AttributeRange,
                   const qol::contracts::AttributeQualityOptions*,
                   qol::contracts::AttributeQuality*) {
    return qol::contracts::error(kQolErrUnsupported, "unused", 6);
}

qol::contracts::AttributeQualityApi g_default_quality_api{
    sizeof(qol::contracts::AttributeQualityApi), query_item, query_option_context, evaluate};

QolResult require_quality(const char* id, QolVersionRange range, const void** out_iface) {
    assert(id != nullptr);
    assert(range.major == qol::contracts::kAttributeQualityVersionMajor);
    if (!g_quality_capability_available || out_iface == nullptr || g_quality_api == nullptr)
        return qol::contracts::error(kQolErrNotReady, "quality unavailable", 19);
    *out_iface = g_quality_api;
    return qol::contracts::ok();
}

}  // namespace

namespace qol::features::autosell {

const contracts::QolHostSdk* host_sdk() { return &g_sdk; }

}  // namespace qol::features::autosell

namespace qol::features::autosell::read_only {

bool item_inventory_view(contracts::QolItemHandle,
                         contracts::QolItemInventoryView* out) {
    if (!g_facts_available || out == nullptr) return false;
    *out = g_facts;
    return true;
}

bool item_is_jewel(int32_t, int32_t* out) {
    if (out == nullptr) return false;
    *out = g_is_jewel ? 1 : 0;
    return true;
}

bool item_is_backpack(int32_t category, int32_t* out) {
    if (out == nullptr) return false;
    *out = category == 60 ? 1 : 0;
    return true;
}

bool item_is_dice(int32_t, int32_t* out) {
    if (out == nullptr) return false;
    *out = 0;
    return true;
}

}  // namespace qol::features::autosell::read_only

namespace {

constexpr uint32_t kRequiredViewFields =
    qol::contracts::kQolItemInventoryHasRarity |
    qol::contracts::kQolItemInventoryHasJewelValue |
    qol::contracts::kQolItemInventoryHasJewelAttributeType;

void prepare_facts(uint32_t fields) {
    g_facts = {};
    g_facts.fields = fields;
    g_facts.rarity = 0;
    g_facts.equip = 0;
    g_facts_available = true;
    g_is_jewel = false;
}

void prepare_jewel_quality() {
    prepare_facts(kRequiredViewFields);
    g_is_jewel = true;
    g_quality_capability_available = true;
    g_quality_api = &g_default_quality_api;
    g_quality_query_calls = 0;
    g_quality_query_result = qol::contracts::ok();
    g_quality_result = {};
    g_quality_result.count = 1;
    g_quality_result.complete = 1;
    g_quality_result.attrs[0].kind = qol::contracts::AttributeKind::Jewel;
    g_quality_result.attrs[0].status = qol::contracts::kQolItemHandleStatusOk;
    g_quality_result.attrs[0].quality.percentile = 73;
    g_sdk = {};
    g_sdk.capabilities.require = require_quality;
}

AutosellViewBuildReason build_jewel_view(autosell::ItemView* out) {
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 28, 1,
        {11, 19}};
    return autosell_build_view(ref, out);
}

void test_jewel_quality_v2_success_uses_provider_percentile() {
    prepare_jewel_quality();
    autosell::ItemView view{};
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 1);
    assert(view.is_jewel);
    assert(view.jewel_percentile == 73);
}

void test_jewel_quality_v2_failures_keep_unknown_sentinel() {
    autosell::ItemView view{};

    prepare_jewel_quality();
    g_quality_capability_available = false;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 0 && view.jewel_percentile == -1);

    prepare_jewel_quality();
    g_quality_query_result = qol::contracts::error(kQolErrNotReady, "query failed", 12);
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 1 && view.jewel_percentile == -1);

    prepare_jewel_quality();
    g_quality_result.attrs[0].status = qol::contracts::kQolItemHandleStatusNotFound;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 1 && view.jewel_percentile == -1);

    prepare_jewel_quality();
    g_quality_result.complete = 0;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 1 && view.jewel_percentile == -1);

    prepare_jewel_quality();
    g_quality_result.count = 2;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 1 && view.jewel_percentile == -1);

    prepare_jewel_quality();
    g_quality_result.attrs[0].kind = qol::contracts::AttributeKind::StoredOption;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 1 && view.jewel_percentile == -1);
}

void test_malformed_quality_api_keeps_unknown_sentinel() {
    autosell::ItemView view{};

    prepare_jewel_quality();
    auto malformed_api = g_default_quality_api;
    malformed_api.struct_size = offsetof(qol::contracts::AttributeQualityApi, evaluate);
    g_quality_api = &malformed_api;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 0 && view.jewel_percentile == -1);

    prepare_jewel_quality();
    malformed_api = g_default_quality_api;
    malformed_api.query_item = nullptr;
    g_quality_api = &malformed_api;
    assert(build_jewel_view(&view) == AutosellViewBuildReason::kBuilt);
    assert(g_quality_query_calls == 0 && view.jewel_percentile == -1);
}

void test_missing_required_field_reason_is_reported() {
    prepare_facts(qol::contracts::kQolItemInventoryHasRarity |
                  qol::contracts::kQolItemInventoryHasJewelValue);
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 60, 1,
        {11, 19}};
    autosell::ItemView view{};
    AutosellViewDiagnostics diagnostics{};
    assert(autosell_build_view(ref, &view, &diagnostics) ==
           AutosellViewBuildReason::kMissingRequiredFields);
    assert(diagnostics.fields == g_facts.fields);
    assert(diagnostics.rarity == 0 && diagnostics.equip == 0);
}

void test_enabled_config_without_matching_rule_is_classified() {
    autosell::ItemView view{};
    autosell::Config config{};
    config.enabled = true;
    assert(autosell::should_sell_with_reason(view, config) ==
           autosell::DecisionReason::kNoRuleMatch);
    assert(!autosell::should_sell(view, config));
}

void test_provider_item_special_rule_match_is_classified() {
    prepare_facts(kRequiredViewFields);
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 60, 1,
        {11, 19}};
    autosell::ItemView view{};
    AutosellViewDiagnostics diagnostics{};
    assert(autosell_build_view(ref, &view, &diagnostics) == AutosellViewBuildReason::kBuilt);
    assert(ref.kind == static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider));
    assert(diagnostics.fields == kRequiredViewFields);
    assert(diagnostics.special_types == autosell::kSpecialBackpack);

    autosell::Config config{};
    config.enabled = true;
    config.special_mask = autosell::kSpecialBackpack;
    assert(autosell::should_sell_with_reason(view, config) ==
           autosell::DecisionReason::kRuleMatch);
}

}  // namespace

int main() {
    test_jewel_quality_v2_success_uses_provider_percentile();
    test_jewel_quality_v2_failures_keep_unknown_sentinel();
    test_malformed_quality_api_keeps_unknown_sentinel();
    test_missing_required_field_reason_is_reported();
    test_enabled_config_without_matching_rule_is_classified();
    test_provider_item_special_rule_match_is_classified();
    return 0;
}
