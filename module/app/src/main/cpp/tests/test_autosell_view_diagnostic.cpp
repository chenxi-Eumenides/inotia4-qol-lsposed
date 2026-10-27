#include "plugins/autosell/view.h"

#include <cassert>
#include <cstdint>

#include "contracts/attribute_quality.h"
#include "contracts/item_facts.h"

namespace {

qol::contracts::QolItemInventoryView g_facts{};
bool g_facts_available = true;
bool g_real_equip = false;
bool g_real_equip_available = true;
bool g_is_jewel = false;
bool g_jewel_predicate_available = true;
int32_t g_current_category = 0;
bool g_backpack_predicate_available = true;
bool g_dice_predicate_available = true;
bool g_quality_capability_available = true;
const qol::contracts::AttributeQualityApi* g_quality_api = nullptr;
int g_quality_query_calls = 0;
QolResult g_quality_query_result{kQolOk, 0, nullptr, 0};
qol::contracts::AttributeQualityItemResult g_quality_result{};

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

const qol::contracts::AttributeQualityApi* quality_provider() {
    return g_quality_capability_available ? g_quality_api : nullptr;
}

}  // namespace

namespace qol::features::autosell::read_only {

bool item_type(contracts::QolItemHandle, contracts::QolItemType* out) {
    if (out == nullptr) return false;
    const bool jewel_category = g_current_category >= 28 && g_current_category <= 32;
    if (g_is_jewel || (jewel_category && !g_jewel_predicate_available)) {
        *out = contracts::QolItemType::kJewel;
        return true;
    }
    if (g_real_equip) {
        if (!g_real_equip_available) return false;
        *out = contracts::QolItemType::kEquipment;
        return true;
    }
    const bool category_classifies_without_equip =
        (g_current_category >= 1 && g_current_category <= 4) ||
        (g_current_category >= 52 && g_current_category <= 56);
    if (!g_real_equip_available && !category_classifies_without_equip && !jewel_category) {
        return false;
    }
    *out = contracts::QolItemType::kOther;
    return true;
}

bool item_inventory_view(contracts::QolItemHandle,
                         contracts::QolItemInventoryView* out) {
    if (!g_facts_available || out == nullptr) return false;
    *out = g_facts;
    return true;
}

bool item_is_backpack(int32_t category, int32_t* out) {
    if (out == nullptr || !g_backpack_predicate_available) return false;
    *out = category >= 1 && category <= 4 ? 1 : 0;
    return true;
}

bool item_is_dice(int32_t category, int32_t* out) {
    if (out == nullptr || !g_dice_predicate_available) return false;
    *out = category >= 52 && category <= 56 ? 1 : 0;
    return true;
}

}  // namespace qol::features::autosell::read_only

namespace {

constexpr uint32_t kFixtureViewFields =
    qol::contracts::kQolItemInventoryHasRarity |
    qol::contracts::kQolItemInventoryHasJewelValue |
    qol::contracts::kQolItemInventoryHasJewelAttributeType;

void prepare_facts(uint32_t fields) {
    g_facts = {};
    g_facts.fields = fields;
    g_facts.rarity = 0;
    g_facts.equip = 0;
    g_facts_available = true;
    g_real_equip = false;
    g_real_equip_available = true;
    g_is_jewel = false;
    g_jewel_predicate_available = true;
    g_backpack_predicate_available = true;
    g_dice_predicate_available = true;
}

void prepare_jewel_quality() {
    prepare_facts(kFixtureViewFields);
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
}

AutosellViewBuildReason build_jewel_view(autosell::ItemView* out) {
    g_current_category = 28;
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 28, 1,
        {11, 19}};
    return autosell_build_view(ref, out, nullptr, &quality_provider);
}

AutosellViewBuildReason build_category_view(int category, autosell::ItemView* out,
                                             AutosellViewDiagnostics* diagnostics = nullptr) {
    g_current_category = category;
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, category, 1,
        {11, 19}};
    return autosell_build_view(ref, out, diagnostics, &quality_provider);
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
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 28, 1,
        {11, 19}};
    g_is_jewel = true;
    autosell::ItemView view{};
    AutosellViewDiagnostics diagnostics{};
    assert(autosell_build_view(ref, &view, &diagnostics) ==
           AutosellViewBuildReason::kMissingRequiredFields);
    assert(diagnostics.fields == g_facts.fields);
    assert(diagnostics.rarity == 0 && diagnostics.equip == 0);
}

void test_non_jewel_without_jewel_fields_builds_view() {
    prepare_facts(qol::contracts::kQolItemInventoryHasRarity);
    autosell::ItemView view{};
    assert(build_category_view(60, &view) == AutosellViewBuildReason::kBuilt);
    assert(!view.is_jewel && !view.is_equip);
}

void test_missing_rarity_builds_view_with_unknown_sentinel() {
    prepare_facts(kFixtureViewFields & ~qol::contracts::kQolItemInventoryHasRarity);
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 60, 1,
        {11, 19}};
    autosell::ItemView view{};
    AutosellViewDiagnostics diagnostics{};
    assert(autosell_build_view(ref, &view, &diagnostics) == AutosellViewBuildReason::kBuilt);
    assert(diagnostics.fields == g_facts.fields);
    assert(view.rarity == -1);
}

