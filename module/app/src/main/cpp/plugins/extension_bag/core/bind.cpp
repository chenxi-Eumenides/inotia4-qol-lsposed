#include "plugins/extension_bag/core/host_bindings.h"
#include "plugins/extension_bag/core/drag_session.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/ui_ports.h"
#include "contracts/ui.h"

#include <cstddef>

namespace qol::features::extension_bag {

namespace {

constexpr std::size_t kRequiredUiAdapterSize =
    offsetof(qol::contracts::QolUiAdapter, draw_part_alpha);

}  // namespace

bool extension_bag_bind_host(const qol::contracts::QolHostSdk* sdk,
                             const qol::contracts::QolGameHost* game,
                             const qol::contracts::QolExtensionBagWritePort* write_port) {
    if (sdk == nullptr || game == nullptr || sdk->ui == nullptr ||
        write_port == nullptr ||
        write_port->abi_version.major != qol::contracts::kQolPortExtensionBagWriteVersion.major ||
        write_port->abi_version.minor != qol::contracts::kQolPortExtensionBagWriteVersion.minor ||
        sdk->abi.size < sizeof(qol::contracts::QolHostSdk) ||
        sdk->abi.major != qol::contracts::kHostSdkMajor ||
        game->abi.size < sizeof(qol::contracts::QolGameHost) ||
        game->abi.major != qol::contracts::kGameHostMajor ||
        sdk->ui->size < kRequiredUiAdapterSize ||
        sdk->ui->major != qol::contracts::kUiAdapterMajor ||
        sdk->hooks.popup_resolve == nullptr || sdk->hooks.popup_acquire == nullptr ||
        sdk->hooks.popup_restore == nullptr || sdk->desc_button.bind == nullptr ||
        sdk->desc_button.unbind == nullptr || sdk->desc_button.current_proc == nullptr ||
        sdk->desc_button.original_matches == nullptr || sdk->desc_button.call_original == nullptr ||
        sdk->scheduler.add == nullptr ||
        sdk->scheduler.remove == nullptr || sdk->capabilities.require == nullptr) {
        runtime_state().host.sdk = nullptr;
        runtime_state().host.game = nullptr;
        runtime_state().host.write_port = nullptr;
        runtime_state().host.character_extras = nullptr;
        runtime_state().host.inventory_extras = nullptr;
        return false;
    }
    runtime_state().host.sdk = sdk;
    runtime_state().host.game = game;
    runtime_state().host.write_port = write_port;
    set_ui_ports(sync_projected_slot_locked, [] { reset_drag_state_locked(nullptr); });
    return true;
}

void extension_bag_bind_character_extras(
    const qol::contracts::QolCharacterExtrasPort* port) {
    runtime_state().host.character_extras = port;
}

void extension_bag_bind_inventory_extras(
    const qol::contracts::QolInventoryExtrasPort* port) {
    runtime_state().host.inventory_extras = port;
}

void extension_bag_bind_move_merge(const qol::contracts::QolMoveMergePort* port) {
    runtime_state().host.move_merge = port;
}

void extension_bag_bind_item_facts(const qol::contracts::ItemFactsApi* facts) {
    runtime_state().host.item_facts = facts;
}

const qol::contracts::ItemFactsApi* extension_bag_item_facts() { return runtime_state().host.item_facts; }

void extension_bag_bind_craft_facts(const qol::contracts::CraftFactsApi* facts) {
    runtime_state().host.craft_facts = facts;
}

}  // namespace qol::features::extension_bag
