#include "plugins/move_merge/plugin.h"

#include <atomic>
#include <cstdint>

#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/move_merge_port.h"
#include "contracts/host_sdk.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/move_merge.h"
#include "contracts/ui.h"

namespace qol::features::move_merge {
namespace {

constexpr const char* kPluginId = "move_merge";
constexpr const char* kX1Resource = "ptrslot:MOVE_MERGE_GOT";
constexpr const char* kReason = "Owns the UIEquip item-proc wrapper and native merge gate";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolMoveMergePort* g_move_port = nullptr;
const contracts::QolCharacterExtrasPort* g_character_port = nullptr;
std::atomic<bool> g_active{false};

uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param);

const contracts::ResourceClaim kResources[] = {{
    kX1Resource,
    kX1Resource,
    contracts::ResourceMechanism::kPtrSlot,
    contracts::ResourcePolicy::kPrivate,
    contracts::ResourceRequirement::kRequired,
    contracts::ResourceFailure::kFailClosed,
    true,
    kPluginId,
    kReason,
    nullptr,
    120,
    reinterpret_cast<void*>(&ui_equip_inven_item_proc_wrapper),
}};

const contracts::CapabilityRequirement kRequires[] = {{
    contracts::kItemFactsCapabilityId,
    {contracts::kItemFactsVersionMajor, 2},
}};
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kInventoryMergeProjectionCapabilityId,
     {contracts::kInventoryMergeProjectionVersionMajor,
      contracts::kInventoryMergeProjectionVersionMinor}},
    {contracts::kInventoryRefreshCapabilityId,
     {contracts::kInventoryRefreshVersionMajor, contracts::kInventoryRefreshVersionMinor}},
};
const contracts::QolPortRequirement kPorts[] = {{
    contracts::QolPortId::QOL_PORT_MOVE_MERGE,
    {contracts::kQolMoveMergePortVersion.major, contracts::kQolMoveMergePortVersion.minor},
}, {
    contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
    {contracts::kQolPortCharacterExtrasVersion.major,
     contracts::kQolPortCharacterExtrasVersion.minor},
}};

// feature 为插件唯一开关：want_run 成立才会 install；wrapper 只在插件活跃时安装，
// 故合并门禁等价于插件活跃态。
bool gate_enabled() {
    return g_active.load(std::memory_order_acquire);
}

const contracts::InventoryMergeProjectionApi* projection_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const auto result = g_sdk->capabilities.require(
        contracts::kInventoryMergeProjectionCapabilityId,
        {contracts::kInventoryMergeProjectionVersionMajor,
         contracts::kInventoryMergeProjectionVersionMinor},
        &iface);
    const auto* api = contracts::succeeded(result)
                          ? static_cast<const contracts::InventoryMergeProjectionApi*>(iface)
                          : nullptr;
    return contracts::inventory_merge_projection_api_valid(api) ? api : nullptr;
}

const contracts::InventoryRefreshApi* refresh_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const auto result = g_sdk->capabilities.require(
        contracts::kInventoryRefreshCapabilityId,
        {contracts::kInventoryRefreshVersionMajor, contracts::kInventoryRefreshVersionMinor},
        &iface);
    const auto* api = contracts::succeeded(result)
                          ? static_cast<const contracts::InventoryRefreshApi*>(iface)
                          : nullptr;
    return api != nullptr && api->call_original != nullptr ? api : nullptr;
}

