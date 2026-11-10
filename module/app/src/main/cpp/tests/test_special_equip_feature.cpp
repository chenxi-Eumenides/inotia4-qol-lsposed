#include "contracts/character_facts.h"
#include "contracts/events.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "plugins/special_equip/feature.h"
#include "plugins/special_equip/plugin.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qol::contracts;
using namespace qol::features::special_equip;

int g_item_object = 0;
int g_character_object = 0;
int g_menu_character_object = 0;

QolHostSdk g_sdk{};
QolGameHost g_game{};
QolCharacterExtrasPort g_extras{};
ItemFactsApi g_item_facts{};
CharacterFactsApi g_character_facts{};
PluginContext g_context{};
const void* g_ports[] = {nullptr};

bool g_item_capability_available = true;
bool g_character_capability_available = true;
bool g_intercept_succeeds = true;
bool g_handle_registered = false;
bool g_register_succeeds = true;
bool g_real_equip_succeeds = true;
bool g_category_succeeds = true;
bool g_equip_level_succeeds = true;
bool g_can_change_succeeds = true;
bool g_character_level_succeeds = true;
bool g_menu_character_succeeds = true;
bool g_character_type_succeeds = true;
bool g_merc_slot_succeeds = true;
bool g_desc_type_succeeds = true;
bool g_debug_enabled = false;
bool g_menu_character_present = true;

int32_t g_real_equip_value = 1;
int32_t g_category_value = 485;
int32_t g_item_level_value = 10;
int32_t g_can_change_value = 1;
int32_t g_character_level_value = 20;
int32_t g_character_type_value = 0;
int32_t g_merc_slot_value = 0;
int32_t g_desc_type_value = 0;
int32_t g_no_equip_bit_value = 1;
int32_t g_original_equip_result = 0;
int32_t g_original_unequip_result = 0;

int g_log_writes = 0;
int32_t g_last_log_domain = -1;
std::string g_last_log_message;
int g_debug_queries = 0;
int g_require_calls = 0;
int g_set_intercept_calls = 0;
int g_clear_intercept_calls = 0;
int g_in_flight_enters = 0;
int g_in_flight_leaves = 0;
int g_lookup_calls = 0;
int g_register_calls = 0;
int g_real_equip_calls = 0;
int g_category_calls = 0;
int g_equip_level_calls = 0;
int g_class_no_equip_bit_calls = 0;
int g_can_change_calls = 0;
int g_character_level_calls = 0;
int g_menu_character_calls = 0;
int g_character_type_calls = 0;
int g_merc_slot_calls = 0;
int g_desc_type_calls = 0;
int g_backup_equip_calls = 0;
int g_backup_unequip_calls = 0;

// ---- 事件总线 fake：记录订阅并支持按类型注入 payload ----
struct FakeEventSubscription {
    uint32_t type = 0;
    QolEventHandler handler = nullptr;
    void* ctx = nullptr;
    uint64_t id = 0;
};

std::vector<FakeEventSubscription> g_event_subscriptions;
int g_event_subscribe_calls = 0;
int g_event_unsubscribe_calls = 0;
uint64_t g_next_event_subscription = 1;

QolResult fake_event_subscribe(uint32_t type, QolEventHandler handler, void* ctx,
                               uint64_t* out_id) {
    ++g_event_subscribe_calls;
    assert(handler != nullptr && out_id != nullptr);
    FakeEventSubscription subscription{};
    subscription.type = type;
    subscription.handler = handler;
    subscription.ctx = ctx;
    subscription.id = g_next_event_subscription++;
    g_event_subscriptions.push_back(subscription);
    *out_id = subscription.id;
    return ok();
}

QolResult fake_event_unsubscribe(uint64_t id) {
    ++g_event_unsubscribe_calls;
    for (auto it = g_event_subscriptions.begin(); it != g_event_subscriptions.end(); ++it) {
        if (it->id == id) {
            g_event_subscriptions.erase(it);
            return ok();
        }
    }
    return error(kQolErrNotFound, "not found", 9);
}

