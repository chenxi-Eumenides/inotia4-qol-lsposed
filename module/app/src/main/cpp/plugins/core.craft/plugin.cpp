#include "plugin.h"

#include "contracts/craft_facts.h"
#include "contracts/game_ports/craft_port.h"

namespace qol::features::core_craft {
namespace {

constexpr const char* kPluginId = "core.craft";
constexpr const char* kPluginVersion = "0.1.0";
[[gnu::used]] constexpr char kCraftFactsBuildMarker[] = "craft.facts:v1";

const contracts::QolCraftPort* g_port = nullptr;

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "craft facts not ready", 21);
}

#define QOL_CRAFT_FACT_READ(name, signature, call) \
    QolResult name signature { \
        if (g_port == nullptr || g_port->read.name == nullptr) return not_ready(); \
        return g_port->read.name call; \
    }

QOL_CRAFT_FACT_READ(ui_type, (uint32_t* out), (out))
QOL_CRAFT_FACT_READ(panel_state, (int32_t* out), (out))
QOL_CRAFT_FACT_READ(mix_type, (uint32_t* out), (out))
QOL_CRAFT_FACT_READ(selected_stuff_index, (int64_t* out), (out))
QOL_CRAFT_FACT_READ(selected_stuff_item, (contracts::QolItemHandle* out), (out))
QOL_CRAFT_FACT_READ(stuff_item, (uint32_t index, contracts::QolItemHandle* out), (index, out))
QOL_CRAFT_FACT_READ(category_held_count, (int32_t category, int32_t* out), (category, out))
QOL_CRAFT_FACT_READ(character_level, (int32_t* out), (out))

#undef QOL_CRAFT_FACT_READ

QolResult read_only_ui_type(uint32_t* out) {
    if (g_port == nullptr || g_port->read_only.ui_type == nullptr) return not_ready();
    return g_port->read_only.ui_type(out);
}

QolResult read_only_get_ui_type(uint32_t* out) {
    if (g_port == nullptr || g_port->read_only.get_ui_type == nullptr) return not_ready();
    return g_port->read_only.get_ui_type(out);
}

QolResult read_only_panel_state(int32_t* out) {
    if (g_port == nullptr || g_port->read_only.panel_state == nullptr) return not_ready();
    return g_port->read_only.panel_state(out);
}

QolResult read_only_mix_type(uint32_t* out) {
    if (g_port == nullptr || g_port->read_only.mix_type == nullptr) return not_ready();
    return g_port->read_only.mix_type(out);
}

QolResult read_only_selected_stuff_index(int64_t* out) {
    if (g_port == nullptr || g_port->read_only.selected_stuff_index == nullptr) return not_ready();
    return g_port->read_only.selected_stuff_index(out);
}

QolResult read_only_selected_stuff_item(contracts::QolItemHandle* out) {
    if (g_port == nullptr || g_port->read_only.selected_stuff_item == nullptr) return not_ready();
    return g_port->read_only.selected_stuff_item(out);
}

QolResult read_only_stuff_item(uint32_t index, contracts::QolItemHandle* out) {
    if (g_port == nullptr || g_port->read_only.stuff_item == nullptr) return not_ready();
    return g_port->read_only.stuff_item(index, out);
}

QolResult read_only_category_held_count(int32_t category, int32_t* out) {
    if (g_port == nullptr || g_port->read_only.category_held_count == nullptr) return not_ready();
    return g_port->read_only.category_held_count(category, out);
}

QolResult read_only_character_level(int32_t* out) {
    if (g_port == nullptr || g_port->read_only.character_level == nullptr) return not_ready();
    return g_port->read_only.character_level(out);
}

QolResult read_only_stuff_slot_available(uint32_t index, int32_t* out_available) {
    if (g_port == nullptr || g_port->read_only.stuff_slot_available == nullptr) return not_ready();
    return g_port->read_only.stuff_slot_available(index, out_available);
}

QolResult read_only_target_slot_item(contracts::QolItemHandle* out) {
    if (g_port == nullptr || g_port->read_only.target_slot_item == nullptr) return not_ready();
    return g_port->read_only.target_slot_item(out);
}

QolResult read_only_selected_inventory_item(contracts::QolItemHandle* out) {
    if (g_port == nullptr || g_port->read_only.selected_inventory_item == nullptr) return not_ready();
    return g_port->read_only.selected_inventory_item(out);
}

QolResult read_only_selected_recipe(int32_t type, int64_t* out) {
    if (g_port == nullptr || g_port->read_only.selected_recipe == nullptr) return not_ready();
    return g_port->read_only.selected_recipe(type, out);
}

QolResult read_only_stuff_slot_count(uint32_t* out) {
    if (g_port == nullptr || g_port->read_only.stuff_slot_count == nullptr) return not_ready();
    return g_port->read_only.stuff_slot_count(out);
}

const contracts::CraftFactsApi kCraftFactsApi{
    {&ui_type, &panel_state, &mix_type, &selected_stuff_index, &selected_stuff_item,
     &stuff_item, &category_held_count, &character_level},
    {&read_only_ui_type, &read_only_get_ui_type, &read_only_panel_state, &read_only_mix_type,
     &read_only_selected_stuff_index, &read_only_selected_stuff_item, &read_only_stuff_item,
      &read_only_category_held_count, &read_only_character_level, &read_only_stuff_slot_available,
     &read_only_target_slot_item, &read_only_selected_inventory_item, &read_only_selected_recipe,
     &read_only_stuff_slot_count},
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kCraftFactsCapabilityId, contracts::kCraftFactsVersion, &kCraftFactsApi},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CRAFT,
     {contracts::kQolPortCraftVersion.major, contracts::kQolPortCraftVersion.minor}},
};

void log_message(const contracts::QolHostSdk* sdk, const char* message) {
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)sdk->log.write(1, 1, kPluginId, message, length);
}

int plugin_install(contracts::PluginContext& ctx) {
    g_port = nullptr;
    if (ctx.struct_size < contracts::PLUGIN_CONTEXT_PORTS_STRUCT_SIZE ||
        ctx.sdk_major != contracts::kHostSdkMajor || ctx.sdk == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        return 1;
    }
    g_port = static_cast<const contracts::QolCraftPort*>(ctx.ports[0]);
    if (!contracts::satisfies(g_port->abi_version,
                              {contracts::kQolPortCraftVersion.major,
                               contracts::kQolPortCraftVersion.minor}) ||
        g_port->read.ui_type == nullptr || g_port->read.stuff_item == nullptr ||
        g_port->read_only.panel_state == nullptr ||
        g_port->read_only.stuff_slot_available == nullptr) {
        g_port = nullptr;
        return 1;
    }
    log_message(ctx.sdk, "[core.craft] provider install");
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[core.craft] provider deactivate");
    g_port = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[core.craft] provider enabled: capability published");
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    log_message(ctx.sdk, "[core.craft] provider disabled: capability withdrawn");
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_port != nullptr;
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.craft.provider", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "合成领域服务", "提供合成界面与配方的只读事实。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId, kPluginVersion,
    0, nullptr,
    0, nullptr,
    1, kProvides,
    0, nullptr,
    0, nullptr,
    &plugin_install, &plugin_deactivate, &plugin_on_enabled, &plugin_on_disabled,
    &plugin_fill_status, nullptr,
    kPorts, 1,
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::core_craft