const contracts::ItemFactsApi* item_facts_api() {
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const auto result = g_sdk->capabilities.require(
        contracts::kItemFactsCapabilityId,
        {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface);
    const auto* api = contracts::succeeded(result)
                          ? static_cast<const contracts::ItemFactsApi*>(iface)
                          : nullptr;
    return contracts::item_facts_api_size_valid(api) ? api : nullptr;
}

void projection_observe_post(const contracts::InventoryMergeProjectionApi* api,
                             uint64_t token, void* control, uint64_t event, void* x2,
                             void* param, uint64_t result) {
    const auto* current = projection_api();
    if (current != nullptr && current->observe_item_proc_post != nullptr) {
        current->observe_item_proc_post(token, control, event, x2, param, result);
    }
    (void)api;
}

uint64_t call_original(void* control, uint64_t event, void* x2, void* param) {
    uint64_t result = 0;
    if (g_sdk == nullptr || g_sdk->hooks.call_original_control_proc == nullptr ||
        !contracts::succeeded(g_sdk->hooks.call_original_control_proc(
            control, event, x2, param, &result))) return 0;
    return result;
}

bool control_slot(void* control, int32_t* out_slot) {
    return g_sdk != nullptr && g_sdk->ui != nullptr && g_sdk->ui->control_item_slot != nullptr &&
           contracts::succeeded(g_sdk->ui->control_item_slot(control, out_slot));
}

bool control_identity(void* control, contracts::QolItemHandle* out_item) {
    return control != nullptr && g_sdk != nullptr && g_sdk->ui != nullptr &&
           g_sdk->ui->control_item_identity != nullptr && out_item != nullptr &&
           contracts::succeeded(g_sdk->ui->control_item_identity(control, out_item));
}

bool native_item(contracts::QolItemHandle item) {
    return g_move_port != nullptr && g_move_port->validate_native_item != nullptr &&
           contracts::succeeded(g_move_port->validate_native_item(item));
}

bool extension_item(const contracts::InventoryMergeProjectionApi* projection,
                    contracts::QolItemHandle item) {
    if (projection == nullptr || projection->identify_extension_item == nullptr) return false;
    int32_t bag = -1;
    int32_t slot = -1;
    return contracts::succeeded(projection->identify_extension_item(item, &bag, &slot));
}

bool can_apply_stuff(contracts::QolItemHandle target, contracts::QolItemHandle material,
                     int32_t* out_allowed) {
    return out_allowed != nullptr && g_character_port != nullptr &&
           g_character_port->read.can_apply_stuff_items != nullptr &&
           contracts::succeeded(g_character_port->read.can_apply_stuff_items(
               target, material, out_allowed));
}

bool item_facts(const contracts::ItemFactsApi* facts, contracts::QolItemHandle item,
                contracts::QolItemInventoryView* view,
                int32_t* count) {
    if (facts == nullptr || facts->view == nullptr || facts->cumulate_count == nullptr ||
        facts->category == nullptr ||
        view == nullptr || count == nullptr) return false;
    *view = {};
    return contracts::succeeded(facts->view(item, view)) &&
           view->count_supported != 0 &&
           contracts::succeeded(facts->cumulate_count(item, count));
}

bool item_count_encoding(const contracts::ItemFactsApi* facts, contracts::QolItemHandle item) {
    if (facts == nullptr || facts->count_encoding == nullptr) return false;
    contracts::QolItemCountEncoding encoding = contracts::QolItemCountEncoding::kUnknown;
    if (!contracts::succeeded(facts->count_encoding(item, &encoding)) ||
        encoding != contracts::QolItemCountEncoding::kEncoded) return false;
    return true;
}

void report_item_proc_source(const contracts::InventoryMergeProjectionApi* api, uint64_t token,
                             void* control, uint64_t event, void* x2, void* param,
                             void* source_control) {
    const auto* current = projection_api();
    if (current != nullptr && current->observe_item_proc_source != nullptr) {
        current->observe_item_proc_source(token, control, event, x2, param, source_control);
    }
    (void)api;
}

uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const contracts::InFlightGuard guard(g_sdk == nullptr ? nullptr : &g_sdk->in_flight,
                                         {kX1Resource});
    const auto* projection = projection_api();
    const uint64_t observation = projection != nullptr && projection->observe_item_proc_pre != nullptr
                                     ? projection->observe_item_proc_pre(control, event, x2, param)
                                     : 0;
    const auto finish = [observation, control, event, x2, param](uint64_t result) {
        projection_observe_post(nullptr, observation, control, event, x2, param, result);
        return result;
    };

    if (event == 0x02 && param != nullptr) {
        void* source = nullptr;
        if (g_sdk != nullptr && g_sdk->ui != nullptr &&
            g_sdk->ui->item_proc_source_control != nullptr &&
            contracts::succeeded(g_sdk->ui->item_proc_source_control(control, event, param, &source))) {
            report_item_proc_source(projection, observation, control, event, x2, param, source);
            const auto* current = projection_api();
            if (current != nullptr && current->projection_drop != nullptr) {
                const auto outcome = current->projection_drop(control, source);
                if (outcome == contracts::MoveMergeProjectionDropResult::kHandled) return finish(1);
                if (outcome == contracts::MoveMergeProjectionDropResult::kRejected) return finish(0);
            }
            contracts::QolItemHandle source_handle{};
            const auto* current_projection = projection_api();
            if (!control_identity(source, &source_handle)) return finish(0);
            if (extension_item(current_projection, source_handle) || !native_item(source_handle)) {
                return finish(0);
            }
        }
    }

    if (event == 0x04) {
        void* source = nullptr;
        const bool has_source = param != nullptr && g_sdk != nullptr && g_sdk->ui != nullptr &&
                                g_sdk->ui->item_proc_source_control != nullptr &&
                                contracts::succeeded(g_sdk->ui->item_proc_source_control(
                                    control, event, param, &source));
        contracts::QolItemHandle source_handle{};
        const auto* current_projection = projection_api();
        const bool has_identity = has_source && control_identity(source, &source_handle);
        if (!has_source || !has_identity) return finish(0);
        const bool protected_source = has_identity &&
            (extension_item(current_projection, source_handle) || !native_item(source_handle));
        if (protected_source) return finish(0);
    }

    if (event == 0x04 && param != nullptr) {
        void* source = nullptr;
        const bool has_source = g_sdk != nullptr && g_sdk->ui != nullptr &&
                                g_sdk->ui->item_proc_source_control != nullptr &&
                                contracts::succeeded(g_sdk->ui->item_proc_source_control(
                                    control, event, param, &source));
        if (has_source) report_item_proc_source(projection, observation, control, event, x2, param, source);
        const auto* current = projection_api();
        if (current != nullptr && current->original_item_input_blocked != nullptr &&
            current->original_item_input_blocked()) {
            if (has_source && current->allow_original_tab_drop != nullptr &&
                current->allow_original_tab_drop(control, source, 0)) {
                return finish(call_original(control, event, x2, param));
            }
            int32_t slot = -1;
            if (control_slot(control, &slot) && slot >= 16) return finish(0);
            return finish(0);
        }
    }

    if (gate_enabled() && event == 0x04 && param != nullptr && g_move_port != nullptr &&
        g_move_port->move_item != nullptr && g_move_port->current_bag != nullptr) {
        void* source = nullptr;
        if (g_sdk != nullptr && g_sdk->ui != nullptr &&
            g_sdk->ui->item_proc_source_control != nullptr &&
            contracts::succeeded(g_sdk->ui->item_proc_source_control(control, event, param, &source))) {
            report_item_proc_source(projection, observation, control, event, x2, param, source);
            contracts::QolItemHandle source_handle{};
            contracts::QolItemHandle destination_handle{};
            bool extension_source = false;
            const auto* current_projection = projection_api();
            if (current_projection != nullptr &&
                current_projection->identify_extension_item != nullptr &&
                control_identity(source, &source_handle)) {
                extension_source = extension_item(current_projection, source_handle);
            }
            if (control_identity(source, &source_handle) && control_identity(control, &destination_handle)) {
                contracts::QolItemInventoryView source_view{};
                contracts::QolItemInventoryView destination_view{};
                int32_t source_count = 0;
                int32_t destination_count = 0;
                int32_t source_category = -1;
                int32_t destination_category = -1;
                const auto* facts = item_facts_api();
                const bool facts_ready = native_item(source_handle) &&
                                         native_item(destination_handle) &&
                                         item_count_encoding(facts, source_handle) &&
                                         item_count_encoding(facts, destination_handle) &&
                                         item_facts(facts, source_handle, &source_view, &source_count) &&
                                         item_facts(facts, destination_handle, &destination_view,
                                                    &destination_count) &&
                                         contracts::succeeded(facts->category(
                                             source_handle, &source_category)) &&
                                         contracts::succeeded(facts->category(
                                             destination_handle, &destination_category));
                int32_t apply_allowed = 0;
                const bool apply_known = facts_ready &&
                                         can_apply_stuff(destination_handle, source_handle,
                                                         &apply_allowed);
                if (facts_ready && apply_known && apply_allowed == 0 &&
                    source_category == destination_category &&
                    source_count > 0 && destination_count > 0) {
                    int32_t limit = 0;
                    if (facts->count_limit != nullptr) {
                        uint32_t count_limit = 0;
                        if (contracts::succeeded(facts->count_limit(destination_handle,
                                                                           &count_limit))) {
                            limit = static_cast<int32_t>(count_limit);
                        }
                    }
                    int32_t target_bag = -1;
                    int32_t destination_slot = -1;
                    int32_t source_slot = -1;
                    const bool location_ready = contracts::succeeded(
                                                    g_move_port->current_bag(&target_bag)) &&
                                                control_slot(control, &destination_slot) &&
                                                control_slot(source, &source_slot);
                    if (location_ready && target_bag >= 0 && target_bag <= 5 &&
                        destination_slot >= 0 && destination_slot < 16 && source_slot >= 0 &&
                        source_slot < 16 && destination_count >= 0 && source_count > 0 &&
                        limit >= static_cast<int32_t>(destination_count) &&
                        source_count <= limit - destination_count) {
                        int32_t moved = 0;
                        const auto result = g_move_port->move_item(source_handle, source_count,
                                                                   target_bag, destination_slot,
                                                                   &moved);
                        if (contracts::succeeded(result) && moved != 0) {
                            const auto* current = projection_api();
                            if (current != nullptr && current->module_view_installed != nullptr &&
                                current->module_view_installed() &&
                                current->sync_projected_slot != nullptr) {
                                (void)current->sync_projected_slot(target_bag, source_slot);
                                (void)current->sync_projected_slot(target_bag, destination_slot);
                            }
                            const auto* refresh = refresh_api();
                            if (refresh != nullptr && refresh->call_original != nullptr) {
                                (void)refresh->call_original();
                            }
                            void* root = nullptr;
                            const auto* ui = g_sdk != nullptr ? g_sdk->ui : nullptr;
                            if (ui != nullptr && ui->find_root != nullptr && ui->set_touch_cursor != nullptr &&
                                contracts::succeeded(ui->find_root(
                                    contracts::QolUiRootKind::kEquipItemRoot, &root)) && root != nullptr) {
                                (void)ui->set_touch_cursor(root, nullptr);
                            }
                            return finish(0);
                        }
                        const auto* current = projection_api();
                        if (current != nullptr && current->identify_extension_item != nullptr) {
                            int32_t bag = -1;
                            int32_t slot = -1;
                            if (contracts::succeeded(current->identify_extension_item(
                                    source_handle, &bag, &slot))) return finish(0);
                        }
                    }
                }
            }
            if (extension_source) return finish(0);
        }
    }

    const uint64_t result = call_original(control, event, x2, param);
    const auto* current = projection_api();
    if (current != nullptr && current->module_view_installed != nullptr &&
        current->module_view_installed() && current->sync_projected_bag != nullptr) {
        (void)current->sync_projected_bag();
    }
    return finish(result);
}