void publish_fake_event(uint32_t type, const void* payload) {
    const std::vector<FakeEventSubscription> snapshot = g_event_subscriptions;
    for (const FakeEventSubscription& subscription : snapshot) {
        if (subscription.type == type) subscription.handler(subscription.ctx, payload);
    }
}

std::string g_intercept_resource_id;
int32_t (*g_intercept_predicate)(void*) = nullptr;
void* g_intercept_context = nullptr;
bool g_intercept_registered = false;
bool g_optional_warning_logged = false;
QolItemHandle g_item_handle{7, 3};

QolResult failure() {
    return error(kQolErrNotReady, "fake failure", 12);
}

QolResult fake_log_write(int32_t, int32_t domain, const char*, const char* message,
                         uint32_t message_len) {
    ++g_log_writes;
    g_last_log_domain = domain;
    g_last_log_message.assign(message == nullptr ? "" : message,
                              message == nullptr ? 0 : message_len);
    if (g_last_log_message.find("optional dependency unavailable") != std::string::npos) {
        g_optional_warning_logged = true;
    }
    return ok();
}

int32_t fake_debug_enabled() {
    ++g_debug_queries;
    return g_debug_enabled ? 1 : 0;
}

QolResult fake_require(const char* id, QolVersionRange, const void** out_iface) {
    ++g_require_calls;
    if (id == nullptr || out_iface == nullptr) return failure();
    if (std::strcmp(id, kItemFactsCapabilityId) == 0 && g_item_capability_available) {
        *out_iface = &g_item_facts;
        return ok();
    }
    if (std::strcmp(id, kCharacterFactsCapabilityId) == 0 &&
        g_character_capability_available) {
        *out_iface = &g_character_facts;
        return ok();
    }
    *out_iface = nullptr;
    return error(kQolErrNotFound, "missing capability", 18);
}

QolResult fake_set_intercept(const char* resource_id, int32_t (*predicate)(void*), void* ctx) {
    ++g_set_intercept_calls;
    if (!g_intercept_succeeds) return failure();
    g_intercept_resource_id = resource_id == nullptr ? "" : resource_id;
    g_intercept_predicate = predicate;
    g_intercept_context = ctx;
    g_intercept_registered = true;
    return ok();
}

QolResult fake_clear_intercept(const char* resource_id) {
    ++g_clear_intercept_calls;
    if (resource_id != nullptr && g_intercept_resource_id == resource_id) {
        g_intercept_registered = false;
        g_intercept_predicate = nullptr;
        g_intercept_context = nullptr;
    }
    return ok();
}

QolResult fake_in_flight_enter(InFlightHandle handle) {
    assert(handle.resource_id != nullptr);
    ++g_in_flight_enters;
    return ok();
}

QolResult fake_in_flight_leave(InFlightHandle handle) {
    assert(handle.resource_id != nullptr);
    ++g_in_flight_leaves;
    return ok();
}

int32_t fake_register_item(void* item, uint32_t provider_id, QolItemHandle* out_handle) {
    ++g_register_calls;
    if (item == nullptr || out_handle == nullptr || provider_id != kQolItemHandleOwnerGame ||
        !g_register_succeeds) {
        return kQolItemHandleStatusInvalidArgument;
    }
    g_handle_registered = true;
    *out_handle = g_item_handle;
    return kQolItemHandleStatusOk;
}

int32_t fake_lookup_item(void* item, QolItemHandle* out_handle, uint32_t* out_provider_id) {
    ++g_lookup_calls;
    if (item == nullptr || out_handle == nullptr || out_provider_id == nullptr) {
        return kQolItemHandleStatusInvalidArgument;
    }
    if (!g_handle_registered) return kQolItemHandleStatusNotFound;
    *out_handle = g_item_handle;
    *out_provider_id = kQolItemHandleOwnerGame;
    return kQolItemHandleStatusOk;
}

