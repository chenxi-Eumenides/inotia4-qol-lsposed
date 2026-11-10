#include "contracts/character_facts.h"
#include "contracts/game_host.h"
#include "contracts/game_ports/character_extras_port.h"
#include "contracts/plugin.h"
#include "contracts/host_sdk.h"
#include "plugins/world_tuning/feature.h"
#include "plugins/world_tuning/panel.h"
#include "plugins/world_tuning/plugin.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qol::contracts;
using namespace qol::features::world_tuning;

struct Character {
    int32_t type = 1;
    int32_t party_member = 0;
    int32_t active_group = 0;
    Character* summoner = nullptr;
    int32_t slot = 0;
    int32_t max_hp = 100;
    int32_t hp = 100;
};

QolHostSdk g_sdk{};
QolGameHost g_game{};
QolCharacterExtrasPort g_extras{};
CharacterFactsApi g_facts{};
PluginContext g_context{};
const void* g_ports[] = {nullptr};
Character g_characters[16]{};

bool g_require_succeeds = true;
bool g_require_returns_null = false;
bool g_fail_party = false;
bool g_fail_active = false;
bool g_fail_summoner = false;
bool g_fail_type = false;
bool g_fail_pool_slot = false;
bool g_fail_max_hp = false;
bool g_fail_hp = false;
int g_require_calls = 0;
int g_party_calls = 0;
int g_active_calls = 0;
int g_summoner_calls = 0;
int g_type_calls = 0;
int g_pool_slot_calls = 0;
int g_max_hp_reads = 0;
int g_hp_reads = 0;
int g_max_hp_writes = 0;
int g_hp_writes = 0;
int g_damage_backup_calls = 0;
int g_attr_backup_calls = 0;
int g_exp_backup_calls = 0;
int g_money_backup_calls = 0;
int32_t g_last_exp = 0;
int64_t g_last_money = 0;
int g_in_flight_enters = 0;
int g_in_flight_leaves = 0;
int g_damage_enters = 0;
int g_damage_leaves = 0;
int g_attr_enters = 0;
int g_attr_leaves = 0;
int g_log_writes = 0;
int32_t g_last_damage = 0;
std::vector<std::string> g_log_messages;

QolResult fake_failure() { return error(kQolErrNotReady, "fake failure", 12); }

QolResult fake_log_write(int32_t, int32_t, const char*, const char* message,
                         uint32_t message_len) {
    ++g_log_writes;
    g_log_messages.emplace_back(message == nullptr ? "" : std::string(message, message_len));
    return ok();
}

// ---- item.use fake ----
ItemUseApi g_item_use_api{};
bool g_item_use_available = false;
int g_item_use_subscribe_calls = 0;
int g_item_use_unsubscribe_calls = 0;
uint64_t g_item_use_subscription = 0;

// ---- ui_panels / ui_components fake ----
UiPanelsApi g_ui_panels_api{};
QolUiComponents g_ui_components_api{};
QolUiAdapter g_ui_adapter{};
bool g_ui_available = false;
int g_panel_register_calls = 0;
int g_panel_unregister_calls = 0;
const PanelProviderSpec* g_registered_panel = nullptr;

QolResult fake_panels_register_provider(const PanelProviderSpec* provider) {
    ++g_panel_register_calls;
    g_registered_panel = provider;
    return ok();
}

QolResult fake_panels_unregister_owner(const char* owner) {
    ++g_panel_unregister_calls;
    if (owner != nullptr && std::strcmp(owner, "world_tuning") == 0) {
        g_registered_panel = nullptr;
    }
    return ok();
}

QolResult fake_ui_panel_create(const UiPanelDesc*, QolUiHandle, QolUiHandle*) { return ok(); }
QolResult fake_ui_arrow_button_create(const UiArrowButtonDesc*, QolUiHandle, QolUiHandle*) {
    return ok();
}
QolResult fake_ui_nav_button_create(const UiNavButtonDesc*, QolUiHandle, QolUiHandle*) {
    return ok();
}
QolResult fake_ui_handle_ok(QolUiHandle) { return ok(); }
QolResult fake_ui_handle_enabled(QolUiHandle, uint8_t) { return ok(); }
QolResult fake_ui_draw_text(QolUiHandle, const char*, int32_t, int32_t, uint32_t, int32_t,
                            int32_t) {
    return ok();
}
QolResult fake_ui_container_styled(const UiContainerStyleDesc*) { return ok(); }
QolResult fake_ui_dispatch_native_event(QolUiHandle, uint64_t, uint64_t, uint64_t, uint32_t*) {
    return ok();
}
QolResult fake_ui_ok() { return ok(); }
QolResult fake_ui_origin(QolUiHandle, int64_t*, int64_t*) { return ok(); }
QolResult fake_ui_fill_rect(UiRect, uint32_t) { return ok(); }
QolResult fake_ui_touch_coordinates(uint64_t, int64_t*, int64_t*) { return ok(); }
int32_t fake_ui_hit_test(QolUiHandle, int64_t, int64_t, UiRect) { return 1; }

QolResult fake_require(const char* id, QolVersionRange, const void** out_iface) {
    ++g_require_calls;
    assert(id != nullptr && out_iface != nullptr);
    if (!g_require_succeeds) {
        *out_iface = nullptr;
        return fake_failure();
    }
    if (std::strcmp(id, kItemUseCapabilityId) == 0) {
        if (!g_item_use_available) {
            *out_iface = nullptr;
            return fake_failure();
        }
        *out_iface = &g_item_use_api;
        return ok();
    }
    if (std::strcmp(id, kUiPanelsCapabilityId) == 0 ||
        std::strcmp(id, kUiComponentsCapabilityId) == 0) {
        if (!g_ui_available) {
            *out_iface = nullptr;
            return fake_failure();
        }
        *out_iface = std::strcmp(id, kUiPanelsCapabilityId) == 0
                         ? static_cast<const void*>(&g_ui_panels_api)
                         : static_cast<const void*>(&g_ui_components_api);
        return ok();
    }
    *out_iface = g_require_returns_null ? nullptr : &g_facts;
    return ok();
}

// ---- 事件总线 fake：记录订阅，支持按事件类型注入 payload ----
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
    // 与 infra 一致：按快照派发，处理器内注销不影响本轮。
    const std::vector<FakeEventSubscription> snapshot = g_event_subscriptions;
    for (const FakeEventSubscription& subscription : snapshot) {
        if (subscription.type == type) subscription.handler(subscription.ctx, payload);
    }
}

// ---- item.use fake ----
QolResult fake_item_use_subscribe(const char*, QolItemUsePostFn fn, void*, uint64_t* out) {
    ++g_item_use_subscribe_calls;
    assert(fn != nullptr && out != nullptr);
    g_item_use_subscription = 1;
    *out = g_item_use_subscription;
    return ok();
}

QolResult fake_item_use_unsubscribe(uint64_t subscription) {
    ++g_item_use_unsubscribe_calls;
    if (subscription != g_item_use_subscription) return error(kQolErrNotFound, "not found", 9);
    g_item_use_subscription = 0;
    return ok();
}