bool control_enabled() { return gate_enabled(); }
const contracts::MoveMergeControlApi kControlApi{
    sizeof(contracts::MoveMergeControlApi),
    contracts::kMoveMergeControlVersionMajor,
    contracts::kMoveMergeControlVersionMinor,
    &control_enabled,
};
const contracts::CapabilityProvisionRef kProvides[] = {{
    contracts::kMoveMergeControlCapabilityId,
    {contracts::kMoveMergeControlVersionMajor, contracts::kMoveMergeControlVersionMinor},
    &kControlApi,
}};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.ports == nullptr || ctx.port_count != 2 ||
        ctx.ports[0] == nullptr || ctx.ports[1] == nullptr ||
        ctx.sdk->capabilities.require == nullptr) return 1;
    const auto* port = static_cast<const contracts::QolMoveMergePort*>(ctx.ports[0]);
    const auto* character =
        static_cast<const contracts::QolCharacterExtrasPort*>(ctx.ports[1]);
    if (port->abi_version.major != contracts::kQolMoveMergePortVersion.major ||
        port->abi_version.minor < contracts::kQolMoveMergePortVersion.minor ||
        port->move_item == nullptr || port->current_bag == nullptr ||
        port->validate_native_item == nullptr ||
        character->abi_version.major != contracts::kQolPortCharacterExtrasVersion.major ||
        character->abi_version.minor < contracts::kQolPortCharacterExtrasVersion.minor ||
        !contracts::qol_character_extras_read_api_size_valid(&character->read) ||
        character->read.can_apply_stuff_items == nullptr) return 1;
    const void* item_iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor},
            &item_iface)) || item_iface == nullptr ||
        !contracts::item_facts_api_size_valid(
            static_cast<const contracts::ItemFactsApi*>(item_iface)) ||
        !contracts::item_facts_api_count_encoding_size_valid(
            static_cast<const contracts::ItemFactsApi*>(item_iface)) ||
        static_cast<const contracts::ItemFactsApi*>(item_iface)->count_encoding == nullptr) return 1;
    g_sdk = ctx.sdk;
    g_move_port = port;
    g_character_port = character;
    g_active.store(true, std::memory_order_release);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    g_active.store(false, std::memory_order_release);
    (void)ctx;
    g_character_port = nullptr;
    g_move_port = nullptr;
    g_sdk = nullptr;
}