QolResult fake_item_type(QolItemHandle handle, qol::contracts::QolItemType* out_type) {
    ++g_real_equip_calls;
    assert(handle.slot == g_item_handle.slot && handle.generation == g_item_handle.generation);
    if (!g_real_equip_succeeds) return failure();
    if (out_type == nullptr) return failure();
    *out_type = g_real_equip_value != 0 ? qol::contracts::QolItemType::kEquipment
                                        : qol::contracts::QolItemType::kOther;
    return ok();
}

QolResult fake_item_category(QolItemHandle handle, int32_t* out_category) {
    ++g_category_calls;
    assert(handle.slot == g_item_handle.slot && handle.generation == g_item_handle.generation);
    if (!g_category_succeeds) return failure();
    *out_category = g_category_value;
    return ok();
}

QolResult fake_equip_level(const void* item, int32_t* out_level) {
    ++g_equip_level_calls;
    assert(item == &g_item_object);
    if (!g_equip_level_succeeds) return failure();
    *out_level = g_item_level_value;
    return ok();
}

QolResult fake_class_no_equip_bit(int32_t category, int32_t* out_value) {
    ++g_class_no_equip_bit_calls;
    assert(category == g_category_value);
    *out_value = g_no_equip_bit_value;
    return ok();
}

QolResult fake_equip_desc_role_type(int32_t* out_type) {
    ++g_desc_type_calls;
    if (!g_desc_type_succeeds) return failure();
    *out_type = g_desc_type_value;
    return ok();
}

QolResult fake_can_change_equip(const void* character, int32_t* out_allowed) {
    ++g_can_change_calls;
    assert(character == &g_character_object || character == &g_menu_character_object);
    if (!g_can_change_succeeds) return failure();
    *out_allowed = g_can_change_value;
    return ok();
}

QolResult fake_character_level(const void* character, int32_t* out_level) {
    ++g_character_level_calls;
    assert(character == &g_character_object);
    if (!g_character_level_succeeds) return failure();
    *out_level = g_character_level_value;
    return ok();
}

QolResult fake_menu_character(void** out_character) {
    ++g_menu_character_calls;
    if (!g_menu_character_succeeds) return failure();
    *out_character = g_menu_character_present ? &g_menu_character_object : nullptr;
    return ok();
}

QolResult fake_character_type(const void* character, int32_t* out_type) {
    ++g_character_type_calls;
    assert(character == &g_character_object || character == &g_menu_character_object);
    if (!g_character_type_succeeds) return failure();
    *out_type = g_character_type_value;
    return ok();
}

QolResult fake_merc_slot(const void* character, int32_t* out_slot) {
    ++g_merc_slot_calls;
    assert(character == &g_character_object || character == &g_menu_character_object);
    if (!g_merc_slot_succeeds) return failure();
    *out_slot = g_merc_slot_value;
    return ok();
}

int fake_backup_can_equip(void*, void*) {
    ++g_backup_equip_calls;
    return g_original_equip_result;
}

int fake_backup_can_unequip(void*, void*) {
    ++g_backup_unequip_calls;
    return g_original_unequip_result;
}

void reset_read_counts() {
    g_lookup_calls = 0;
    g_register_calls = 0;
    g_real_equip_calls = 0;
    g_category_calls = 0;
    g_equip_level_calls = 0;
    g_class_no_equip_bit_calls = 0;
    g_can_change_calls = 0;
    g_character_level_calls = 0;
    g_menu_character_calls = 0;
    g_character_type_calls = 0;
    g_merc_slot_calls = 0;
    g_desc_type_calls = 0;
}

void assert_no_fact_reads() {
    assert(g_lookup_calls == 0 && g_register_calls == 0);
    assert(g_real_equip_calls == 0 && g_category_calls == 0);
    assert(g_equip_level_calls == 0 && g_class_no_equip_bit_calls == 0);
    assert(g_can_change_calls == 0 && g_character_level_calls == 0);
    assert(g_menu_character_calls == 0 && g_character_type_calls == 0);
    assert(g_merc_slot_calls == 0 && g_desc_type_calls == 0);
}

