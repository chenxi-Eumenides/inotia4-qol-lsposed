#include <cassert>
#include <cstdint>
#include <cstring>

#include "contracts/game_host.h"
#include "contracts/game_ports/quest_port.h"
#include "contracts/host_sdk.h"
#include "contracts/world_facts.h"
#include "infra/runtime.h"
#include "infra/commands.h"
#include "infra/scheduler.h"
#include "plugins/quest/feature.h"
#include "plugins/quest/plugin.h"

namespace {

int64_t g_frame = 1;
bool g_in_world = false;
int g_set_state_calls = 0;
qol::contracts::QolFrameHandler g_scheduler_handler = nullptr;

int64_t fake_frame_count() {
    return g_frame;
}

bool scheduler_bridge(int64_t frame, void* ctx) {
    return g_scheduler_handler != nullptr && g_scheduler_handler(frame, ctx) != 0;
}

QolResult fake_scheduler_add(int32_t point,
                                             qol::contracts::QolFrameHandler handler,
                                             void* ctx, int32_t interval, int32_t count,
                                             uint64_t* out_id) {
    g_scheduler_handler = handler;
    const auto id = qol::infra::scheduler_add(
        static_cast<qol::infra::FramePoint>(point), &scheduler_bridge, ctx, interval, count);
    if (out_id == nullptr || id == 0) return qol::contracts::error(1, "scheduler failed", 16);
    *out_id = id;
    return qol::contracts::ok();
}

QolResult fake_scheduler_remove(uint64_t id) {
    return qol::infra::scheduler_remove(id) ? qol::contracts::ok()
                                             : qol::contracts::error(1, "remove failed", 13);
}

QolResult fake_world_state(int32_t* out) {
    if (out == nullptr) return qol::contracts::error(1, "invalid", 7);
    *out = g_in_world ? 5 : 4;
    return qol::contracts::ok();
}

QolResult fake_capability_require(const char* id, QolVersionRange, const void** out_iface) {
    static const qol::contracts::WorldFactsApi api{&fake_world_state, nullptr, nullptr, nullptr,
                                                    nullptr, nullptr, nullptr, nullptr};
    if (id == nullptr || out_iface == nullptr ||
        std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) != 0) {
        return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
    }
    *out_iface = &api;
    return qol::contracts::ok();
}

QolResult fake_active_slot_count(uint32_t* out) {
    *out = 1;
    return qol::contracts::ok();
}

QolResult fake_active_slot_id(uint32_t index, uint32_t* out) {
    if (index != 0 || out == nullptr) return qol::contracts::error(1, "invalid", 7);
    *out = 7;
    return qol::contracts::ok();
}

QolResult fake_quest_state(uint32_t quest_id, int32_t* out) {
    if (quest_id != 7 || out == nullptr) return qol::contracts::error(1, "invalid", 7);
    *out = 1;
    return qol::contracts::ok();
}

QolResult fake_quest_complete(uint32_t quest_id, int32_t* out) {
    if (quest_id != 7 || out == nullptr) return qol::contracts::error(1, "invalid", 7);
    *out = 1;
    return qol::contracts::ok();
}

QolResult fake_set_state(uint32_t quest_id, int32_t state) {
    if (quest_id != 7 || state != 2) return qol::contracts::error(1, "invalid", 7);
    ++g_set_state_calls;
    return qol::contracts::ok();
}

qol::contracts::QolHostSdk fake_sdk() {
    qol::contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(qol::contracts::QolHostSdk), qol::contracts::kHostSdkMajor,
               qol::contracts::kHostSdkMinor};
    sdk.scheduler = {&fake_scheduler_add, &fake_scheduler_remove};
    sdk.capabilities.require = &fake_capability_require;
    sdk.commands = qol::infra::commands_port();
    return sdk;
}

const qol::contracts::QolQuestPort kQuestPort{
    {1, 0},
    {&fake_world_state, &fake_active_slot_count, &fake_active_slot_id, nullptr, &fake_quest_state,
     &fake_quest_complete},
    {&fake_set_state},
};

qol::contracts::QolGameHost fake_game() {
    qol::contracts::QolGameHost game{};
    game.abi = {sizeof(qol::contracts::QolGameHost), qol::contracts::kGameHostMajor,
                qol::contracts::kGameHostMinor};
    return game;
}

}  // namespace

int main() {
    qol::infra::runtime_init({&fake_frame_count});
    qol::infra::scheduler_reset_for_test();
    qol::infra::commands_reset_for_test();
    auto sdk = fake_sdk();
    auto game = fake_game();
    const void* ports[] = {&kQuestPort};
    qol::contracts::PluginContext ctx{sizeof(qol::contracts::PluginContext),
                                      qol::contracts::kHostSdkMajor,
                                      qol::contracts::kHostSdkMinor, &sdk, &game, "quest", ports, 1};
    const auto& descriptor = qol::features::quest::plugin_descriptor();

    assert(descriptor.install(ctx) == 0);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 1);

    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    qol::infra::commands_drain();
    assert(g_set_state_calls == 0);
    g_in_world = true;
    g_frame = 2;
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    qol::infra::commands_drain();
    assert(g_set_state_calls == 1);
    g_frame = 3;
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    qol::infra::commands_drain();
    assert(g_set_state_calls == 1);

    qol::features::quest::on_save_enter(nullptr);
    g_frame = 4;
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    qol::infra::commands_drain();
    assert(g_set_state_calls == 2);

    descriptor.on_disabled(ctx);
    assert(!qol::features::quest::gate_enabled());
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 0);
    descriptor.on_enabled(ctx);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 1);
    descriptor.deactivate(ctx);
    assert(!qol::features::quest::gate_enabled());
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 0);
    return 0;
}
