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

}  // namespace

namespace qol::features::autosell {

const contracts::QolHostSdk* host_sdk() { return nullptr; }

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
    *out = 0;
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
    test_missing_required_field_reason_is_reported();
    test_enabled_config_without_matching_rule_is_classified();
    test_provider_item_special_rule_match_is_classified();
    return 0;
}