void setup_fake_interfaces() {
    g_sdk = {};
    g_game = {};
    g_extras = {};
    g_item_facts = {};
    g_character_facts = {};
    g_context = {};
    g_ports[0] = &g_extras;

    g_item_capability_available = true;
    g_character_capability_available = true;
    g_intercept_succeeds = true;
    g_handle_registered = false;
    g_register_succeeds = true;
    g_real_equip_succeeds = true;
    g_category_succeeds = true;
    g_equip_level_succeeds = true;
    g_can_change_succeeds = true;
    g_character_level_succeeds = true;
    g_menu_character_succeeds = true;
    g_character_type_succeeds = true;
    g_merc_slot_succeeds = true;
    g_desc_type_succeeds = true;
    g_debug_enabled = false;
    g_menu_character_present = true;

    g_real_equip_value = 1;
    g_category_value = 485;
    g_item_level_value = 10;
    g_can_change_value = 1;
    g_character_level_value = 20;
    g_character_type_value = 0;
    g_merc_slot_value = 0;
    g_desc_type_value = 0;
    g_no_equip_bit_value = 1;
    g_original_equip_result = 0;
    g_original_unequip_result = 0;

    g_log_writes = 0;
    g_last_log_domain = -1;
    g_last_log_message.clear();
    g_debug_queries = 0;
    g_require_calls = 0;
    g_set_intercept_calls = 0;
    g_clear_intercept_calls = 0;
    g_in_flight_enters = 0;
    g_in_flight_leaves = 0;
    g_intercept_resource_id.clear();
    g_intercept_predicate = nullptr;
    g_intercept_context = nullptr;
    g_intercept_registered = false;
    g_optional_warning_logged = false;
    g_backup_equip_calls = 0;
    g_backup_unequip_calls = 0;
    g_event_subscriptions.clear();
    g_event_subscribe_calls = 0;
    g_event_unsubscribe_calls = 0;
    g_next_event_subscription = 1;
    reset_read_counts();

    g_sdk.log.write = &fake_log_write;
    g_sdk.log.debug_enabled = &fake_debug_enabled;
    g_sdk.capabilities.require = &fake_require;
    g_sdk.events.subscribe = &fake_event_subscribe;
    g_sdk.events.unsubscribe = &fake_event_unsubscribe;
    g_sdk.hooks.set_intercept = &fake_set_intercept;
    g_sdk.hooks.clear_intercept = &fake_clear_intercept;
    g_sdk.in_flight.enter = &fake_in_flight_enter;
    g_sdk.in_flight.leave = &fake_in_flight_leave;
    g_sdk.item_handle.register_item = &fake_register_item;
    g_sdk.item_handle.lookup_item = &fake_lookup_item;

    g_game.read_only.item.equip_level = &fake_equip_level;
    g_game.read_only.item.class_no_equip_bit = &fake_class_no_equip_bit;
    g_game.read_only.item.equip_desc_role_type = &fake_equip_desc_role_type;

    g_extras.abi_version = kQolPortCharacterExtrasVersion;
    g_extras.read.level = &fake_character_level;
    g_extras.read.merc_slot = &fake_merc_slot;
    g_extras.read.can_change_equip = &fake_can_change_equip;
    g_extras.read.menu_character = &fake_menu_character;
    g_extras.read.struct_size = static_cast<uint32_t>(sizeof(QolCharacterExtrasReadApi));

    g_item_facts.struct_size = static_cast<uint32_t>(sizeof(ItemFactsApi));
    g_item_facts.category = &fake_item_category;
    g_item_facts.item_type = &fake_item_type;
    g_character_facts.type = &fake_character_type;

    g_context.struct_size = sizeof(g_context);
    g_context.sdk_major = kHostSdkMajor;
    g_context.sdk = &g_sdk;
    g_context.game = &g_game;
    g_context.plugin_id = "special_equip";
    g_context.ports = g_ports;
    g_context.port_count = 1;

    const auto& descriptor = plugin_descriptor();
    descriptor.on_resource_ready(g_context, kResourceIdCanEquipItem, nullptr);
    descriptor.on_resource_ready(g_context, kResourceIdCanUnequipItem, nullptr);
}

