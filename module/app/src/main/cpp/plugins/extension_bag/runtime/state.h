#pragma once

#include "contracts/craft_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/extension_bag_write_port.h"
#include "contracts/game_ports/inventory_extras_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/save_facts.h"
#include "contracts/world_facts.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"

#include <atomic>
#include <cstdint>
#include <jni.h>

struct ModuleUseToken;

struct ExtensionBagHostPorts {
    const qol::contracts::QolHostSdk* sdk = nullptr;
    const qol::contracts::QolGameHost* game = nullptr;
    const qol::contracts::QolExtensionBagWritePort* write_port = nullptr;
    const qol::contracts::QolCharacterExtrasPort* character_extras = nullptr;
    const qol::contracts::QolInventoryExtrasPort* inventory_extras = nullptr;
    const qol::contracts::ItemFactsApi* item_facts = nullptr;
    const qol::contracts::CraftFactsApi* craft_facts = nullptr;
    const qol::contracts::WorldFactsApi* world_facts = nullptr;
    const qol::contracts::SaveFactsApi* save_facts = nullptr;
};

struct ExtensionBagPersistenceState {
    bool dirty = false;
    std::atomic<uint64_t> revision{0};
    int64_t shortcut_refreshed_revision = -1;
    int loaded_slot = -2;
    uint8_t* state_entry = nullptr;
    uint8_t* save_state_entry = nullptr;
    jclass bridge_class = nullptr;
};

struct ExtensionBagBusinessState {
    virtual_bag::State bag{};
};

struct ExtensionBagRuntimeState {
    ExtensionBagHostPorts host{};
    ExtensionBagPersistenceState persistence{};
    ExtensionBagBusinessState objects{};
    std::atomic<bool> enabled{false};
};

inline ExtensionBagRuntimeState& runtime_state() {
    static ExtensionBagRuntimeState state{};
    return state;
}

virtual_bag::State* extension_bag_state();

extern thread_local ModuleUseToken* g_active_module_use_token;
extern thread_local bool g_p5_observation_active;
extern thread_local bool g_refresh_restore_suppress;
extern thread_local bool g_in_native_call;
