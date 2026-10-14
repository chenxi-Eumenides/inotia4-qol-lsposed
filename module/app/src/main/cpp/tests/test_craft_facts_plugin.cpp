#include "contracts/craft_facts.h"
#include "contracts/craft_commands.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/craft_port.h"
#include "contracts/game_ports/craft_ui_write_port.h"
#include "contracts/game_ports/custom_recipe_write_port.h"
#include "plugins/core.craft/plugin.h"
#include "plugins/custom_recipe/result.h"

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace {

QolResult fake_craft_ui_type(uint32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = 0;
    return qol::contracts::ok();
}

QolResult fake_craft_panel_state(int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = 0;
    return qol::contracts::ok();
}

QolResult fake_craft_stuff_item(uint32_t, qol::contracts::QolItemHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = qol::contracts::kInvalidItemHandle;
    return qol::contracts::ok();
}

QolResult fake_craft_slot_available(uint32_t, int32_t* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "bad", 3);
    *out = 1;
    return qol::contracts::ok();
}

using CraftItemOutFn = QolResult (*)(qol::contracts::QolItemHandle*);
using CraftIndexedItemOutFn = QolResult (*)(uint32_t, qol::contracts::QolItemHandle*);
using CraftSlotAvailabilityFn = QolResult (*)(uint32_t, int32_t*);
using CraftTargetGateFn =
    qol::features::custom_recipe::result::CraftGate (*)(qol::contracts::QolItemHandle);
using CraftJewelPredicateFn = bool (*)(qol::contracts::QolItemHandle);
using CraftSetStuffFn = QolResult (*)(uint32_t, qol::contracts::QolItemHandle);
using CraftTargetWriteFn = QolResult (*)(qol::contracts::QolItemHandle);
using CraftReadJewelWordFn = QolResult (*)(qol::contracts::QolItemHandle, uint32_t*);
using CraftPortRandomFn = QolResult (*)(int32_t, int32_t, int32_t*);

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
static_assert(std::is_same<decltype(qol::features::custom_recipe::result::CraftPlaceInput::item),
                           qol::contracts::QolItemHandle>::value);
static_assert(std::is_same<decltype(qol::features::custom_recipe::result::CraftExecuteInput::items),
                           qol::contracts::QolItemHandle[3]>::value);
static_assert(std::is_same<decltype(qol::features::custom_recipe::result::CraftMakeItemInput::item),
                           qol::contracts::QolItemHandle>::value);
static_assert(std::is_same<
                  decltype(qol::features::custom_recipe::result::CraftMakeItemInput::rand_inclusive),
                  CraftPortRandomFn>::value,
              "typed random source must propagate QolResult (no silent clamp)");
static_assert(qol::contracts::kCraftFactsVersionMajor == 1 &&
                  qol::contracts::kCraftFactsVersionMinor == 0,
              "craft.facts remains v1.0");
// 结果枚举已内部化到 custom_recipe::result；失败阶段追加，既有值保持稳定。
using ExplodeResult = qol::features::custom_recipe::result::CraftExecuteResult;
using ExplodeOutcome = qol::features::custom_recipe::result::PlaceOutcome;
static_assert(static_cast<uint8_t>(ExplodeOutcome::kNotMine) == 0 &&
                  static_cast<uint8_t>(ExplodeOutcome::kPlaced) == 1 &&
                  static_cast<uint8_t>(ExplodeOutcome::kAlreadyPlaced) == 2 &&
                  static_cast<uint8_t>(ExplodeOutcome::kNotEnoughHeld) == 3 &&
                  static_cast<uint8_t>(ExplodeOutcome::kWriteFailed) == 4,
              "PlaceOutcome appends write failure without changing existing values");
static_assert(static_cast<uint8_t>(ExplodeResult::kOk) == 0 &&
                  static_cast<uint8_t>(ExplodeResult::kStaleSlots) == 1 &&
                  static_cast<uint8_t>(ExplodeResult::kNotEnoughHeld) == 2 &&
                  static_cast<uint8_t>(ExplodeResult::kNoBagSpace) == 3 &&
                  static_cast<uint8_t>(ExplodeResult::kAborted) == 4 &&
                  static_cast<uint8_t>(ExplodeResult::kPrecheckFailed) == 5 &&
                  static_cast<uint8_t>(ExplodeResult::kProductPrepareFailed) == 6 &&
                  static_cast<uint8_t>(ExplodeResult::kMaterialRemoveFailed) == 7 &&
                  static_cast<uint8_t>(ExplodeResult::kMaterialObservationFailed) == 8 &&
                  static_cast<uint8_t>(ExplodeResult::kProductSaveFailed) == 9,
              "CraftExecuteResult appends staged failure results without changing existing values");