QolResult fake_enter(InFlightHandle handle) {
    assert(handle.resource_id != nullptr);
    ++g_in_flight_enters;
    if (std::strcmp(handle.resource_id, kResourceIdAddDamage) == 0) {
        ++g_damage_enters;
    } else if (std::strcmp(handle.resource_id, kResourceIdUpdateAttrFromMonster) == 0) {
        ++g_attr_enters;
    } else if (std::strcmp(handle.resource_id, kResourceIdAddExperience) == 0) {
        // exp/gold 也在途计数，归入 total 即可。
    } else if (std::strcmp(handle.resource_id, kResourceIdAddMoney) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdDropItem) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdMakeItem) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdMakeEquipDetail) == 0) {
    } else {
        assert(false && "unexpected in-flight resource id");
    }
    return ok();
}

QolResult fake_leave(InFlightHandle handle) {
    assert(handle.resource_id != nullptr);
    ++g_in_flight_leaves;
    if (std::strcmp(handle.resource_id, kResourceIdAddDamage) == 0) {
        ++g_damage_leaves;
    } else if (std::strcmp(handle.resource_id, kResourceIdUpdateAttrFromMonster) == 0) {
        ++g_attr_leaves;
    } else if (std::strcmp(handle.resource_id, kResourceIdAddExperience) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdAddMoney) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdDropItem) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdMakeItem) == 0) {
    } else if (std::strcmp(handle.resource_id, kResourceIdMakeEquipDetail) == 0) {
    } else {
        assert(false && "unexpected in-flight resource id");
    }
    return ok();
}

QolResult fake_party_member(const void* character, int32_t* out_member) {
    ++g_party_calls;
    if (g_fail_party) return fake_failure();
    assert(character != nullptr && out_member != nullptr);
    *out_member = static_cast<const Character*>(character)->party_member;
    return ok();
}

QolResult fake_active_group(const void* character, int32_t* out_active) {
    ++g_active_calls;
    if (g_fail_active) return fake_failure();
    assert(character != nullptr && out_active != nullptr);
    *out_active = static_cast<const Character*>(character)->active_group;
    return ok();
}

QolResult fake_summoner(const void* character, void** out_summoner) {
    ++g_summoner_calls;
    if (g_fail_summoner) return fake_failure();
    assert(character != nullptr && out_summoner != nullptr);
    *out_summoner = static_cast<const Character*>(character)->summoner;
    return ok();
}

QolResult fake_type(const void* character, int32_t* out_type) {
    ++g_type_calls;
    if (g_fail_type) return fake_failure();
    assert(character != nullptr && out_type != nullptr);
    *out_type = static_cast<const Character*>(character)->type;
    return ok();
}

QolResult fake_pool_slot(const void* character, int32_t* out_slot) {
    ++g_pool_slot_calls;
    if (g_fail_pool_slot) return fake_failure();
    assert(character != nullptr && out_slot != nullptr);
    *out_slot = static_cast<const Character*>(character)->slot;
    return ok();
}

QolResult fake_max_hp(const void* character, int32_t* out_value) {
    ++g_max_hp_reads;
    if (g_fail_max_hp) return fake_failure();
    assert(character != nullptr && out_value != nullptr);
    *out_value = static_cast<const Character*>(character)->max_hp;
    return ok();
}

QolResult fake_hp(const void* character, int32_t* out_value) {
    ++g_hp_reads;
    if (g_fail_hp) return fake_failure();
    assert(character != nullptr && out_value != nullptr);
    *out_value = static_cast<const Character*>(character)->hp;
    return ok();
}

QolResult fake_write_max_hp(void* character, int32_t value) {
    assert(character != nullptr);
    ++g_max_hp_writes;
    static_cast<Character*>(character)->max_hp = value;
    return ok();
}

QolResult fake_write_hp(void* character, int32_t value) {
    assert(character != nullptr);
    ++g_hp_writes;
    static_cast<Character*>(character)->hp = value;
    return ok();
}

void fake_damage_backup(void*, void*, int32_t damage, int32_t, int32_t) {
    ++g_damage_backup_calls;
    g_last_damage = damage;
}

void fake_attr_backup(void*, int32_t) { ++g_attr_backup_calls; }

int32_t fake_exp_backup(void*, int32_t amount, uint8_t) {
    ++g_exp_backup_calls;
    g_last_exp = amount;
    return 1;
}

int32_t fake_money_backup(int64_t amount) {
    ++g_money_backup_calls;
    g_last_money = amount;
    return 1;
}


// ---- 掉落域 fake：ITEM* 用 FakeDropItem 承载类别/等级/稀有度，free 只记录次数 ----
struct FakeDropItem {
    int32_t category = 0;
    int32_t level = 0;
    int32_t rarity = 0;
    int32_t freed = 0;
};
struct FakeItemSpec {
    int32_t category = 0;
    int32_t level = 0;
    int32_t rarity = 0;
};
FakeDropItem g_fake_items[64]{};
int g_fake_item_next = 0;
FakeItemSpec g_make_item_specs[8]{};
int g_make_item_spec_count = 0;
int g_make_item_spec_index = 0;
FakeItemSpec g_equip_specs[8]{};
int g_equip_spec_count = 0;
int g_equip_spec_index = 0;
int g_drop_item_calls = 0;
int g_make_item_calls = 0;
int g_equip_detail_calls = 0;
int g_free_item_calls = 0;
FakeDropItem* g_last_item = nullptr;
void (*g_drop_invoke)() = nullptr;
int32_t g_drop_threshold_result = 0;

void* next_fake_item(const FakeItemSpec& spec) {
    FakeDropItem& item = g_fake_items[g_fake_item_next++ % 64];
    item = FakeDropItem{};
    item.category = spec.category;
    item.level = spec.level;
    item.rarity = spec.rarity;
    return &item;
}

void fake_drop_item_backup(void*) {
    ++g_drop_item_calls;
    if (g_drop_invoke != nullptr) g_drop_invoke();
}

void* fake_make_item_backup(int32_t, int32_t, int32_t) {
    ++g_make_item_calls;
    int index = g_make_item_spec_index < g_make_item_spec_count
                    ? g_make_item_spec_index
                    : (g_make_item_spec_count > 0 ? g_make_item_spec_count - 1 : 0);
    ++g_make_item_spec_index;
    FakeItemSpec spec{};
    if (g_make_item_spec_count > 0) spec = g_make_item_specs[index];
    return next_fake_item(spec);
}

void* fake_equip_detail_backup(int32_t, int32_t, int32_t, int32_t, int32_t) {
    ++g_equip_detail_calls;
    int index = g_equip_spec_index < g_equip_spec_count
                    ? g_equip_spec_index
                    : (g_equip_spec_count > 0 ? g_equip_spec_count - 1 : 0);
    ++g_equip_spec_index;
    FakeItemSpec spec{};
    if (g_equip_spec_count > 0) spec = g_equip_specs[index];
    return next_fake_item(spec);
}

QolResult fake_item_category(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) return error(kQolErrInvalidArgument, "bad", 3);
    *out = static_cast<const FakeDropItem*>(item)->category;
    return ok();
}

QolResult fake_item_level(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) return error(kQolErrInvalidArgument, "bad", 3);
    *out = static_cast<const FakeDropItem*>(item)->level;
    return ok();
}

