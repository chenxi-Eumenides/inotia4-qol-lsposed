#include <cassert>
#include <cstddef>
#include <cstdint>

#include "contracts/panels.h"
#include "core/native/game_variant.h"
#include "game/access/attr_range_ui.h"
#include "game/access/char.h"
#include "game/access/craft.h"
#include "game/access/quest.h"
#include "game/access/save.h"
#include "game/access/symbols.h"
#include "game/access/world.h"
#include "game/clock.h"
#include "host/adapters.h"
#include "infra/capabilities.h"
#include "infra/events.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "infra/text_scope.h"

uintptr_t g_base = 0;

uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

bool game_memory_accessible(const void*, size_t, char) { return false; }

namespace qol {
namespace {
GameVariant g_test_variant{GameSeries::kOriginal, "test", kCapWarehouse, {}, true};
}

bool game_variant_init() { return true; }
const GameVariant& game_variant() { return g_test_variant; }
}  // namespace qol

namespace qol::game {

int char_type(const void*) { return 2; }
bool char_active_group_resolved() { return true; }
bool char_party_index_resolved() { return true; }
bool char_is_active_player_group(void*) { return true; }
int char_party_index(const void*) { return 0; }
int attr_item_category(const void*) { return 4; }
bool attr_is_jewel_category(int) { return true; }
int attr_ability_level(const void*) { return 3; }
bool attr_probe_option_range(int, int, int, void*, int* out_min, int* out_max) {
    *out_min = 1;
    *out_max = 4;
    return true;
}
bool attr_probe_jewel_range(int, void*, int* out_min, int* out_max) {
    *out_min = 2;
    *out_max = 8;
    return true;
}
int64_t craft_ui_type() { return 1; }
uint8_t craft_panel_state() { return 2; }
uint32_t craft_mix_type() { return 3; }
int64_t craft_selected_stuff_index() { return 0; }
void* craft_stuff_item(size_t) { return reinterpret_cast<void*>(0x1); }
int craft_category_held_count(uint16_t) { return 5; }
int craft_character_level() { return 10; }
void craft_write_stuff_need(size_t, uint16_t) {}
void craft_set_stuff_item(size_t, void*) {}
void craft_set_target_slot_item(void*) {}
void craft_set_cost_zero() {}
void* save_slot_struct(int32_t) { return reinterpret_cast<void*>(0x1); }
bool save_slot_exists(const void*) { return true; }
int world_state() { return 7; }
int64_t frame_count() { return 99; }

}  // namespace qol::game

namespace {

int64_t g_frame = 1;
int g_event_calls = 0;

int64_t fake_frame_count() { return g_frame; }

void event_handler(void*, const void*) { ++g_event_calls; }

int32_t scheduler_handler(int64_t frame, void* ctx) {
    assert(frame == g_frame);
    ++*static_cast<int*>(ctx);
    return 0;
}

bool panel_open() { return true; }
void panel_close() {}
bool panel_active() { return true; }
const char* panel_block_reason() { return nullptr; }

}  // namespace

int main() {
    const qol::contracts::QolHostSdk& sdk = qol::host::g_host_sdk;
    assert(sdk.abi.size == sizeof(qol::contracts::QolHostSdk));
    assert(sdk.abi.major == qol::contracts::kHostSdkMajor);
    assert(sdk.ui == nullptr);
    assert(sdk.config.get_bool != nullptr);
    assert(sdk.scheduler.add != nullptr);
    assert(sdk.events.subscribe != nullptr);

    int32_t value = 0;
    assert(sdk.config.set_bool("adapter.test", 1).ok == kQolOk);
    assert(sdk.config.get_bool("adapter.test", 0, &value).ok == kQolOk);
    assert(value == 1);

    uint64_t subscription = 0;
    assert(sdk.events.subscribe(1, &event_handler, nullptr, &subscription).ok == kQolOk);
    assert(subscription != 0);
    assert(qol::infra::publish(1, nullptr) == 1);
    assert(g_event_calls == 1);
    assert(sdk.events.unsubscribe(subscription).ok == kQolOk);

    static int capability = 7;
    assert(qol::infra::register_capability(
               qol::infra::CapabilityProvision{"adapter.capability", QolVersion{1, 0},
                                                &capability}));
    const void* iface = nullptr;
    assert(sdk.capabilities.require("adapter.capability", QolVersionRange{1, 0}, &iface).ok ==
           kQolOk);
    assert(iface == &capability);
    uint64_t lease = 0;
    assert(sdk.capabilities.lease_enter("adapter.capability", QolVersionRange{1, 0}, &lease).ok ==
           kQolOk);
    assert(sdk.capabilities.lease_leave(lease).ok == kQolOk);
    assert(qol::infra::unregister_capability("adapter.capability"));

    qol::infra::runtime_init(qol::infra::Clock{&fake_frame_count});
    int scheduler_calls = 0;
    uint64_t task = 0;
    assert(sdk.scheduler.add(0, &scheduler_handler, &scheduler_calls, 1, 1, &task).ok == kQolOk);
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kRenderPre);
    assert(scheduler_calls == 1);
    assert(sdk.scheduler.remove(task).ok != kQolOk);

    assert(sdk.text_scope.push(static_cast<int32_t>(qol::infra::TextScope::kCharacterPanel)).ok ==
           kQolOk);
    assert(qol::infra::active_text_scope() == qol::infra::TextScope::kCharacterPanel);
    assert(sdk.text_scope.pop().ok == kQolOk);
    assert(qol::infra::active_text_scope() == qol::infra::TextScope::kNone);

    static const qol::contracts::PanelProviderSpec provider{
        "adapter.panel", "adapter.test", &panel_open, &panel_close, &panel_active,
        &panel_block_reason};
    assert(sdk.panels.register_provider(&provider).ok == kQolOk);
    int32_t active = 0;
    assert(sdk.panels.open("adapter.panel").ok == kQolOk);
    assert(sdk.panels.active("adapter.panel", &active).ok == kQolOk);
    assert(active == 1);
    assert(sdk.panels.close("adapter.panel").ok == kQolOk);
    assert(sdk.panels.unregister_owner("adapter.test").ok == kQolOk);

    const qol::contracts::QolGameHost& game = qol::host::g_game_host;
    assert(game.abi.size == sizeof(qol::contracts::QolGameHost));
    assert(game.target.query_variant != nullptr);
    qol::contracts::QolTargetVariant variant{};
    assert(game.target.query_variant(&variant).ok == kQolOk);
    assert(variant.id == static_cast<uint32_t>(qol::GameSeries::kOriginal));
    assert(variant.capabilities == qol::kCapWarehouse);
    qol::host::game_host_mark_main_thread();
    assert(game.thread.is_main_thread() == 1);
    assert(game.thread.frame_count() == 99);
    int32_t character_type = -1;
    assert(game.read.character.type(reinterpret_cast<void*>(0x1), &character_type).ok == kQolOk);
    assert(character_type == 2);
    assert(game.call.call_known(1, nullptr, 0, nullptr, 0).ok == kQolErrUnsupported);
    return 0;
}
