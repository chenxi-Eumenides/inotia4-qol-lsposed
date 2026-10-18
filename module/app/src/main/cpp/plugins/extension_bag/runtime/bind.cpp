#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_internal.h"

namespace qol::features::extension_bag {

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
        sdk->ui->size < sizeof(qol::contracts::QolUiAdapter) ||
        sdk->ui->major != qol::contracts::kUiAdapterMajor ||
        sdk->hooks.popup_resolve == nullptr || sdk->hooks.popup_acquire == nullptr ||
        sdk->hooks.popup_restore == nullptr || sdk->desc_button.bind == nullptr ||
        sdk->desc_button.unbind == nullptr || sdk->desc_button.current_proc == nullptr ||
        sdk->desc_button.original_matches == nullptr || sdk->desc_button.call_original == nullptr ||
        sdk->scheduler.add == nullptr ||
        sdk->scheduler.remove == nullptr || sdk->capabilities.require == nullptr) {
        g_sdk = nullptr;
        g_game = nullptr;
        g_write_port = nullptr;
        g_character_extras = nullptr;
        g_inventory_extras = nullptr;
        return false;
    }
    g_sdk = sdk;
    g_game = game;
    g_write_port = write_port;
    return true;
}

void extension_bag_bind_character_extras(
    const qol::contracts::QolCharacterExtrasPort* port) {
    g_character_extras = port;
}

void extension_bag_bind_inventory_extras(
    const qol::contracts::QolInventoryExtrasPort* port) {
    g_inventory_extras = port;
}

const qol::contracts::QolGameHost* extension_bag_game_host() { return g_game; }
const qol::contracts::QolHostSdk* extension_bag_host_sdk() { return g_sdk; }

void extension_bag_bind_item_facts(const qol::contracts::ItemFactsApi* facts) {
    g_item_facts = facts;
}

const qol::contracts::ItemFactsApi* extension_bag_item_facts() { return g_item_facts; }

void extension_bag_bind_craft_facts(const qol::contracts::CraftFactsApi* facts) {
    g_craft_facts = facts;
}

const qol::contracts::CraftFactsApi* extension_bag_craft_facts() { return g_craft_facts; }

}  // namespace qol::features::extension_bag