QolResult fake_item_rarity(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) return error(kQolErrInvalidArgument, "bad", 3);
    *out = static_cast<const FakeDropItem*>(item)->rarity;
    return ok();
}

QolResult fake_free_item(void* item) {
    ++g_free_item_calls;
    if (item != nullptr) static_cast<FakeDropItem*>(item)->freed = 1;
    return ok();
}

QolResult fake_predicate(int32_t category, int32_t* out, int32_t expected) {
    if (out == nullptr) return error(kQolErrInvalidArgument, "bad", 3);
    *out = category == expected ? 1 : 0;
    return ok();
}

QolResult fake_is_scroll(int32_t category, int32_t* out) {
    return fake_predicate(category, out, 24);
}
QolResult fake_is_bag(int32_t category, int32_t* out) {
    return fake_predicate(category, out, 31);
}
QolResult fake_is_dice(int32_t category, int32_t* out) {
    return fake_predicate(category, out, 32);
}

void reset_counters() {
    g_require_calls = 0;
    g_party_calls = 0;
    g_active_calls = 0;
    g_summoner_calls = 0;
    g_type_calls = 0;
    g_pool_slot_calls = 0;
    g_max_hp_reads = 0;
    g_hp_reads = 0;
    g_max_hp_writes = 0;
    g_hp_writes = 0;
    g_damage_backup_calls = 0;
    g_attr_backup_calls = 0;
    g_exp_backup_calls = 0;
    g_money_backup_calls = 0;
    g_drop_item_calls = 0;
    g_make_item_calls = 0;
    g_equip_detail_calls = 0;
    g_free_item_calls = 0;
    g_make_item_spec_count = 0;
    g_make_item_spec_index = 0;
    g_equip_spec_count = 0;
    g_equip_spec_index = 0;
    g_fake_item_next = 0;
    g_last_item = nullptr;
    g_drop_invoke = nullptr;
    g_drop_threshold_result = 0;
    g_last_exp = 0;
    g_last_money = 0;
    g_in_flight_enters = 0;
    g_in_flight_leaves = 0;
    g_damage_enters = 0;
    g_damage_leaves = 0;
    g_attr_enters = 0;
    g_attr_leaves = 0;
    g_log_writes = 0;
    g_last_damage = 0;
    g_log_messages.clear();
    g_event_subscriptions.clear();
    g_event_subscribe_calls = 0;
    g_event_unsubscribe_calls = 0;
    g_next_event_subscription = 1;
    g_item_use_available = false;
    g_item_use_subscribe_calls = 0;
    g_item_use_unsubscribe_calls = 0;
    g_item_use_subscription = 0;
    g_ui_available = false;
    g_panel_register_calls = 0;
    g_panel_unregister_calls = 0;
    g_registered_panel = nullptr;
}

void setup_fake_interfaces() {
    g_sdk = {};
    g_game = {};
    g_extras = {};
    g_facts = {};
    g_context = {};
    for (auto& character : g_characters) character = Character{};
    g_ports[0] = &g_extras;

    g_require_succeeds = true;
    g_require_returns_null = false;
    g_fail_party = false;
    g_fail_active = false;
    g_fail_summoner = false;
    g_fail_type = false;
    g_fail_pool_slot = false;
    g_fail_max_hp = false;
    g_fail_hp = false;
    reset_counters();

    g_sdk.log.write = &fake_log_write;
    g_sdk.capabilities.require = &fake_require;
    g_sdk.events.subscribe = &fake_event_subscribe;
    g_sdk.events.unsubscribe = &fake_event_unsubscribe;
    g_sdk.in_flight.enter = &fake_enter;
    g_sdk.in_flight.leave = &fake_leave;

    g_ui_panels_api = {};
    g_ui_components_api = {};
    g_ui_adapter = {};
    g_ui_components_api.panel_create = &fake_ui_panel_create;
    g_ui_components_api.arrow_button_create = &fake_ui_arrow_button_create;
    g_ui_components_api.arrow_button_draw = &fake_ui_handle_ok;
    g_ui_components_api.arrow_button_set_enabled = &fake_ui_handle_enabled;
    g_ui_components_api.nav_button_create = &fake_ui_nav_button_create;
    g_ui_components_api.nav_button_draw = &fake_ui_handle_ok;
    g_ui_components_api.destroy = &fake_ui_handle_ok;
    g_ui_components_api.draw_control_text = &fake_ui_draw_text;
    g_ui_components_api.draw_container_styled = &fake_ui_container_styled;
    g_ui_components_api.dispatch_native_event = &fake_ui_dispatch_native_event;
    g_ui_adapter.begin_frame = &fake_ui_ok;
    g_ui_adapter.end_frame = &fake_ui_ok;
    g_ui_adapter.delete_control = &fake_ui_handle_ok;
    g_ui_adapter.control_absolute_origin = &fake_ui_origin;
    g_ui_adapter.fill_rect = &fake_ui_fill_rect;
    g_ui_adapter.touch_coordinates = &fake_ui_touch_coordinates;
    g_ui_adapter.hit_test = &fake_ui_hit_test;

    g_sdk.panels.register_provider = &fake_panels_register_provider;
    g_sdk.panels.unregister_owner = &fake_panels_unregister_owner;
    g_sdk.ui = &g_ui_adapter;

    g_item_use_api = {};
    g_item_use_api.struct_size = sizeof(ItemUseApi);
    g_item_use_api.subscribe_post = &fake_item_use_subscribe;
    g_item_use_api.unsubscribe_post = &fake_item_use_unsubscribe;

    g_extras.read.is_party_member = &fake_party_member;
    g_extras.read.summoner = &fake_summoner;
    g_extras.read.pool_slot = &fake_pool_slot;
    g_extras.read.max_hp = &fake_max_hp;
    g_extras.read.hp = &fake_hp;

    g_game.abi = {sizeof(QolGameHost), kGameHostMajor, kGameHostMinor};
    g_game.character_write.max_hp = &fake_write_max_hp;
    g_game.character_write.hp = &fake_write_hp;
    g_game.item_category.category = &fake_item_category;
    g_game.read_only.item.equip_level = &fake_item_level;
    g_game.read_only.item.rarity = &fake_item_rarity;
    g_game.read_only.item.is_enchant_scroll = &fake_is_scroll;
    g_game.read_only.item.is_backpack_category = &fake_is_bag;
    g_game.read_only.item.is_dice = &fake_is_dice;
    g_game.inventory_write.free_item = &fake_free_item;

    g_facts.type = &fake_type;
    g_facts.is_active_player_group = &fake_active_group;

    g_context.struct_size = sizeof(g_context);
    g_context.sdk_major = kHostSdkMajor;
    g_context.sdk_minor = kHostSdkMinor;
    g_context.sdk = &g_sdk;
    g_context.game = &g_game;
    g_context.plugin_id = "world_tuning";
    g_context.ports = g_ports;
    g_context.port_count = 1;

    set_gate(false);
    const auto& descriptor = plugin_descriptor();
    descriptor.on_resource_ready(g_context, kResourceIdAddDamage, nullptr);
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster, nullptr);
}

