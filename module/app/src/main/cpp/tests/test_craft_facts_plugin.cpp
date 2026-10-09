#include "contracts/craft_facts.h"
#include "contracts/craft_result.h"
#include "contracts/craft_commands.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/craft_port.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "plugins/core.craft/plugin.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace {

using CraftItemOutFn = QolResult (*)(qol::contracts::QolItemHandle*);
using CraftIndexedItemOutFn = QolResult (*)(uint32_t, qol::contracts::QolItemHandle*);
using CraftSlotAvailabilityFn = QolResult (*)(uint32_t, int32_t*);
using CraftTargetGateFn = qol::contracts::CraftGate (*)(qol::contracts::QolItemHandle);
using CraftJewelPredicateFn = bool (*)(qol::contracts::QolItemHandle);
using CraftSetStuffFn = QolResult (*)(uint32_t, qol::contracts::QolItemHandle);
using CraftTargetWriteFn = QolResult (*)(qol::contracts::QolItemHandle);
using CraftReadJewelWordFn = QolResult (*)(qol::contracts::QolItemHandle, uint32_t*);

static_assert(std::is_same<decltype(qol::contracts::QolCraftReadApi::selected_stuff_item),
                           CraftItemOutFn>::value,
              "craft selected item read must return a typed item handle");
static_assert(std::is_same<decltype(qol::contracts::QolCraftReadApi::stuff_item),
                           CraftIndexedItemOutFn>::value,
              "craft material reads must return typed item handles");
static_assert(std::is_same<decltype(qol::contracts::QolCraftReadOnlyApi::stuff_slot_available),
                           CraftSlotAvailabilityFn>::value,
              "craft controls cross the facts API only as slot availability");
static_assert(std::is_same<decltype(qol::contracts::QolCraftReadOnlyApi::target_slot_item),
                           CraftItemOutFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCraftReadOnlyApi::selected_inventory_item),
                           CraftItemOutFn>::value);
static_assert(std::is_same<decltype(qol::contracts::CraftPlaceInput::item),
                           qol::contracts::QolItemHandle>::value);
static_assert(std::is_same<decltype(qol::contracts::CraftExecuteInput::items),
                           qol::contracts::QolItemHandle[3]>::value);
static_assert(std::is_same<decltype(qol::contracts::CraftMakeItemInput::item),
                           qol::contracts::QolItemHandle>::value);
static_assert(qol::contracts::kCraftFactsVersionMajor == 1 &&
                  qol::contracts::kCraftFactsVersionMinor == 0,
              "craft.facts remains v1.0");
static_assert(qol::contracts::kCraftResultVersionMajor == 1 &&
                  qol::contracts::kCraftResultVersionMinor == 1,
              "typed CraftResultApi parameters are v1.1");
static_assert(std::is_standard_layout<qol::contracts::CraftFactsApi>::value &&
                  std::is_trivially_copyable<qol::contracts::CraftFactsApi>::value);
static_assert(std::is_standard_layout<qol::contracts::QolCraftPort>::value &&
                  std::is_trivially_copyable<qol::contracts::QolCraftPort>::value);
static_assert(std::is_standard_layout<qol::contracts::QolCraftUiWritePort>::value &&
                  std::is_trivially_copyable<qol::contracts::QolCraftUiWritePort>::value);
static_assert(std::is_standard_layout<qol::contracts::QolCustomRecipeWritePort>::value &&
                  std::is_trivially_copyable<qol::contracts::QolCustomRecipeWritePort>::value);
static_assert(qol::contracts::kQolPortCraftVersion.major == 1 &&
                  qol::contracts::kQolPortCraftVersion.minor == 1 &&
                  qol::contracts::kQolPortCraftUiWriteVersion.minor == 1 &&
                  qol::contracts::kQolPortCustomRecipeWriteVersion.minor == 1);
static_assert(offsetof(qol::contracts::CraftFactsApi, read_only) ==
                  sizeof(qol::contracts::QolCraftReadApi),
              "CraftFactsApi layout changed");
static_assert(std::is_standard_layout<qol::contracts::CraftResultApi>::value &&
                  std::is_trivially_copyable<qol::contracts::CraftResultApi>::value);
static_assert(std::is_same<decltype(qol::contracts::CraftResultApi::craft_gate),
                           CraftTargetGateFn>::value);
static_assert(std::is_same<decltype(qol::contracts::CraftResultApi::is_jewel_item),
                           CraftJewelPredicateFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::set_stuff_item),
                           CraftSetStuffFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::read_jewel_word),
                           CraftReadJewelWordFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::free_item),
                           CraftTargetWriteFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCraftUiWritePort::set_target_slot_item),
                           CraftTargetWriteFn>::value);
static_assert(std::is_same<decltype(qol::contracts::CraftSetStuffItemPayload::item),
                           qol::contracts::QolItemHandle>::value);
static_assert(std::is_trivially_copyable<qol::contracts::CraftSetStuffItemPayload>::value);
static_assert(offsetof(qol::contracts::QolCustomRecipeWritePort, set_stuff_item) ==
                  offsetof(qol::contracts::QolCustomRecipeWritePort, write_stuff_need) +
                      sizeof(qol::contracts::QolCustomRecipeWritePort::write_stuff_need),
              "custom recipe port must preserve typed item identity at its first operation");

}  // namespace

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

int main() {
    const auto& descriptor = qol::features::core_craft::plugin_descriptor();
    CHECK(std::strcmp(descriptor.id, "core.craft") == 0);
    CHECK(std::strcmp(descriptor.version, "0.1.0") == 0);
    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 0);
    CHECK(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    CHECK(std::strcmp(descriptor.provides[0].id, qol::contracts::kCraftFactsCapabilityId) == 0);
    CHECK(descriptor.provides[0].version.major == 1 && descriptor.provides[0].version.minor == 0);
    CHECK(descriptor.provides[0].iface != nullptr);
    CHECK(descriptor.port_count == 1 && descriptor.ports != nullptr);
    CHECK(descriptor.ports[0].id == qol::contracts::QolPortId::QOL_PORT_CRAFT);

    const auto* api = static_cast<const qol::contracts::CraftFactsApi*>(descriptor.provides[0].iface);
    CHECK(api->read.ui_type != nullptr);
    CHECK(api->read.panel_state != nullptr);
    CHECK(api->read.mix_type != nullptr);
    CHECK(api->read.selected_stuff_index != nullptr);
    CHECK(api->read.selected_stuff_item != nullptr);
    CHECK(api->read.stuff_item != nullptr);
    CHECK(api->read.category_held_count != nullptr);
    CHECK(api->read.character_level != nullptr);
    CHECK(api->read_only.ui_type != nullptr);
    CHECK(api->read_only.get_ui_type != nullptr);
    CHECK(api->read_only.panel_state != nullptr);
    CHECK(api->read_only.mix_type != nullptr);
    CHECK(api->read_only.selected_stuff_index != nullptr);
    CHECK(api->read_only.selected_stuff_item != nullptr);
    CHECK(api->read_only.stuff_item != nullptr);
    CHECK(api->read_only.category_held_count != nullptr);
    CHECK(api->read_only.character_level != nullptr);
    CHECK(api->read_only.stuff_slot_available != nullptr);
    CHECK(api->read_only.target_slot_item != nullptr);
    CHECK(api->read_only.selected_inventory_item != nullptr);
    CHECK(api->read_only.selected_recipe != nullptr);
    CHECK(api->read_only.stuff_slot_count != nullptr);
    CHECK(g_fail == 0);
    std::printf("craft_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
