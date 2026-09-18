#include <cassert>
#include <cstdint>

#include "core/native/save_enter.h"
#include "infra/runtime.h"
#include "infra/scheduler.h"
#include "plugins/quest/feature.h"
#include "plugins/quest/plugin.h"

namespace {

int64_t g_frame = 1;
bool g_in_world = true;
bool g_save_enter_registered = false;
int g_set_state_calls = 0;

int64_t fake_frame_count() {
    return g_frame;
}

}  // namespace

bool game_in_world() {
    return g_in_world;
}

bool save_enter_register(SaveEnterFn fn, void* ctx) {
    g_save_enter_registered = fn != nullptr && ctx == nullptr;
    return g_save_enter_registered;
}

void save_enter_unregister(SaveEnterFn fn, void* ctx) {
    if (fn != nullptr && ctx == nullptr) g_save_enter_registered = false;
}

void save_enter_mark_pending() {}
bool save_enter_init() { return true; }
bool save_enter_host_install_if_ready() { return true; }

namespace qol::game {

uint32_t quest_active_slot_count() { return 1; }

bool quest_active_slot_id(uint32_t index, uint16_t* out_id) {
    if (index != 0 || out_id == nullptr) return false;
    *out_id = 7;
    return true;
}

uint16_t quest_total_count() { return 1; }
int32_t quest_state(uint16_t quest_id) { return quest_id == 7 ? 1 : -1; }
bool quest_set_state(uint16_t quest_id, int32_t state) {
    if (quest_id != 7 || state != 2) return false;
    ++g_set_state_calls;
    return true;
}
bool quest_is_complete(uint16_t quest_id) { return quest_id == 7; }

}  // namespace qol::game

int main() {
    qol::infra::runtime_init({&fake_frame_count});
    qol::infra::scheduler_reset_for_test();
    qol::contracts::PluginContext ctx;
    const auto& descriptor = qol::features::quest::plugin_descriptor();

    assert(descriptor.install(ctx) == 0);
    assert(g_save_enter_registered);
    qol::features::quest::on_save_enter(nullptr);
    qol::features::quest::on_save_enter(nullptr);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 1);

    g_frame = 2;
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    assert(g_set_state_calls == 1);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 0);

    qol::features::quest::on_save_enter(nullptr);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 1);
    qol::features::quest::set_gate(false);
    g_frame = 3;
    qol::infra::scheduler_dispatch(qol::infra::FramePoint::kLogicPre);
    assert(g_set_state_calls == 1);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 0);

    descriptor.on_enabled(ctx);
    qol::features::quest::on_save_enter(nullptr);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 1);
    descriptor.on_disabled(ctx);
    assert(!qol::features::quest::gate_enabled());
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 0);

    descriptor.on_enabled(ctx);
    qol::features::quest::on_save_enter(nullptr);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 1);
    descriptor.deactivate(ctx);
    assert(!qol::features::quest::gate_enabled());
    assert(!g_save_enter_registered);
    assert(qol::infra::scheduler_task_count(qol::infra::FramePoint::kLogicPre) == 0);
    return 0;
}