bool status_enabled() {
    PluginStatusOut status{};
    plugin_descriptor().fill_status(status);
    return status.enabled;
}

void test_descriptor() {
    const auto& descriptor = plugin_descriptor();
    assert(std::strcmp(descriptor.id, "special_equip") == 0);
    assert(std::strcmp(descriptor.version, "0.1.0") == 0);
    assert(descriptor.requires_count == 1);
    assert(std::strcmp(descriptor.requires[0].id, kItemFactsCapabilityId) == 0);
    assert(descriptor.optional_count == 1);
    assert(std::strcmp(descriptor.optional_requires[0].id, kCharacterFactsCapabilityId) == 0);

    assert(descriptor.resources_count == 2);
    assert(std::strcmp(descriptor.resources[0].id, kResourceIdCanEquipItem) == 0);
    assert(std::strcmp(descriptor.resources[1].id, kResourceIdCanUnequipItem) == 0);
    assert(descriptor.resources[0].handler == reinterpret_cast<void*>(&can_equip_wrapper));
    assert(descriptor.resources[1].handler == reinterpret_cast<void*>(&can_unequip_wrapper));
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        assert(std::strcmp(descriptor.resources[i].owner, "special_equip") == 0);
        assert(descriptor.resources[i].gate == nullptr);
    }
    assert(descriptor.port_count == 1);
    assert(descriptor.ports[0].id == QolPortId::QOL_PORT_CHARACTER_EXTRAS);
}

void test_install_failures_and_optional_dependency() {
    const auto& descriptor = plugin_descriptor();

    setup_fake_interfaces();
    g_intercept_succeeds = false;
    assert(descriptor.install(g_context) != 0);
    assert(!status_enabled());
    assert(g_set_intercept_calls == 1);
    assert(!g_intercept_registered);
    assert(host_sdk() == nullptr);
    assert(game_host() == nullptr);
    assert(character_extras() == nullptr);

    setup_fake_interfaces();
    g_item_capability_available = false;
    assert(descriptor.install(g_context) != 0);
    assert(!status_enabled());
    assert(g_set_intercept_calls == 0);

    setup_fake_interfaces();
    g_character_capability_available = false;
    assert(descriptor.install(g_context) == 0);
    assert(status_enabled());
    assert(g_intercept_registered);
    assert(g_optional_warning_logged);
    descriptor.deactivate(g_context);
    assert(!status_enabled());
    assert(g_clear_intercept_calls == 1);
    assert(character_extras() == nullptr);
}

void install_success() {
    const auto& descriptor = plugin_descriptor();
    setup_fake_interfaces();
    assert(descriptor.install(g_context) == 0);
    assert(status_enabled());
    assert(g_intercept_registered);
    assert(g_intercept_resource_id == "hook:UIEquip_ButtonDestroyExe");
    assert(g_intercept_predicate != nullptr);
    assert(g_intercept_predicate == &sell_gate_should_block);
    assert(g_intercept_context == nullptr);
}