void install_success() {
    setup_fake_interfaces();
    const auto& descriptor = plugin_descriptor();
    assert(descriptor.install(g_context) == 0);
    assert(!gate_enabled());
    assert(host_sdk() == &g_sdk);
    assert(game_host() == &g_game);
    assert(character_extras() == &g_extras);
    assert(character_facts() == &g_facts);
    descriptor.on_resource_ready(g_context, kResourceIdAddDamage,
                                 reinterpret_cast<void*>(&fake_damage_backup));
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster,
                                 reinterpret_cast<void*>(&fake_attr_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddExperience,
                                 reinterpret_cast<void*>(&fake_exp_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddMoney,
                                 reinterpret_cast<void*>(&fake_money_backup));
    descriptor.on_resource_ready(g_context, kResourceIdDropItem,
                                 reinterpret_cast<void*>(&fake_drop_item_backup));
    descriptor.on_resource_ready(g_context, kResourceIdMakeItem,
                                 reinterpret_cast<void*>(&fake_make_item_backup));
    descriptor.on_resource_ready(g_context, kResourceIdMakeEquipDetail,
                                 reinterpret_cast<void*>(&fake_equip_detail_backup));
}

template <typename Invoke>
void assert_one_damage_flight(Invoke&& invoke) {
    const int enters = g_damage_enters;
    const int leaves = g_damage_leaves;
    invoke();
    assert(g_damage_enters == enters + 1);
    assert(g_damage_leaves == leaves + 1);
}

template <typename Invoke>
void assert_one_attr_flight(Invoke&& invoke) {
    const int enters = g_attr_enters;
    const int leaves = g_attr_leaves;
    invoke();
    assert(g_attr_enters == enters + 1);
    assert(g_attr_leaves == leaves + 1);
}

void test_descriptor() {
    const auto& descriptor = plugin_descriptor();
    assert(std::strcmp(descriptor.id, "world_tuning") == 0);
    assert(std::strcmp(descriptor.version, "0.1.0") == 0);
    assert(descriptor.requires_count == 1);
    assert(std::strcmp(descriptor.requires[0].id, kCharacterFactsCapabilityId) == 0);
    assert(descriptor.optional_count == 3);
    assert(descriptor.validate_config == &validate_rates_config);
    assert(descriptor.resources_count == 12);
    assert(descriptor.provides_count == 0);
    assert(descriptor.provides == nullptr);
    assert(std::strcmp(descriptor.resources[0].id, kResourceIdAddDamage) == 0);
    assert(std::strcmp(descriptor.resources[0].symbol, "F_CHAR_ADD_DAMAGE_VMA") == 0);
    assert(descriptor.resources[0].handler == reinterpret_cast<void*>(&char_add_damage_wrapper));
    assert(std::strcmp(descriptor.resources[1].id, kResourceIdUpdateAttrFromMonster) == 0);
    assert(std::strcmp(descriptor.resources[1].symbol,
                       "F_CHAR_UPDATE_ATTR_FROM_MONSTER_VMA") == 0);
    assert(descriptor.resources[1].handler ==
           reinterpret_cast<void*>(&char_update_attr_from_monster_wrapper));
    assert(std::strcmp(descriptor.resources[2].id, kResourceIdAddExperience) == 0);
    assert(std::strcmp(descriptor.resources[2].symbol, "F_ADD_EXP_VMA") == 0);
    assert(std::strcmp(descriptor.resources[3].id, kResourceIdAddMoney) == 0);
    assert(std::strcmp(descriptor.resources[3].symbol, "F_ADD_MONEY_VMA") == 0);
    assert(std::strcmp(descriptor.resources[4].id, kResourceIdDropItem) == 0);
    assert(std::strcmp(descriptor.resources[4].symbol, "F_CHARSYSTEM_DROP_ITEM_VMA") == 0);
    assert(descriptor.resources[4].handler == reinterpret_cast<void*>(&drop_item_wrapper));
    assert(std::strcmp(descriptor.resources[5].id, kResourceIdMakeItem) == 0);
    assert(std::strcmp(descriptor.resources[5].symbol, "F_MAKE_ITEM_VMA") == 0);
    assert(descriptor.resources[5].handler == reinterpret_cast<void*>(&make_item_wrapper));
    assert(std::strcmp(descriptor.resources[6].id, kResourceIdMakeEquipDetail) == 0);
    assert(std::strcmp(descriptor.resources[6].symbol, "F_MAKE_EQUIP_DETAIL_VMA") == 0);
    assert(descriptor.resources[6].handler ==
           reinterpret_cast<void*>(&make_equip_detail_wrapper));
    assert(std::strcmp(descriptor.resources[7].id, kResourceIdGetActionCoolTime) == 0);
    assert(std::strcmp(descriptor.resources[7].symbol, "F_CHAR_GET_ACTION_COOLTIME_VMA") == 0);
    assert(descriptor.resources[7].handler ==
           reinterpret_cast<void*>(&char_get_action_cooltime_wrapper));
    assert(descriptor.resources[7].follow_trampoline);
    assert(descriptor.resources[7].short_hook);
    const char* const drop_patch_ids[] = {
        kResourceIdDropReroll258, kResourceIdDropReroll3B8,
        kResourceIdDropReroll400, kResourceIdDropReroll460,
    };
    const char* const drop_patch_sites[] = {
        "INSN_DROP_REROLL_258", "INSN_DROP_REROLL_3B8",
        "INSN_DROP_REROLL_400", "INSN_DROP_REROLL_460",
    };
    for (uint32_t i = 0; i < 4; ++i) {
        const ResourceClaim& claim = descriptor.resources[8 + i];
        assert(std::strcmp(claim.id, drop_patch_ids[i]) == 0);
        assert(std::strcmp(claim.symbol, drop_patch_sites[i]) == 0);
        assert(claim.mechanism == ResourceMechanism::kInsnPatch);
        assert(claim.requirement == ResourceRequirement::kOptional);
        assert(claim.failure == ResourceFailure::kFailSoft);
        assert(claim.handler == reinterpret_cast<void*>(&drop_threshold_patch_helper));
    }
    setup_fake_interfaces();
    const size_t log_count = g_log_messages.size();
    for (const char* id : drop_patch_ids) {
        assert(descriptor.on_resource_ready(g_context, id,
                                            reinterpret_cast<void*>(uintptr_t{0x6b00039f})) == 0);
    }
    for (size_t i = log_count; i < g_log_messages.size(); ++i) {
        assert(g_log_messages[i].find("unknown resource id") == std::string::npos);
    }
    for (uint32_t i = 0; i < descriptor.resources_count; ++i) {
        assert(descriptor.resources[i].requirement == ResourceRequirement::kOptional);
        assert(descriptor.resources[i].failure == ResourceFailure::kFailSoft);
    }
    assert(descriptor.jni_count == 0);
    assert(descriptor.jni == nullptr);
    assert(descriptor.feature_count == 1);
    assert(std::strcmp(descriptor.features[0].id, "world_tuning.panel") == 0);
    assert(descriptor.features[0].type == FeatureType::kEntry);
    assert(descriptor.features[0].default_enabled == 0);
    assert(descriptor.features[0].on_entry != nullptr);
    assert(descriptor.features[0].action_label != nullptr);
}

void assert_install_clean() {
    assert(!gate_enabled());
    assert(host_sdk() == nullptr);
    assert(game_host() == nullptr);
    assert(character_extras() == nullptr);
    assert(character_facts() == nullptr);
}

void test_install_lifecycle_and_toggle() {
    const auto& descriptor = plugin_descriptor();

    setup_fake_interfaces();
    g_context.port_count = 0;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    setup_fake_interfaces();
    g_require_succeeds = false;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    setup_fake_interfaces();
    g_sdk.capabilities.require = nullptr;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    setup_fake_interfaces();
    g_require_returns_null = true;
    assert(descriptor.install(g_context) != 0);
    assert_install_clean();

    install_success();
    assert(!gate_enabled());
    assert(character_facts() == &g_facts && character_extras() == &g_extras);
    descriptor.on_enabled(g_context);
    assert(gate_enabled());
    descriptor.on_disabled(g_context);
    assert(!gate_enabled());
    descriptor.on_enabled(g_context);
    assert(gate_enabled());
    // 面板未绑定（fake SDK 无 UI 能力）时入口返回 NotReady，但不影响门禁。
    assert(descriptor.features[0].on_entry(g_context).ok == kQolErrNotReady);
    assert(gate_enabled());

    descriptor.on_disabled(g_context);
    assert(descriptor.features[0].on_entry(g_context).ok == kQolErrNotReady);
    descriptor.deactivate(g_context);
    assert(!gate_enabled());
    assert(character_extras() == nullptr);
    assert(character_facts() == nullptr);
}

void test_damage_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    Character& attacker = g_characters[0];
    Character& victim = g_characters[1];
    attacker.party_member = 1;
    victim.type = 1;
    victim.slot = 1;

    descriptor.on_disabled(g_context);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&attacker, &victim, 100, 3, 4);
    });
    assert(g_last_damage == 100);
    assert(g_party_calls == 0);

    descriptor.on_enabled(g_context);
    {
        Rates rates = default_rates();
        rates.value[static_cast<uint32_t>(RateId::kDamageOut)] = 150;
        rates.value[static_cast<uint32_t>(RateId::kDamageIn)] = 50;
        apply_rates(rates);
    }
    g_party_calls = g_active_calls = g_summoner_calls = 0;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&attacker, &victim, 100, 3, 4);
    });
    assert(g_last_damage == 150);
    // The attacker is already identified by party_member; later faction primitives are skipped.
    assert(g_active_calls == 1 && g_summoner_calls == 1);

    Character& monster = g_characters[2];
    Character& player = g_characters[3];
    monster.type = 1;
    player.party_member = 1;
    g_party_calls = g_active_calls = g_summoner_calls = 0;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&monster, &player, 100, 0, 0);
    });
    assert(g_last_damage == 50);
    assert(g_party_calls == 1 && g_active_calls == 0 && g_summoner_calls == 0);

    {
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kDamageOut)] = 0;
        rates.value[static_cast<uint32_t>(RateId::kDamageIn)] = 0;
        apply_rates(rates);
    }
    assert_one_damage_flight([&] { char_add_damage_wrapper(&attacker, &victim, 100, 0, 0); });
    assert(g_last_damage == 0);
    assert_one_damage_flight([&] { char_add_damage_wrapper(&monster, &player, 100, 0, 0); });
    assert(g_last_damage == 0);
    {
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kDamageOut)] = 150;
        rates.value[static_cast<uint32_t>(RateId::kDamageIn)] = 50;
        apply_rates(rates);
    }

    Character& neutral = g_characters[4];
    neutral.type = 2;
    Character& player_victim = g_characters[5];
    player_victim.party_member = 1;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&neutral, &player_victim, 100, 0, 0);
    });
    assert(g_last_damage == 50);

    Character& active_player_victim = g_characters[15];
    active_player_victim.active_group = 1;
    g_party_calls = g_active_calls = g_summoner_calls = 0;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&neutral, &active_player_victim, 100, 0, 0);
    });
    assert(g_last_damage == 50);
    assert(g_party_calls == 1 && g_active_calls == 1 && g_summoner_calls == 0);

    Character& player_attacker = g_characters[6];
    player_attacker.party_member = 1;
    Character& neutral_victim = g_characters[7];
    neutral_victim.type = 2;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &neutral_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);

    Character& unknown_victim = g_characters[8];
    unknown_victim.type = 1;
    g_fail_party = true;
    const auto faction_warning_count = [&] {
        int count = 0;
        for (const auto& message : g_log_messages) {
            if (message.find("faction dependencies unavailable") != std::string::npos) ++count;
        }
        return count;
    };
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(faction_warning_count() == 1);
    g_fail_party = false;

    g_fail_active = true;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    g_fail_active = false;

    g_fail_summoner = true;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    g_fail_summoner = false;

    unknown_victim.summoner = &unknown_victim;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    unknown_victim.summoner = nullptr;

    for (int i = 8; i < 13; ++i) g_characters[i].summoner = &g_characters[i + 1];
    g_characters[13].summoner = &player_attacker;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    for (int i = 8; i < 14; ++i) g_characters[i].summoner = nullptr;

    const auto active_group_fn = g_facts.is_active_player_group;
    g_facts.is_active_player_group = nullptr;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    g_facts.is_active_player_group = active_group_fn;

    descriptor.on_resource_ready(g_context, kResourceIdAddDamage, nullptr);
    const int reads_before_null_backup = g_party_calls + g_active_calls + g_summoner_calls;
    const int backups_before_null_backup = g_damage_backup_calls;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 100, 0, 0);
    });
    assert(g_damage_backup_calls == backups_before_null_backup);
    assert(g_party_calls + g_active_calls + g_summoner_calls == reads_before_null_backup);
    descriptor.on_resource_ready(g_context, kResourceIdAddDamage,
                                 reinterpret_cast<void*>(&fake_damage_backup));

    const int reads_before_nonpositive = g_party_calls + g_active_calls + g_summoner_calls;
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, 0, 0, 0);
    });
    assert(g_last_damage == 0);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&player_attacker, &unknown_victim, -7, 0, 0);
    });
    assert(g_last_damage == -7);
    assert(g_party_calls + g_active_calls + g_summoner_calls == reads_before_nonpositive);

    // 倍率 100（×1.0）必须恒等；配置 10/100 只让玩家侧缩小 10 倍。
    apply_rates(default_rates());
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&attacker, &victim, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&monster, &player, 100, 0, 0);
    });
    assert(g_last_damage == 100);
    {
        Rates rates = default_rates();
        rates.value[static_cast<uint32_t>(RateId::kDamageOut)] = 10;
        rates.value[static_cast<uint32_t>(RateId::kDamageIn)] = 100;
        apply_rates(rates);
    }
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&attacker, &victim, 100, 0, 0);
    });
    assert(g_last_damage == 10);
    assert_one_damage_flight([&] {
        char_add_damage_wrapper(&monster, &player, 100, 0, 0);
    });
    assert(g_last_damage == 100);
}

