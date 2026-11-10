// core.feature 暴击特效屏蔽 wrapper 回归：
// 只拦截暴击分支的 0x5a 闪白、0x1f..0x22 横向打击条及其同帧震屏/震动，
// 其他特效与反馈透传。

#include "plugins/core.feature/plugin.cpp"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int32_t add_act_calls = 0;
int32_t last_effect_id = -1;
int32_t fake_add_act_effect(void*, void*, int32_t effect_id) {
    ++add_act_calls;
    last_effect_id = effect_id;
    return 1;
}

int32_t shake_calls = 0;
int32_t last_shake_type = -1;
void fake_map_set_shake(int32_t type) {
    ++shake_calls;
    last_shake_type = type;
}

int32_t vibration_calls = 0;
int32_t last_vibration_type = -1;
void fake_vibration_play(int32_t type) {
    ++vibration_calls;
    last_vibration_type = type;
}

int64_t fake_frame = 100;
int64_t fake_frame_count() { return fake_frame; }

void reset_counters() {
    add_act_calls = 0;
    last_effect_id = -1;
    shake_calls = 0;
    last_shake_type = -1;
    vibration_calls = 0;
    last_vibration_type = -1;
}

}  // namespace

int main() {
    using namespace qol::features::core_feature;

    qol::contracts::PluginContext context{};
    context.plugin_id = "core.feature";

    g_add_act_effect_backup.store(&fake_add_act_effect);
    g_map_set_shake_backup.store(&fake_map_set_shake);
    g_vibration_play_backup.store(&fake_vibration_play);
    g_frame_count.store(&fake_frame_count);
    g_mask_crit_effects.store(true);

    // 默认开启：0x5a 被拦截，同帧震屏/震动也被拦截，原函数不调用。
    reset_counters();
    g_crit_feedback_pending.store(false);
    g_crit_feedback_frame.store(-1);
    check(add_act_effect_wrapper(nullptr, nullptr, 0x5a) == 0, "flash suppressed");
    check(add_act_calls == 0, "flash does not reach backup");
    map_set_shake_wrapper(1);
    vibration_play_wrapper(3);
    check(shake_calls == 0 && vibration_calls == 0, "crit shake/vibration suppressed");
    check(!g_crit_feedback_pending.load(), "pending cleared after vibration");

    // 横向打击条 0x1f..0x22 也被拦截。
    reset_counters();
    for (int32_t id = 0x1f; id <= 0x22; ++id) {
        check(add_act_effect_wrapper(nullptr, nullptr, id) == 0, "crit bar suppressed");
    }
    check(add_act_calls == 0, "crit bar does not reach backup");

    // 暴击光效与其他特效透传。
    reset_counters();
    check(add_act_effect_wrapper(nullptr, nullptr, 0x52) == 1, "crit spark passes through");
    check(add_act_effect_wrapper(nullptr, nullptr, 0x23) == 1, "non-crit effect passes through");
    check(add_act_calls == 2 && last_effect_id == 0x23, "backup receives pass-through effects");
    map_set_shake_wrapper(1);
    vibration_play_wrapper(3);
    check(shake_calls == 1 && vibration_calls == 1, "non-crit feedback passes through");

    // 关闭功能：0x5a 也透传。
    reset_counters();
    check(qol::contracts::succeeded(kFeatures[1].on_toggle(context, 0)), "toggle off");
    check(add_act_effect_wrapper(nullptr, nullptr, 0x5a) == 1, "flash passes through when disabled");
    check(add_act_calls == 1 && last_effect_id == 0x5a, "backup receives flash when disabled");
    map_set_shake_wrapper(1);
    vibration_play_wrapper(3);
    check(shake_calls == 1 && vibration_calls == 1, "feedback passes through when disabled");

    // 重新开启：同帧保护，下一帧不再拦截。
    reset_counters();
    check(qol::contracts::succeeded(kFeatures[1].on_toggle(context, 1)), "toggle on");
    check(add_act_effect_wrapper(nullptr, nullptr, 0x5a) == 0, "flash suppressed again");
    fake_frame += 1;
    map_set_shake_wrapper(1);
    vibration_play_wrapper(3);
    check(shake_calls == 1 && vibration_calls == 1, "next frame feedback passes through");

    if (failures == 0) std::printf("core_feature_crit_fx: pass\n");
    return failures == 0 ? 0 : 1;
}