void test_wrappers_and_sale_predicate() {
    const auto& descriptor = plugin_descriptor();
    install_success();

    descriptor.on_resource_ready(g_context, kResourceIdCanEquipItem, nullptr);
    descriptor.on_resource_ready(g_context, kResourceIdCanUnequipItem, nullptr);
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    assert(can_unequip_wrapper(&g_character_object, &g_item_object) == 0);
    assert(g_backup_equip_calls == 0);
    assert(g_backup_unequip_calls == 0);

    descriptor.on_resource_ready(g_context, kResourceIdCanEquipItem,
                                 reinterpret_cast<void*>(&fake_backup_can_equip));
    descriptor.on_resource_ready(g_context, kResourceIdCanUnequipItem,
                                 reinterpret_cast<void*>(&fake_backup_can_unequip));

    g_original_equip_result = 1;
    g_original_unequip_result = 1;
    reset_read_counts();
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 1);
    assert(can_unequip_wrapper(&g_character_object, &g_item_object) == 1);
    assert_no_fact_reads();

    g_original_equip_result = 0;
    g_original_unequip_result = 0;
    descriptor.on_disabled(g_context);
    reset_read_counts();
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    assert(can_unequip_wrapper(&g_character_object, &g_item_object) == 0);
    assert_no_fact_reads();
    descriptor.on_enabled(g_context);

    g_handle_registered = false;
    g_real_equip_value = 1;
    g_can_change_value = 1;
    g_character_level_value = 20;
    g_item_level_value = 10;
    reset_read_counts();
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 1);
    assert(g_real_equip_calls == 1 && g_equip_level_calls == 1);
    assert(g_can_change_calls == 1 && g_character_level_calls == 1);
    assert(g_register_calls == 1);

    g_real_equip_value = 0;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_real_equip_value = 1;

    g_can_change_value = 0;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_can_change_value = 1;

    g_character_level_value = 9;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_character_level_value = 20;

    g_real_equip_succeeds = false;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_real_equip_succeeds = true;

    g_equip_level_succeeds = false;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_equip_level_succeeds = true;

    g_can_change_succeeds = false;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_can_change_succeeds = true;

    g_character_level_succeeds = false;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_character_level_succeeds = true;

    g_handle_registered = false;
    g_register_succeeds = false;
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    g_register_succeeds = true;

    g_can_change_value = 1;
    assert(can_unequip_wrapper(&g_character_object, &g_item_object) == 1);
    g_can_change_value = 0;
    assert(can_unequip_wrapper(&g_character_object, &g_item_object) == 0);
    g_can_change_value = 1;
    g_can_change_succeeds = false;
    assert(can_unequip_wrapper(&g_character_object, &g_item_object) == 0);
    g_can_change_succeeds = true;

    assert(g_in_flight_enters == g_in_flight_leaves);

    descriptor.on_disabled(g_context);
    const int desc_reads_before = g_desc_type_calls;
    assert(g_intercept_predicate(g_intercept_context) == 0);
    assert(g_desc_type_calls == desc_reads_before);
    descriptor.on_enabled(g_context);

    g_desc_type_value = 0;
    g_can_change_value = 0;
    assert(g_intercept_predicate(g_intercept_context) == 1);
    g_can_change_value = 1;
    assert(g_intercept_predicate(g_intercept_context) == 0);
    g_desc_type_value = 1;
    g_can_change_value = 0;
    assert(g_intercept_predicate(g_intercept_context) == 0);
    g_desc_type_value = 2;
    assert(g_intercept_predicate(g_intercept_context) == 0);
    g_desc_type_value = 3;
    assert(g_intercept_predicate(g_intercept_context) == 1);
    g_desc_type_value = 0;
    g_menu_character_present = false;
    assert(g_intercept_predicate(g_intercept_context) == 1);
    g_menu_character_present = true;
}

void test_reject_diagnostics() {
    const int writes_before = g_log_writes;
    g_debug_enabled = false;
    g_real_equip_value = 0;
    reset_read_counts();
    for (int i = 0; i < 121; ++i) {
        assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    }
    assert(g_log_writes == writes_before);
    assert(g_category_calls == 0 && g_class_no_equip_bit_calls == 0);
    assert(g_character_type_calls == 0 && g_merc_slot_calls == 0);
    assert(g_real_equip_calls == 121 && g_equip_level_calls == 121);
    assert(g_can_change_calls == 121 && g_character_level_calls == 121);

    g_debug_enabled = true;
    reset_read_counts();
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    assert(g_log_writes == writes_before + 1);
    assert(g_last_log_domain == 17);
    assert(g_last_log_message.find("not overridden") != std::string::npos);
    assert(g_real_equip_calls == 1 && g_equip_level_calls == 1);
    assert(g_can_change_calls == 1 && g_character_level_calls == 1);
    assert(g_category_calls == 1 && g_class_no_equip_bit_calls == 1);
    assert(g_character_type_calls == 1 && g_merc_slot_calls == 1);
    g_debug_enabled = false;
}