void test_max_hp_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    {
        Rates rates = default_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 50;
        apply_rates(rates);
    }

    Character& monster = g_characters[9];
    monster.type = 1;
    monster.slot = 20;
    monster.max_hp = 100;
    monster.hp = 120;
    int max_writes_before = g_max_hp_writes;
    int hp_writes_before = g_hp_writes;
    int attr_backups_before = g_attr_backup_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(g_attr_backup_calls == attr_backups_before + 1);
    assert(monster.max_hp == 50 && monster.hp == 50);
    assert(g_max_hp_writes == max_writes_before + 1);
    assert(g_hp_writes == hp_writes_before + 1);

    max_writes_before = g_max_hp_writes;
    hp_writes_before = g_hp_writes;
    monster.max_hp = 50;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 50);
    // 先把上次的 applied 还原成 raw，再在游戏结果上重新缩放：两次写入，数值不累积。
    assert(g_max_hp_writes == max_writes_before + 2);
    assert(g_hp_writes == hp_writes_before);

    monster.max_hp = 200;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 100);
    assert(g_max_hp_writes == max_writes_before + 3);

    {
        // 倍率变化在下一次属性重算时生效，且仍从游戏 raw 值重算。
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 200;
        apply_rates(rates);
    }
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 400);
    {
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 50;
        apply_rates(rates);
    }
    {
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 0;
        apply_rates(rates);
    }
    monster.max_hp = 100;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&monster, 0x1e); });
    assert(monster.max_hp == 1);
    {
        Rates rates = current_rates();
        rates.value[static_cast<uint32_t>(RateId::kEnemyHp)] = 50;
        apply_rates(rates);
    }

    Character& player_summon = g_characters[10];
    player_summon.type = 1;
    player_summon.party_member = 1;
    player_summon.slot = 21;
    player_summon.max_hp = 100;
    max_writes_before = g_max_hp_writes;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&player_summon, 0x1e); });
    assert(player_summon.max_hp == 100 && g_max_hp_writes == max_writes_before);

    Character& unknown = g_characters[11];
    unknown.type = 1;
    unknown.slot = 22;
    unknown.max_hp = 100;
    g_fail_party = true;
    max_writes_before = g_max_hp_writes;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&unknown, 0x1e); });
    assert(unknown.max_hp == 100 && g_max_hp_writes == max_writes_before);
    g_fail_party = false;

    Character& non_max_attr = g_characters[12];
    non_max_attr.type = 1;
    non_max_attr.slot = 23;
    non_max_attr.max_hp = 100;
    max_writes_before = g_max_hp_writes;
    const int pool_reads_before = g_pool_slot_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&non_max_attr, 0x1d); });
    assert(non_max_attr.max_hp == 100 && g_max_hp_writes == max_writes_before);
    assert(g_pool_slot_calls == pool_reads_before);

    Character& invalid_slot = g_characters[13];
    invalid_slot.type = 1;
    invalid_slot.slot = 100;
    invalid_slot.max_hp = 100;
    const int warnings_before = [&] {
        int count = 0;
        for (const auto& message : g_log_messages) {
            if (message.find("char not in pool; max-hp scaling skipped") != std::string::npos) ++count;
        }
        return count;
    }();
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&invalid_slot, 0x1e); });
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&invalid_slot, 0x1e); });
    const int warnings_after = [&] {
        int count = 0;
        for (const auto& message : g_log_messages) {
            if (message.find("char not in pool; max-hp scaling skipped") != std::string::npos) ++count;
        }
        return count;
    }();
    assert(invalid_slot.max_hp == 100 && warnings_after - warnings_before == 1);

    Character& disabled = g_characters[14];
    disabled.type = 1;
    disabled.slot = 24;
    disabled.max_hp = 100;
    descriptor.on_disabled(g_context);
    max_writes_before = g_max_hp_writes;
    const int pool_before_disabled = g_pool_slot_calls;
    attr_backups_before = g_attr_backup_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&disabled, 0x1e); });
    assert(g_attr_backup_calls == attr_backups_before + 1);
    assert(g_max_hp_writes == max_writes_before && g_pool_slot_calls == pool_before_disabled);

    descriptor.on_enabled(g_context);
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster, nullptr);
    attr_backups_before = g_attr_backup_calls;
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&disabled, 0x1e); });
    assert(g_attr_backup_calls == attr_backups_before);
    descriptor.on_resource_ready(g_context, kResourceIdUpdateAttrFromMonster,
                                 reinterpret_cast<void*>(&fake_attr_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddExperience,
                                 reinterpret_cast<void*>(&fake_exp_backup));
    descriptor.on_resource_ready(g_context, kResourceIdAddMoney,
                                 reinterpret_cast<void*>(&fake_money_backup));
    descriptor.on_resource_ready(g_context, kResourceIdDropItem,
                                 reinterpret_cast<void*>(&fake_drop_item_backup));
    descriptor.on_resource_ready(g_context, kResourceIdMakeItem,
                                 reinterpret_cast<void*>(&fake_make_item_backup));
    descriptor.on_resource_ready(g_context, kResourceIdMakeEquipDetail,
                                 reinterpret_cast<void*>(&fake_equip_detail_backup));
}