void plugin_on_enabled(contracts::PluginContext&) { g_active.store(true, std::memory_order_release); }
void plugin_on_disabled(contracts::PluginContext&) { g_active.store(false, std::memory_order_release); }
void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = g_active.load(std::memory_order_acquire);
    out.error = nullptr;
}

const contracts::QolPortRequirement kPortRequirements[] = {{
    contracts::QolPortId::QOL_PORT_MOVE_MERGE,
    {contracts::kQolMoveMergePortVersion.major, contracts::kQolMoveMergePortVersion.minor},
}, {
    contracts::QolPortId::QOL_PORT_CHARACTER_EXTRAS,
    {contracts::kQolPortCharacterExtrasVersion.major,
     contracts::kQolPortCharacterExtrasVersion.minor},
}};
// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"move_merge.drag_merge", 0, contracts::FeatureCategory::kGameplay,
     contracts::FeatureType::kToggle, "拖拽合并", "拖拽同类物品时自动合并。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor{
    kPluginId, "0.1.0", 1, kRequires, 2, kOptionalRequires, 1, kProvides,
    1, kResources, 0, nullptr, &plugin_install, &plugin_deactivate, &plugin_on_enabled,
    &plugin_on_disabled, &plugin_fill_status, nullptr, kPortRequirements, 2,
    nullptr, 0,
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() { return kDescriptor; }

}  // namespace qol::features::move_merge