void test_real_equip_uses_item_predicate_not_inventory_view_fact() {
    const qol::features::autosell::read_only::InventoryItemRef ref{
        static_cast<int32_t>(qol::contracts::QolInventoryItemKind::kProvider), 6, 2, 60, 1,
        {11, 19}};
    autosell::ItemView view{};

    prepare_facts(kFixtureViewFields);
    g_facts.equip = 1;
    g_real_equip = false;
    assert(autosell_build_view(ref, &view) == AutosellViewBuildReason::kBuilt);
    assert(!view.is_equip);

    prepare_facts(kFixtureViewFields);
    g_facts.equip = 0;
    g_real_equip = true;
    assert(autosell_build_view(ref, &view) == AutosellViewBuildReason::kBuilt);
    assert(view.is_equip);
}

void test_real_equip_failure_only_skips_possible_equipment() {
    autosell::ItemView view{};

    prepare_facts(kFixtureViewFields);
    g_real_equip_available = false;
    g_backpack_predicate_available = false;
    assert(build_category_view(1, &view) == AutosellViewBuildReason::kBuilt);
    assert(view.is_backpack && !view.is_equip);

    prepare_facts(kFixtureViewFields);
    g_real_equip_available = false;
    g_dice_predicate_available = false;
    assert(build_category_view(52, &view) == AutosellViewBuildReason::kBuilt);
    assert(view.dice_tier == 1 && !view.is_equip);

    prepare_facts(kFixtureViewFields);
    g_real_equip_available = false;
    g_is_jewel = true;
    assert(build_category_view(28, &view) == AutosellViewBuildReason::kBuilt);
    assert(view.is_jewel && !view.is_equip);

    prepare_facts(kFixtureViewFields);
    g_real_equip_available = false;
    assert(build_category_view(60, &view) == AutosellViewBuildReason::kFactsUnavailable);
}

void test_jewel_predicate_failure_falls_back_to_category() {
    prepare_facts(kFixtureViewFields);
    g_jewel_predicate_available = false;
    autosell::ItemView view{};
    assert(build_category_view(28, &view) == AutosellViewBuildReason::kBuilt);
    assert(view.is_jewel && view.jewel_tier == 0);

    prepare_facts(kFixtureViewFields);
    g_jewel_predicate_available = false;
    assert(build_category_view(60, &view) == AutosellViewBuildReason::kBuilt);
    assert(!view.is_jewel);
}

void test_enhance_level_is_copied_from_inventory_facts() {
    prepare_facts(kFixtureViewFields);
    g_facts.enchant_level = 7;
    autosell::ItemView view{};
    assert(build_category_view(60, &view) == AutosellViewBuildReason::kBuilt);
    assert(view.enhance_level == 7);
}

void test_enabled_config_without_matching_rule_is_classified() {
    autosell::ItemView view{};
    autosell::Config config{};
    config.enabled = true;
    assert(autosell::should_sell_with_reason(view, config) ==
           autosell::DecisionReason::kNoRuleMatch);
    assert(!autosell::should_sell(view, config));
}

void test_provider_item_backpack_tier_rule_match_is_classified() {
    prepare_facts(kFixtureViewFields);
    autosell::ItemView view{};
    AutosellViewDiagnostics diagnostics{};
    assert(build_category_view(1, &view, &diagnostics) == AutosellViewBuildReason::kBuilt);
    assert(view.category == 1 && view.is_backpack);
    assert(diagnostics.category == 1 && diagnostics.is_backpack);
    assert(diagnostics.fields == kFixtureViewFields);

    autosell::Config config{};
    config.enabled = true;
    config.backpack_tier = 1;
    assert(autosell::should_sell_with_reason(view, config) ==
           autosell::DecisionReason::kRuleMatch);
}

void test_category_tiers_are_exposed_in_view() {
    prepare_facts(kFixtureViewFields);
    autosell::ItemView view{};
    AutosellViewDiagnostics diagnostics{};
    for (int category = 1; category <= 4; ++category) {
        assert(build_category_view(category, &view, &diagnostics) == AutosellViewBuildReason::kBuilt);
        assert(view.category == category && view.is_backpack);
        assert(diagnostics.category == category && diagnostics.is_backpack);
    }

    const struct BadgeCategory { int category; int rank; } badges[] = {
        {42, 1}, {43, 1}, {44, 1}, {45, 1}, {46, 1}, {47, 1},
        {48, 2}, {50, 3}, {49, 4}, {928, 4}, {929, 4}, {930, 4},
        {931, 4}, {932, 4}, {933, 4},
    };
    for (const auto& badge : badges) {
        assert(build_category_view(badge.category, &view, &diagnostics) ==
               AutosellViewBuildReason::kBuilt);
        assert(view.category == badge.category && view.badge_rank == badge.rank);
        assert(diagnostics.category == badge.category && diagnostics.badge_rank == badge.rank);
    }

    for (int category = 52; category <= 56; ++category) {
        assert(build_category_view(category, &view, &diagnostics) == AutosellViewBuildReason::kBuilt);
        assert(view.category == category && view.dice_tier == category - 51);
        assert(diagnostics.category == category && diagnostics.dice_tier == category - 51);
    }
}

}  // namespace

int main() {
    test_jewel_quality_v2_success_uses_provider_percentile();
    test_jewel_quality_v2_failures_keep_unknown_sentinel();
    test_malformed_quality_api_keeps_unknown_sentinel();
    test_missing_required_field_reason_is_reported();
    test_non_jewel_without_jewel_fields_builds_view();
    test_missing_rarity_builds_view_with_unknown_sentinel();
    test_real_equip_uses_item_predicate_not_inventory_view_fact();
    test_real_equip_failure_only_skips_possible_equipment();
    test_jewel_predicate_failure_falls_back_to_category();
    test_enhance_level_is_copied_from_inventory_facts();
    test_enabled_config_without_matching_rule_is_classified();
    test_provider_item_backpack_tier_rule_match_is_classified();
    test_category_tiers_are_exposed_in_view();
    return 0;
}