void test_deactivate_residual_wrappers() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    Character& attacker = g_characters[0];
    Character& victim = g_characters[1];
    attacker.party_member = 1;
    victim.type = 1;

    descriptor.deactivate(g_context);
    assert(!gate_enabled());
    assert(character_extras() == nullptr && character_facts() == nullptr);
    const int damage_before = g_damage_backup_calls;
    const int attr_before = g_attr_backup_calls;
    assert_one_damage_flight([&] { char_add_damage_wrapper(&attacker, &victim, 100, 0, 0); });
    assert_one_attr_flight([&] { char_update_attr_from_monster_wrapper(&victim, 0x1e); });
    assert(g_damage_backup_calls == damage_before + 1 && g_last_damage == 100);
    assert(g_attr_backup_calls == attr_before + 1);
    assert(g_max_hp_writes == 0 && g_hp_writes == 0);
}

void test_exp_and_money() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);

    Rates rates = default_rates();
    rates.value[static_cast<uint32_t>(RateId::kExp)] = 150;
    rates.value[static_cast<uint32_t>(RateId::kGold)] = 150;
    apply_rates(rates);

    Character player{};
    player.party_member = 1;
    Character monster{};
    monster.type = 1;

    assert(char_add_experience_wrapper(&player, 100, 1) == 1);
    assert(g_last_exp == 150);
    (void)char_add_experience_wrapper(&monster, 100, 1);
    assert(g_last_exp == 100);
    (void)char_add_experience_wrapper(&player, 3, 1);
    assert(g_last_exp == 4);
    (void)char_add_experience_wrapper(&player, 0, 1);
    assert(g_last_exp == 0);

    (void)inven_add_money_wrapper(100);
    assert(g_last_money == 150);
    (void)inven_add_money_wrapper(3);
    assert(g_last_money == 4);
    (void)inven_add_money_wrapper(-5);
    assert(g_last_money == -5);

    rates.value[static_cast<uint32_t>(RateId::kExp)] = 0;
    rates.value[static_cast<uint32_t>(RateId::kGold)] = 0;
    apply_rates(rates);
    (void)char_add_experience_wrapper(&player, 100, 1);
    assert(g_last_exp == 0);
    (void)inven_add_money_wrapper(100);
    assert(g_last_money == 0);

    descriptor.on_disabled(g_context);
    (void)char_add_experience_wrapper(&player, 100, 1);
    assert(g_last_exp == 100);
    (void)inven_add_money_wrapper(100);
    assert(g_last_money == 100);
}