static_assert(static_cast<uint8_t>(qol::features::custom_recipe::result::CraftGate::kPass) == 0 &&
                  static_cast<uint8_t>(qol::features::custom_recipe::result::CraftGate::kNotJewel) == 1 &&
                  static_cast<uint8_t>(qol::features::custom_recipe::result::CraftGate::kAlreadyGold) == 2 &&
                  static_cast<uint8_t>(qol::features::custom_recipe::result::CraftGate::kUnavailable) == 3,
              "CraftGate appends quality-unavailable without changing existing values");
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
                  qol::contracts::kQolPortCustomRecipeWriteVersion.minor == 2,
              "custom recipe write port is v1.2 (typed random tail)");
static_assert(offsetof(qol::contracts::CraftFactsApi, read_only) ==
                  sizeof(qol::contracts::QolCraftReadApi),
              "CraftFactsApi layout changed");
static_assert(std::is_standard_layout<qol::features::custom_recipe::result::CraftResultApi>::value &&
                  std::is_trivially_copyable<qol::features::custom_recipe::result::CraftResultApi>::value);
static_assert(std::is_same<decltype(qol::features::custom_recipe::result::CraftResultApi::craft_gate),
                           CraftTargetGateFn>::value);
static_assert(std::is_same<decltype(qol::features::custom_recipe::result::CraftResultApi::is_jewel_item),
                           CraftJewelPredicateFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::set_stuff_item),
                           CraftSetStuffFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::read_jewel_word),
                           CraftReadJewelWordFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::free_item),
                           CraftTargetWriteFn>::value);
static_assert(std::is_same<decltype(qol::contracts::QolCustomRecipeWritePort::random_inclusive),
                           CraftPortRandomFn>::value);
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
    CHECK(descriptor.requires_count == 0 && descriptor.optional_count == 1);
    CHECK(descriptor.optional_requires != nullptr);
    CHECK(std::strcmp(descriptor.optional_requires[0].id,
                      qol::contracts::kHttpRegistryCapabilityId) == 0);
    CHECK(descriptor.optional_requires[0].range.major == 1 &&
          descriptor.optional_requires[0].range.min_minor == 1);
    CHECK(descriptor.http_query_count == 0 && descriptor.http_queries == nullptr);
    CHECK(descriptor.http_endpoint_count == 0 && descriptor.http_endpoints == nullptr);
    CHECK(descriptor.http_route_metadata_count == 1 &&
          descriptor.http_route_metadata != nullptr);
    CHECK(descriptor.http_route_metadata[0].method == qol::contracts::HttpMethod::kPost);
    CHECK(std::strcmp(descriptor.http_route_metadata[0].path,
                      "/api/op/craft/mix-direct") == 0);
    CHECK(std::strcmp(descriptor.http_route_metadata[0].route_owner, "core.craft") == 0);
    CHECK(descriptor.http_route_metadata[0].op_required == 1 &&
          descriptor.http_route_metadata[0].lease_required == 1);
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

    qol::contracts::QolCraftReadApi craft_read{};
    craft_read.ui_type = &fake_craft_ui_type;
    craft_read.stuff_item = &fake_craft_stuff_item;
    qol::contracts::QolCraftReadOnlyApi craft_read_only{};
    craft_read_only.panel_state = &fake_craft_panel_state;
    craft_read_only.stuff_slot_available = &fake_craft_slot_available;
    const qol::contracts::QolCraftPort craft_port{qol::contracts::kQolPortCraftVersion,
                                                  craft_read, craft_read_only};
    const void* ports[] = {&craft_port};
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), qol::contracts::kHostSdkMajor, qol::contracts::kHostSdkMinor};
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk_minor = qol::contracts::kHostSdkMinor;
    context.sdk = &sdk;
    context.plugin_id = "core.craft";
    context.ports = ports;
    context.port_count = 1;
    CHECK(descriptor.install(context) == 0);
    descriptor.deactivate(context);
    CHECK(g_fail == 0);
    std::printf("craft_facts_plugin: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