int reject_character_type_reads() {
    reset_read_counts();
    g_debug_enabled = true;
    g_can_change_value = 0;  // 保证走 log_reject（诊断路径读取 character type）。
    const int result = can_equip_wrapper(&g_character_object, &g_item_object);
    g_debug_enabled = false;
    g_can_change_value = 1;
    assert(result == 0);
    return g_character_type_calls;
}

void test_capability_event_subscription() {
    const auto& descriptor = plugin_descriptor();
    install_success();

    // install 订阅能力变更事件。
    assert(g_event_subscribe_calls == 1);
    assert(g_event_subscriptions.size() == 1);
    assert(g_event_subscriptions[0].type == kEventCapabilityChanged);
    descriptor.on_resource_ready(g_context, kResourceIdCanEquipItem,
                                 reinterpret_cast<void*>(&fake_backup_can_equip));

    // character.facts 撤销：缓存清空，诊断不再读取 character type。
    g_character_capability_available = false;
    const CapabilityChangedEvent unavailable_event{kCharacterFactsCapabilityId,
                                                   kCharacterFactsVersionMajor,
                                                   kCharacterFactsVersionMinor, 0, {}};
    publish_fake_event(kEventCapabilityChanged, &unavailable_event);
    assert(reject_character_type_reads() == 0);

    // 无关能力、空 payload、空 id 均保持幂等。
    const CapabilityChangedEvent other_event{kItemFactsCapabilityId, kItemFactsVersionMajor,
                                             kItemFactsVersionMinor, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &other_event);
    publish_fake_event(kEventCapabilityChanged, nullptr);
    const CapabilityChangedEvent null_id_event{nullptr, 1, 0, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &null_id_event);
    assert(reject_character_type_reads() == 0);

    // character.facts 恢复：事件处理器重新 require 并重新设置缓存。
    g_character_capability_available = true;
    const int requires_before = g_require_calls;
    const CapabilityChangedEvent available_event{kCharacterFactsCapabilityId,
                                                 kCharacterFactsVersionMajor,
                                                 kCharacterFactsVersionMinor, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &available_event);
    assert(g_require_calls == requires_before + 1);
    assert(reject_character_type_reads() == 1);

    // deactivate 取消事件订阅。
    const int unsubscribes_before = g_event_unsubscribe_calls;
    descriptor.deactivate(g_context);
    assert(g_event_unsubscribe_calls == unsubscribes_before + 1);
    assert(g_event_subscriptions.empty());

    setup_fake_interfaces();
}

void test_lifecycle_cleanup() {
    const auto& descriptor = plugin_descriptor();
    const int clears_before = g_clear_intercept_calls;
    descriptor.deactivate(g_context);
    assert(!status_enabled());
    assert(character_extras() == nullptr);
    assert(!deps_ready());
    assert(g_clear_intercept_calls == clears_before + 1);
    assert(!g_intercept_registered);

    descriptor.on_enabled(g_context);
    g_debug_enabled = true;
    reset_read_counts();
    assert(can_equip_wrapper(&g_character_object, &g_item_object) == 0);
    assert(g_category_calls == 0 && g_character_type_calls == 0);
    g_debug_enabled = false;
    descriptor.on_disabled(g_context);

    descriptor.on_resource_ready(g_context, kResourceIdCanEquipItem, nullptr);
    descriptor.on_resource_ready(g_context, kResourceIdCanUnequipItem, nullptr);
    setup_fake_interfaces();
}

}  // namespace

int main() {
    test_descriptor();
    test_install_failures_and_optional_dependency();
    test_wrappers_and_sale_predicate();
    test_reject_diagnostics();
    test_capability_event_subscription();
    test_lifecycle_cleanup();
    return 0;
}