void set_drop_rates(int32_t quantity, int32_t quality) {
    Rates rates = default_rates();
    rates.value[static_cast<uint32_t>(RateId::kDropQuantity)] = quantity;
    rates.value[static_cast<uint32_t>(RateId::kDropReroll)] = quality;
    apply_rates(rates);
}

void test_drop_quantity_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    g_drop_invoke = nullptr;
    assert(drop_threshold_patch_helper(500, 10000) == 500);

    set_drop_rates(1, 3);
    g_drop_threshold_result = 0;
    g_drop_invoke = [] { g_drop_threshold_result = drop_threshold_patch_helper(500, 10000); };
    drop_item_wrapper(nullptr);
    assert(g_drop_threshold_result == 4500);
    set_drop_rates(1, 1);
    g_drop_threshold_result = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_threshold_result == 500);
    descriptor.on_disabled(g_context);
    assert(drop_threshold_patch_helper(500, 10000) == 500);
    descriptor.on_enabled(g_context);
    g_drop_invoke = nullptr;

    set_drop_rates(3, 1);
    g_drop_item_calls = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_item_calls == 3);

    set_drop_rates(0, 1);
    g_drop_item_calls = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_item_calls == 0);

    set_drop_rates(3, 0);
    g_drop_item_calls = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_item_calls == 0);

    set_drop_rates(1, 1);
    g_drop_item_calls = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_item_calls == 1);

    const size_t logs_before_large = g_log_messages.size();
    set_drop_rates(17, 1);
    g_drop_item_calls = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_item_calls == 17);
    int large_warning_count = 0;
    for (size_t i = logs_before_large; i < g_log_messages.size(); ++i) {
        if (g_log_messages[i].find("drop quantity") != std::string::npos &&
            g_log_messages[i].find("17") != std::string::npos) ++large_warning_count;
    }
    assert(large_warning_count == 1);

    descriptor.on_disabled(g_context);
    g_drop_item_calls = 0;
    drop_item_wrapper(nullptr);
    assert(g_drop_item_calls == 1);
}

void test_make_item_quality_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    set_drop_rates(1, 3);

    // 首抽卷轴(Lv30)，重掷出宝石(Lv90)与卷轴(Lv60)：只保留同类别卷轴，取 Lv60。
    g_make_item_specs[0] = {24, 30, 0};
    g_make_item_specs[1] = {25, 90, 0};
    g_make_item_specs[2] = {24, 60, 0};
    g_make_item_spec_count = 3;
    g_make_item_spec_index = 0;
    g_make_item_calls = 0;
    g_free_item_calls = 0;
    g_drop_invoke = [] { g_last_item = static_cast<FakeDropItem*>(make_item_wrapper(0, 3, 0)); };
    drop_item_wrapper(nullptr);
    assert(g_last_item != nullptr && g_last_item->category == 24 && g_last_item->level == 60);
    assert(g_make_item_calls == 3);
    assert(g_free_item_calls == 2);

    // 药水不参与：只调用一次，不重掷。
    g_make_item_specs[0] = {22, 1, 0};
    g_make_item_specs[1] = {22, 99, 0};
    g_make_item_spec_count = 2;
    g_make_item_spec_index = 0;
    g_make_item_calls = 0;
    g_free_item_calls = 0;
    g_drop_invoke = [] { g_last_item = static_cast<FakeDropItem*>(make_item_wrapper(0, 2, 0)); };
    drop_item_wrapper(nullptr);
    assert(g_last_item != nullptr && g_last_item->category == 22 && g_last_item->level == 1);
    assert(g_make_item_calls == 1 && g_free_item_calls == 0);

    // 背包参与：同类别取 Lv35（重抽档 2 = 首抽 + 1 次重掷）。
    set_drop_rates(1, 2);
    g_make_item_specs[0] = {31, 15, 0};
    g_make_item_specs[1] = {31, 35, 0};
    g_make_item_spec_count = 2;
    g_make_item_spec_index = 0;
    g_make_item_calls = 0;
    g_free_item_calls = 0;
    g_drop_invoke = [] { g_last_item = static_cast<FakeDropItem*>(make_item_wrapper(0, 5, 0)); };
    drop_item_wrapper(nullptr);
    assert(g_last_item != nullptr && g_last_item->category == 31 && g_last_item->level == 35);
    assert(g_make_item_calls == 2 && g_free_item_calls == 1);

    // 掉落上下文外（商店/合成）不重掷。
    g_make_item_specs[0] = {24, 30, 0};
    g_make_item_spec_count = 1;
    g_make_item_spec_index = 0;
    g_make_item_calls = 0;
    g_drop_invoke = nullptr;
    (void)make_item_wrapper(0, 3, 0);
    assert(g_make_item_calls == 1);
}

void test_make_equip_detail_quality_wrapper() {
    install_success();
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    set_drop_rates(1, 3);

    // 稀有度 0 -> 2 -> 1：取 rarity 2，释放另外两件。
    g_equip_specs[0] = {241, 2, 0};
    g_equip_specs[1] = {390, 3, 2};
    g_equip_specs[2] = {408, 1, 1};
    g_equip_spec_count = 3;
    g_equip_spec_index = 0;
    g_equip_detail_calls = 0;
    g_free_item_calls = 0;
    g_drop_invoke = [] {
        g_last_item = static_cast<FakeDropItem*>(make_equip_detail_wrapper(0, 1, 5, -1, 0));
    };
    drop_item_wrapper(nullptr);
    assert(g_last_item != nullptr && g_last_item->rarity == 2);
    assert(g_equip_detail_calls == 3 && g_free_item_calls == 2);

    // 掉落上下文外不重掷。
    g_equip_specs[0] = {241, 2, 0};
    g_equip_spec_count = 1;
    g_equip_spec_index = 0;
    g_equip_detail_calls = 0;
    g_drop_invoke = nullptr;
    (void)make_equip_detail_wrapper(0, 1, 5, -1, 0);
    assert(g_equip_detail_calls == 1);
}

