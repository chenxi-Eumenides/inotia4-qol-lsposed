#include "game/access/inventory.h"
#include "game/access/item_handle_registry.h"
#include "game/adapter_ports/inventory_extension_facade.h"
#include "game/access/item_class.h"
#include "game/access/game_access.h"
#include "game/access/ui_state.h"
#include "game/access/world.h"
#include "game/json/game_json.h"
#include "contracts/game_ports/ui_write_port.h"
#include "game/nav/game_nav.h"
#include "game/symbols/game_symbols.h"
#include "host/plugin_registry.h"
#include "patch/ptr_slot.h"

#include <string>
#include <cstring>

int g_host_ui_step_calls = 0;
int g_host_ui_cancel_calls = 0;
int g_host_ui_snapshot_reads = 0;
int g_host_ui_not_ready_reads = 0;
std::string g_host_ui_snapshot = "{\"screen\":\"world\"}";

uint8_t* g_snd_fx = nullptr;
UiEquipMakeDescFn fn_ui_equip_make_desc = nullptr;
decltype(fn_inven_find_item_slot) fn_inven_find_item_slot = nullptr;
extern qol::contracts::QolItemHandle g_inventory_item_handle;

namespace qol::game {

int item_sell_price(const void*) { return -1; }
int item_class_category(const void*) { return 28; }

// host_adapter_tests 不链接真实 game/adapter_ports/system_port.cpp；身份版本按未下发处理。
const char* system_port_version() { return nullptr; }

#ifndef QOL_HOST_ADAPTER_REAL_INVENTORY
namespace {
contracts::QolFindItemAbiFn g_test_find_item_backup = nullptr;
thread_local uint32_t g_test_original_only_depth = 0;
}

void* game_inventory_find_item_hook(int32_t category) {
    if (g_test_find_item_backup != nullptr) {
        void* original = g_test_find_item_backup(category);
        if (original != nullptr) return original;
    }
    if (g_test_original_only_depth != 0) return nullptr;
    const contracts::QolInventoryExtensionProvider* provider =
        inventory_extension_provider_if_resident();
    if (provider == nullptr || provider->for_each_item == nullptr) return nullptr;
    struct Context {
        int32_t category;
        contracts::QolItemHandle item;
    } context{category, contracts::kInvalidItemHandle};
    const QolResult result = provider->for_each_item(
        [](const contracts::QolInventoryExtensionItemRef* ref, void* raw) -> int32_t {
            auto* state = static_cast<Context*>(raw);
            if (ref == nullptr || ref->category != state->category) return 0;
            state->item = ref->item;
            return 1;
        },
        &context);
    if (!contracts::succeeded(result) || context.item.slot == 0) return nullptr;
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(context.item, contracts::kQolItemHandleOwnerAny);
    return resolved.error == ItemHandleError::kNone ? resolved.item : nullptr;
}

QolResult game_inventory_bind_find_item_backup(contracts::QolFindItemAbiFn backup) {
    if (backup == nullptr) return contracts::error(kQolErrInvalidArgument, "null", 4);
    g_test_find_item_backup = backup;
    return contracts::ok();
}

QolResult game_original_find_item_slot(contracts::QolItemHandle item, int32_t* out_bag,
                                       int32_t* out_slot) {
    if (out_bag == nullptr || out_slot == nullptr || fn_inven_find_item_slot == nullptr) {
        return contracts::error(kQolErrNotReady, "test stub", 9);
    }
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(item, contracts::kQolItemHandleOwnerAny);
    if (resolved.error != ItemHandleError::kNone) {
        return contracts::error(kQolErrNotFound, "stale", 5);
    }
    int8_t encoded = -1;
    if (fn_inven_find_item_slot(resolved.item, &encoded) == 0) {
        return contracts::error(kQolErrNotFound, "not found", 9);
    }
    *out_bag = static_cast<uint8_t>(encoded) >> 5;
    *out_slot = static_cast<uint8_t>(encoded) & 0x1f;
    return contracts::ok();
}

QolResult game_item_identity_at(int32_t, int32_t,
                                contracts::QolItemHandle* out_item) {
    if (out_item == nullptr) return contracts::error(kQolErrInvalidArgument, "null", 4);
    *out_item = ::g_inventory_item_handle;
    return out_item->slot == 0 ? contracts::error(kQolErrNotFound, "empty", 5)
                               : contracts::ok();
}

void game_begin_original_only() { ++g_test_original_only_depth; }
void game_end_original_only() {
    if (g_test_original_only_depth != 0) --g_test_original_only_depth;
}
QolResult game_native_item_at(int32_t, int32_t, void** out_item) {
    if (out_item != nullptr) *out_item = nullptr;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}
QolResult game_native_item_for_handle(contracts::QolItemHandle item, void** out_item) {
    if (out_item == nullptr) return contracts::error(kQolErrInvalidArgument, "null", 4);
    const ItemHandleResolution resolved =
        item_handle_registry().resolve_item(item, contracts::kQolItemHandleOwnerAny);
    *out_item = resolved.error == ItemHandleError::kNone ? resolved.item : nullptr;
    return *out_item == nullptr ? contracts::error(kQolErrNotFound, "stale", 5)
                                : contracts::ok();
}

QolResult physical_item_at(int32_t, int32_t, void** out_item) {
    if (out_item != nullptr) *out_item = nullptr;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult detail_item_context(contracts::QolItemDetailContext* out_context) {
    if (out_context != nullptr) {
        *out_context = {0, -1, -1, contracts::kInvalidItemHandle};
    }
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult current_bag(int32_t* out_bag) {
    if (out_bag != nullptr) *out_bag = -1;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult set_current_bag(int32_t) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult desc_type(int32_t* out_type) {
    if (out_type != nullptr) *out_type = -1;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult set_desc_type(int32_t) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult bag_capacity_word(int32_t, uint32_t* out_word) {
    if (out_word != nullptr) *out_word = 0;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult set_bag_capacity_word(int32_t, uint32_t) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    if (out_desc_type != nullptr) *out_desc_type = 0;
    if (out_bag != nullptr) *out_bag = 0;
    if (out_control != nullptr) *out_control = nullptr;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}

QolResult cursor_index(const void*, int32_t* out_slot) {
    if (out_slot != nullptr) *out_slot = -1;
    return contracts::error(kQolErrNotReady, "test stub", 9);
}
#endif

contracts::QolItemCountEncoding item_count_encoding_for_category(int) {
    return contracts::QolItemCountEncoding::kUnknown;
}

bool extension_desc_button_slot(uint32_t, PtrSlotSite* out) {
    if (out == nullptr) return false;
    out->slot_addr = 0;
    out->expected_value = nullptr;
    return false;
}

bool extension_desc_button_instance(uint32_t, void** out_button) {
    if (out_button != nullptr) *out_button = nullptr;
    return false;
}

bool extension_desc_button_original_matches(void*, const char*) { return false; }

// P2/G5 语义面在 host 测试中的桩（真实实现位于 game/access/*.cpp，测试目标不编译它们）。
void* char_party_member(int32_t) { return nullptr; }

#ifndef QOL_HOST_ADAPTER_REAL_INVENTORY
QolResult set_physical_item(int32_t, int32_t, void*) {
    return contracts::error(kQolErrNotReady, "test stub", 9);
}
#endif

int32_t g_test_craft_apply_stuff_result = -1;
int32_t craft_is_apply_stuff(void*, void*) { return g_test_craft_apply_stuff_result; }

// legacy 聚合 TU 的写路由拼装接缝（真实实现位于 game_system.cpp，测试目标不编译它）。
std::string build_quest_quit_result_json(int32_t) { return "{\"ok\":false,\"error\":\"stub\"}"; }

int32_t build_ui_write_result_json(int32_t, const char*, char* out, uint32_t capacity,
                                   uint32_t* out_size) {
    constexpr char kSuccess[] = "{\"ok\":true}";
    if (out == nullptr || out_size == nullptr || capacity < sizeof(kSuccess) - 1) return 1;
    std::memcpy(out, kSuccess, sizeof(kSuccess) - 1);
    *out_size = sizeof(kSuccess) - 1;
    return 0;
}

QolResult ui_state_write_step(uint64_t, uint32_t, int32_t, const char*, char* out,
                              uint32_t capacity, uint32_t* out_size) {
    ++g_host_ui_step_calls;
    constexpr char kSuccess[] = "{\"ok\":true}";
    if (out == nullptr || out_size == nullptr || capacity < sizeof(kSuccess) - 1) {
        return contracts::error(kQolErrInternal, "UI result buffer unavailable", 28);
    }
    std::memcpy(out, kSuccess, sizeof(kSuccess) - 1);
    *out_size = sizeof(kSuccess) - 1;
    return contracts::ok();
}

QolResult ui_state_write_cancel_pending(uint64_t) {
    ++g_host_ui_cancel_calls;
    return contracts::ok();
}

QolResult ui_write_result_state_json(int32_t kind, std::string* out_json) {
    if (out_json == nullptr) return contracts::error(kQolErrInvalidArgument, "output required", 15);
    if (kind == contracts::kQolUiWriteMainMenu) {
        *out_json = "{\"player\":true}";
        return contracts::ok();
    }
    if (kind != contracts::kQolUiWritePanelOpen && kind != contracts::kQolUiWritePanelClose) {
        return contracts::error(kQolErrInvalidArgument, "invalid UI write kind", 21);
    }
    ++g_host_ui_snapshot_reads;
    if (g_host_ui_not_ready_reads > 0) {
        --g_host_ui_not_ready_reads;
        out_json->clear();
        return contracts::error(kQolErrNotReady, "UI result state not ready", 25);
    }
    *out_json = g_host_ui_snapshot;
    return qol::game::json::is_ready_state_snapshot(*out_json)
               ? contracts::ok()
               : contracts::error(kQolErrNotReady, "UI result state not ready", 25);
}

}  // namespace qol::game

bool game_in_world() { return false; }
void* lead_member() { return nullptr; }
const uint8_t* nav_tiles() { return nullptr; }
bool nav_blocked(const uint8_t*, int, int) { return true; }
void nav_unit_blocks(bool* blocks) {
    if (blocks == nullptr) return;
    for (int i = 0; i < NAV_W * NAV_H; ++i) blocks[i] = false;
}


std::string build_inventory_json() { return "{\"bags\":[]}"; }

namespace qol::patch {

bool ptr_slot_write_raw(uintptr_t, void*) { return false; }
bool ptr_slot_restore_raw(uintptr_t, void*) { return false; }

}  // namespace qol::patch

AddMoneyFn fn_add_money = nullptr;
RemoveItemDirectFn fn_remove_item_direct = nullptr;

// T2l-sho：game/access/shop.cpp 读原语经 game_access 函数指针；nullptr = 空货架（{"items":[]}）。
// fn_get_bit/fn_get_cumulate_count 由 test_host_adapters.cpp 提供（本文件不重复定义）。
ItemGetBuyPriceFn fn_item_get_buy_price = nullptr;

// T2l-w-read2：world_port 读面在 host_adapter_tests 用桩（product 链 game/access/world_read.cpp）。
namespace qol::game {
std::string build_units_json() { return "{\"units\":[]}"; }
std::string build_enemies_json() { return "{\"enemies\":[]}"; }
std::string build_interactives_json() { return "{\"interactives\":[]}"; }
std::string build_drops_json() { return "{\"drops\":[]}"; }
}  // namespace qol::game

// The craft write adapter forwards its typed random op to the resolved MATH_GetRandom symbol;
// host tests never resolve it (the fake port answers), so the link-time symbol is provided here.
int (*fn_math_get_random)(int, int) = nullptr;

// host_adapter_tests exercises adapter forwarding only; the production feature registry
// (host/plugin_registry.cpp) is not linked here, so the new-model reconciler seam is stubbed.
namespace qol::host {
uint32_t plugin_count() { return 0; }
const contracts::PluginDescriptor* find_plugin(const char*) { return nullptr; }
bool feature_registry_prepare(const HostDeps&) { return true; }
void feature_registry_bind_desired_request(FeatureDesiredRequest, void*) {}
uint32_t feature_registry_order(const char**, uint32_t) { return 0; }
bool feature_registry_dependencies_wanted(const char*, FeatureWants, void*) { return true; }
bool feature_registry_dependencies_ready(const char*) { return true; }
bool feature_registry_busy(const char*) { return false; }
bool feature_registry_active(const char*) { return false; }
bool feature_registry_transition(const char*, bool) { return false; }
bool feature_registry_context(const char*, contracts::PluginContext*) { return false; }
void feature_registry_set_wanted(const char*, bool) {}
uint32_t feature_registry_step() { return 0; }
bool feature_registry_owner_ready(const char*) { return true; }
}  // namespace qol::host