void test_capability_event_subscription() {
    const auto& descriptor = plugin_descriptor();
    install_success();

    // a. install 订阅能力变更事件。
    assert(g_event_subscribe_calls == 1);
    assert(g_event_subscriptions.size() == 1);
    assert(g_event_subscriptions[0].type == kEventCapabilityChanged);

    // 禁用状态（install 后 gate=false）：available=1 不绑定 item.use / 不注册面板。
    g_item_use_available = true;
    const CapabilityChangedEvent available_event{kItemUseCapabilityId, kItemUseVersion.major,
                                                 kItemUseVersion.minor, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &available_event);
    assert(g_item_use_subscribe_calls == 0);
    assert(!item_use_bound());
    g_ui_available = true;
    const CapabilityChangedEvent panels_available_disabled{
        kUiPanelsCapabilityId, static_cast<uint16_t>(kUiPanelsVersionMajor),
        static_cast<uint16_t>(kUiPanelsVersionMinor), 1, {}};
    publish_fake_event(kEventCapabilityChanged, &panels_available_disabled);
    assert(g_panel_register_calls == 0);
    assert(!panel_available());

    // 重新启用：初始探测补齐 item.use 与面板绑定。
    descriptor.on_enabled(g_context);
    assert(g_item_use_subscribe_calls == 1);
    assert(item_use_bound());
    assert(g_panel_register_calls == 1);
    assert(panel_available());

    // b. item.use 注册后重复事件保持幂等。
    publish_fake_event(kEventCapabilityChanged, &available_event);
    assert(g_item_use_subscribe_calls == 1);
    assert(item_use_bound());

    // 无关能力的事件不触发 item.use 绑定。
    const CapabilityChangedEvent other_event{"character.facts", 1, 0, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &other_event);
    assert(g_item_use_subscribe_calls == 1);

    // 空 payload / 空 id 直接返回。
    publish_fake_event(kEventCapabilityChanged, nullptr);
    const CapabilityChangedEvent null_id_event{nullptr, 1, 0, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &null_id_event);
    assert(g_item_use_subscribe_calls == 1);

    // c. 注销事件后解绑：unsubscribe_post 被调用且 item_use_bound() 为 false。
    g_item_use_available = false;
    const CapabilityChangedEvent unavailable_event{kItemUseCapabilityId, kItemUseVersion.major,
                                                   kItemUseVersion.minor, 0, {}};
    publish_fake_event(kEventCapabilityChanged, &unavailable_event);
    assert(g_item_use_unsubscribe_calls == 1);
    assert(!item_use_bound());

    // d. ui 能力晚到：available=1 时补注册面板 provider。
    g_ui_available = true;
    const CapabilityChangedEvent panels_available{
        kUiPanelsCapabilityId, static_cast<uint16_t>(kUiPanelsVersionMajor),
        static_cast<uint16_t>(kUiPanelsVersionMinor), 1, {}};
    publish_fake_event(kEventCapabilityChanged, &panels_available);
    assert(g_panel_register_calls == 1);
    assert(g_registered_panel != nullptr);
    assert(panel_available());

    // 另一个 UI 能力也注册时保持幂等：已注册则不再重复注册。
    const CapabilityChangedEvent components_available{kUiComponentsCapabilityId, kUiContractMajor,
                                                      kUiContractMinor, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &components_available);
    assert(g_panel_register_calls == 1);
    assert(panel_available());

    // e. 任一 UI 能力撤销：注销 provider 且绑定失效。
    const CapabilityChangedEvent components_unavailable{kUiComponentsCapabilityId, kUiContractMajor,
                                                        kUiContractMinor, 0, {}};
    publish_fake_event(kEventCapabilityChanged, &components_unavailable);
    assert(g_panel_unregister_calls == 1);
    assert(g_registered_panel == nullptr);
    assert(!panel_available());

    // f. 再次注册后重新注册 provider（ui.panels 分支）。
    publish_fake_event(kEventCapabilityChanged, &panels_available);
    assert(g_panel_register_calls == 2);
    assert(panel_available());

    // ui.panels 撤销同样注销。
    const CapabilityChangedEvent panels_unavailable{
        kUiPanelsCapabilityId, static_cast<uint16_t>(kUiPanelsVersionMajor),
        static_cast<uint16_t>(kUiPanelsVersionMinor), 0, {}};
    publish_fake_event(kEventCapabilityChanged, &panels_unavailable);
    assert(g_panel_unregister_calls == 2);
    assert(!panel_available());

    // ui.components 分支也可单独触发重新注册。
    publish_fake_event(kEventCapabilityChanged, &components_available);
    assert(g_panel_register_calls == 3);
    assert(panel_available());

    // g. 无关能力与空 payload/空 id 不影响面板注册状态。
    const int register_before_unrelated = g_panel_register_calls;
    const int unregister_before_unrelated = g_panel_unregister_calls;
    const CapabilityChangedEvent unrelated_event{"character.facts", 1, 0, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &unrelated_event);
    publish_fake_event(kEventCapabilityChanged, nullptr);
    const CapabilityChangedEvent null_ui_event{nullptr, 1, 0, 1, {}};
    publish_fake_event(kEventCapabilityChanged, &null_ui_event);
    assert(g_panel_register_calls == register_before_unrelated);
    assert(g_panel_unregister_calls == unregister_before_unrelated);
    assert(panel_available());

    // h. 禁用期间：available=0 仍注销，available=1 不重新挂载；重新启用后恢复绑定。
    descriptor.on_disabled(g_context);
    const int unregisters_before_disabled_events = g_panel_unregister_calls;
    const int registers_before_disabled_events = g_panel_register_calls;
    const int subscribes_before_disabled_events = g_item_use_subscribe_calls;
    publish_fake_event(kEventCapabilityChanged, &panels_unavailable);
    assert(g_panel_unregister_calls == unregisters_before_disabled_events + 1);
    assert(!panel_available());
    publish_fake_event(kEventCapabilityChanged, &panels_available);
    assert(g_panel_register_calls == registers_before_disabled_events);
    assert(!panel_available());
    g_item_use_available = true;
    publish_fake_event(kEventCapabilityChanged, &available_event);
    assert(g_item_use_subscribe_calls == subscribes_before_disabled_events);
    assert(!item_use_bound());
    descriptor.on_enabled(g_context);
    assert(g_panel_register_calls == registers_before_disabled_events + 1);
    assert(panel_available());
    assert(g_item_use_subscribe_calls == subscribes_before_disabled_events + 1);
    assert(item_use_bound());

    // deactivate 取消事件订阅并注销面板。
    const int unsubscribes_before_deactivate = g_event_unsubscribe_calls;
    const int unregisters_before_deactivate = g_panel_unregister_calls;
    descriptor.deactivate(g_context);
    assert(g_event_unsubscribe_calls == unsubscribes_before_deactivate + 1);
    assert(g_event_subscriptions.empty());
    assert(g_panel_unregister_calls == unregisters_before_deactivate + 1);
    assert(!panel_available());
}

void test_panel_contract() {
    const auto& provider = panel_provider();
    assert(std::strcmp(provider.panel_id, "world_tuning") == 0);
    assert(std::strcmp(provider.owner, "world_tuning") == 0);
    assert(provider.callbacks != nullptr);
    assert(!panel_available());
    install_success();
    assert(!panel_provider().open());  // 门禁关闭时拒绝打开
    const auto& descriptor = plugin_descriptor();
    descriptor.on_enabled(g_context);
    assert(!panel_provider().open());  // 未绑定 UI 时不崩溃且拒绝打开
    descriptor.deactivate(g_context);
}

}  // namespace

int main() {
    test_descriptor();
    test_install_lifecycle_and_toggle();
    test_damage_wrapper();
    test_max_hp_wrapper();
    test_deactivate_residual_wrappers();
    test_exp_and_money();
    test_drop_quantity_wrapper();
    test_make_item_quality_wrapper();
    test_make_equip_detail_quality_wrapper();
    test_capability_event_subscription();
    test_panel_contract();
    assert(g_in_flight_enters == g_in_flight_leaves);
    return 0;
}
